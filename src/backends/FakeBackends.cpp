// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeBackends.h"

#include "V4l2Logic.h"
#include "engine/Clock.h"

#include <linux/videodev2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace pd {

FakeVideoBackend::FakeVideoBackend(FakeVideoOptions o) : m_opt(o) {}

bool FakeVideoBackend::setInput(int index, std::string&) {
    m_input = index;
    return true;
}

std::vector<VideoStandardInfo> FakeVideoBackend::standards() {
    return {{V4L2_STD_NTSC_M, "NTSC-M", {30000, 1001}, true}, {V4L2_STD_PAL_BG, "PAL-B/G", {25, 1}, false}};
}

uint64_t FakeVideoBackend::currentStandard() { return m_opt.ntsc ? V4L2_STD_NTSC_M : V4L2_STD_PAL_BG; }

bool FakeVideoBackend::setStandard(uint64_t id, std::string&) {
    m_opt.ntsc = (id & V4L2_STD_525_60) != 0;
    return true;
}

bool FakeVideoBackend::configure(PixFmt pixfmt, VideoFormat& out, std::string& err) {
    if (pixfmt != PixFmt::YUYV) {
        err = "test pattern is YUYV only";
        return false;
    }
    m_fmt = VideoFormat{};
    m_fmt.pixfmt = PixFmt::YUYV;
    m_fmt.width = 720;
    m_fmt.height = m_opt.ntsc ? 480 : 576;
    m_fmt.bytesPerLine = 1440;
    m_fmt.frameBytes = size_t(1440) * size_t(m_fmt.height);
    m_fmt.fps = m_opt.ntsc ? Rational{30000, 1001} : Rational{25, 1};
    m_fmt.field = m_opt.ntsc ? FieldOrder::BFF : FieldOrder::TFF;
    m_fmt.lines525 = m_opt.ntsc;
    m_fmt.sar = sarFor(720, m_opt.ntsc);
    m_fmt.standardName = m_opt.ntsc ? "NTSC-M" : "PAL-B/G";
    out = m_fmt;
    return true;
}

std::vector<VideoControl> FakeVideoBackend::controls() {
    VideoControl b;
    b.id = V4L2_CID_BRIGHTNESS;
    b.name = "Brightness";
    b.minimum = -64;
    b.maximum = 64;
    b.value = m_brightness;
    return {b};
}

bool FakeVideoBackend::setControl(uint32_t id, int64_t value, std::string& err) {
    if (id != V4L2_CID_BRIGHTNESS) {
        err = "unknown control";
        return false;
    }
    m_brightness = int(value);
    return true;
}

SignalStatus FakeVideoBackend::signalStatus() {
    SignalStatus s;
    s.known = true;
    s.noSignal = m_inLoss.load();
    s.detected = m_fmt.standardName;
    return s;
}

bool FakeVideoBackend::start(int, std::string&) {
    m_running = true;
    m_start = monotonicNs();
    m_next = 0;
    return true;
}

void FakeVideoBackend::render(uint8_t* dst, uint64_t frame) {
    // 75% color bars in BT.601 YUV, a moving white block so motion is
    // visible, and the frame number in binary along the bottom.
    static const uint8_t bars[8][3] = {{180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
                                       {84, 184, 198},  {65, 100, 212}, {35, 212, 114}, {16, 128, 128}};
    const int w = m_fmt.width, h = m_fmt.height;
    const int blockX = int((frame * 6) % uint64_t(w - 48));
    for (int y = 0; y < h; ++y) {
        uint8_t* row = dst + size_t(y) * size_t(m_fmt.bytesPerLine);
        for (int x = 0; x < w; x += 2) {
            uint8_t Y, U, V;
            if (m_input == 1) {
                Y = uint8_t(16 + (219 * x) / w);
                U = V = 128;
            } else {
                const int b = (x * 8) / w;
                Y = bars[b][0];
                U = bars[b][1];
                V = bars[b][2];
            }
            if (y > h / 2 - 24 && y < h / 2 + 24 && x >= blockX && x < blockX + 48) {
                Y = 235;
                U = V = 128;
            }
            if (y >= h - 16) {
                const int bit = x / 16;
                Y = (bit < 32 && ((frame >> bit) & 1)) ? 235 : 16;
                U = V = 128;
            }
            const int yv = std::clamp(int(Y) + m_brightness, 16, 235);
            row[2 * x + 0] = uint8_t(yv);
            row[2 * x + 1] = U;
            row[2 * x + 2] = uint8_t(yv);
            row[2 * x + 3] = V;
        }
    }
}

int FakeVideoBackend::dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) {
    if (!m_running) {
        err = "not streaming";
        return -1;
    }
    const double interval = 1e9 * m_fmt.fps.den / m_fmt.fps.num;
    while (true) {
        const int64_t due = m_start + int64_t(std::llround(double(m_next) * interval));
        const int64_t now = monotonicNs();
        if (due > now) {
            const int64_t wait = std::min<int64_t>(due - now, int64_t(timeoutMs) * 1000000LL);
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
            if (monotonicNs() < due) return 0;
        }
        const double t = double(m_next) * interval / 1e9;
        const uint64_t idx = m_next++;
        const bool lost = m_opt.lossAtSec >= 0 && t >= m_opt.lossAtSec && t < m_opt.lossAtSec + m_opt.lossLenSec;
        m_inLoss = lost;
        if (lost) return 0;  // driver delivers nothing (poll times out), sequence does not advance
        if (m_opt.dropEverySec > 0 && idx > 0 && std::fmod(t, m_opt.dropEverySec) < interval / 1e9 && t > 0.5) {
            ++m_seq;  // driver dropped it
            ++m_skipped;
            continue;
        }
        out.data = pool.acquire(m_fmt.frameBytes);
        render(out.data->data(), idx);
        out.size = m_fmt.frameBytes;
        out.tsNs = due;
        out.seq = m_seq++;
        out.flags = 0;
        ++m_delivered;
        return 1;
    }
}

// ---------------------------------------------------------------------------

bool FakeAudioBackend::open(int, AudioFormat& out, std::string&) {
    out.rate = m_opt.rate;
    out.channels = m_opt.channels;
    out.fmt = m_opt.fmt;
    return true;
}

bool FakeAudioBackend::start(std::string&) {
    m_running = true;
    m_start = monotonicNs();
    m_pos = 0;
    m_nextXrun = m_opt.xrunEverySec;
    return true;
}

int FakeAudioBackend::read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) {
    xrun = false;
    if (!m_running) {
        err = "not running";
        return -1;
    }
    const double trueRate = m_opt.rate * (1.0 + m_opt.ppm * 1e-6);
    const uint32_t chunk = uint32_t(m_opt.rate / 100);
    const int64_t due = m_start + int64_t(double(m_pos + chunk) * 1e9 / trueRate);
    const int64_t now = monotonicNs();
    if (due > now) {
        const int64_t wait = std::min<int64_t>(due - now, int64_t(timeoutMs) * 1000000LL);
        std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
        if (monotonicNs() < due) return 0;
    }
    const double t = double(m_pos) / trueRate;
    if (m_opt.xrunEverySec > 0 && t >= m_nextXrun) {
        m_nextXrun += m_opt.xrunEverySec;
        m_pos += uint64_t(m_opt.rate / 20);  // 50 ms lost
        xrun = true;
    }
    out.samples = std::make_shared<std::vector<int32_t>>(size_t(chunk) * size_t(m_opt.channels));
    int32_t* d = out.samples->data();
    for (uint32_t i = 0; i < chunk; ++i) {
        const double ts = double(m_pos + i) / trueRate;
        // 1 kHz tone, -12 dBFS, with a 50 ms click every second for sync checks.
        double v = 0.25 * std::sin(2.0 * M_PI * 1000.0 * ts);
        if (std::fmod(ts, 1.0) < 0.05) v = 0.5 * std::sin(2.0 * M_PI * 3000.0 * ts);
        const int32_t s = int32_t(std::lrint(v * 2147483647.0));
        const int bits = sampleFmtBits(m_opt.fmt);
        const int32_t q = bits >= 32 ? s : int32_t(s & ~int32_t((1u << (32 - bits)) - 1));
        for (int c = 0; c < m_opt.channels; ++c) d[i * size_t(m_opt.channels) + size_t(c)] = c ? -q : q;
    }
    out.frames = chunk;
    out.tsNs = m_start + int64_t(double(m_pos) * 1e9 / trueRate);
    out.flags = 0;
    m_pos += chunk;
    return int(chunk);
}

}  // namespace pd
