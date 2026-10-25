// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers shared by the tests that write real files through OutputWriter.
#pragma once

#include "engine/Output.h"
#include "engine/PixelConvert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace pdtest {

inline std::string tmpDir() {
    static std::string dir;
    if (dir.empty()) {
        const char* base = std::getenv("PD_TEST_TMP");
        std::string t = (base ? std::string(base) : std::string("/tmp")) + "/pingdub-test-XXXXXX";
        std::vector<char> buf(t.begin(), t.end());
        buf.push_back(0);
        dir = mkdtemp(buf.data());
    }
    return dir;
}

inline std::string shell(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

inline std::string ffprobePath() {
    if (const char* p = std::getenv("PD_FFPROBE")) return p;
    return "ffprobe";
}

inline pd::VideoFormat smallFormat(pd::PixFmt pf, int w = 160, int h = 120) {
    pd::VideoFormat vf;
    vf.pixfmt = pf;
    vf.width = w;
    vf.height = h;
    vf.fps = {30000, 1001};
    vf.field = pd::FieldOrder::BFF;
    vf.sar = {10, 11};
    switch (pf) {
    case pd::PixFmt::YUYV:
    case pd::PixFmt::UYVY: vf.bytesPerLine = w * 2; vf.frameBytes = size_t(vf.bytesPerLine) * size_t(h); break;
    case pd::PixFmt::Y210: vf.bytesPerLine = w * 4; vf.frameBytes = size_t(vf.bytesPerLine) * size_t(h); break;
    case pd::PixFmt::V210: vf.bytesPerLine = pd::v210Stride(w); vf.frameBytes = size_t(vf.bytesPerLine) * size_t(h); break;
    case pd::PixFmt::NV12: vf.bytesPerLine = w; vf.frameBytes = size_t(w) * size_t(h) * 3 / 2; break;
    case pd::PixFmt::GREY: vf.bytesPerLine = w; vf.frameBytes = size_t(w) * size_t(h); break;
    default: break;
    }
    return vf;
}

inline pd::BytesPtr noiseFrame(const pd::VideoFormat& vf, int idx) {
    auto b = std::make_shared<pd::Bytes>(vf.frameBytes);
    uint32_t s = 0x9e3779b9u * uint32_t(idx + 1);
    for (size_t i = 0; i < b->size(); ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        (*b)[i] = uint8_t(s);
    }
    if (vf.pixfmt == pd::PixFmt::Y210)
        for (size_t i = 0; i + 1 < b->size(); i += 2) (*b)[i] &= 0xC0;
    if (vf.pixfmt == pd::PixFmt::V210)
        for (size_t i = 3; i < b->size(); i += 4) (*b)[i] &= 0x3F;
    return b;
}

struct WriteOptions {
    pd::Container container = pd::Container::MKV;
    pd::VideoCodec codec = pd::VideoCodec::FFV1;
    pd::PixFmt pixfmt = pd::PixFmt::YUYV;
    int frames = 60;
    int width = 160, height = 120;
    bool audio = true;
    double segmentSeconds = 0;
    // Slots that repeat the previous frame (inserted), to cover the hash reuse path.
    std::vector<int> repeats;
    std::function<std::vector<pd::Chapter>(int64_t, int64_t, bool)> chapters;
};

struct WriteResult {
    std::vector<std::string> files;
    std::vector<pd::SegmentRecord> segments;
};

inline WriteResult writeTestCapture(const std::string& name, const WriteOptions& o) {
    pd::OutputPlan plan;
    plan.out.container = o.container;
    plan.out.vcodec = o.codec;
    plan.out.segmentSeconds = o.segmentSeconds;
    plan.out.threads = 4;
    plan.vfmt = smallFormat(o.pixfmt, o.width, o.height);
    plan.haveAudio = o.audio;
    plan.afmt.rate = 48000;
    plan.afmt.channels = 2;
    plan.folder = tmpDir();
    plan.stem = name;
    plan.chapters = o.chapters;
    pd::EventLog log;
    pd::OutputWriter w(&log);
    std::string err;
    WriteResult out;
    if (!w.open(plan, err)) {
        std::printf("  open failed: %s\n", err.c_str());
        return out;
    }
    const double spf = 48000.0 * plan.vfmt.fps.den / plan.vfmt.fps.num;
    std::vector<int32_t> audio;
    uint64_t emitted = 0;
    pd::VideoFrame last;
    for (int i = 0; i < o.frames; ++i) {
        pd::VideoFrame f;
        const bool rep = std::find(o.repeats.begin(), o.repeats.end(), i) != o.repeats.end() && last.data;
        if (rep) {
            f = last;
        } else {
            f.data = noiseFrame(plan.vfmt, i);
            f.size = f.data->size();
            last = f;
        }
        w.syncVideo(f, i, rep);
        if (o.audio) {
            const uint64_t target = uint64_t(std::llround((i + 1) * spf));
            const size_t n = size_t(target - emitted);
            audio.assign(n * 2, 0);
            for (size_t k = 0; k < n; ++k) audio[2 * k] = audio[2 * k + 1] = int32_t((emitted + k) * 7919u) << 16;
            w.syncAudio(audio.data(), n);
            emitted = target;
        }
    }
    w.close({});
    if (w.failed()) std::printf("  writer error: %s\n", w.error().c_str());
    out.files = w.files();
    out.segments = w.segments();
    return out;
}

}  // namespace pdtest
