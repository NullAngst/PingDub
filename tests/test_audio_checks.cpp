// SPDX-License-Identifier: GPL-3.0-or-later
// Audio checks on synthetic tracks: injected hi-fi dropouts (gaps and a
// burst), a dead channel and an inverted channel are each found exactly
// once, near-misses (a 3 ms gap, a 400 ms gap, xrun silence) are not, and six
// simulated hours of clean program give zero events.
#include "engine/AudioCheck.h"
#include "testing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

using namespace pd;

namespace {

constexpr int kRate = 48000;

// Program material that looks like music to a level meter: a few tones with
// a slow swell, percussive hits that attack instantly and decay naturally,
// noise, and pauses with fades in and out.
class Music {
public:
    explicit Music(uint32_t seed = 1) : m_noise(seed) {
        const double f[3] = {55.0, 330.0, 1250.0};
        for (int i = 0; i < 3; ++i) {
            const double w = 2.0 * M_PI * f[i] / kRate;
            m_c[i] = std::cos(w);
            m_s[i] = std::sin(w);
            m_re[i] = 1.0;
            m_im[i] = 0.0;
        }
    }
    // Next sample pair, left and right, in -1..1.
    void next(double& l, double& r) {
        const uint64_t n = m_n++;
        double tone[3];
        for (int i = 0; i < 3; ++i) {
            const double re = m_re[i] * m_c[i] - m_im[i] * m_s[i];
            const double im = m_re[i] * m_s[i] + m_im[i] * m_c[i];
            m_re[i] = re;
            m_im[i] = im;
            tone[i] = im;
        }
        if ((n & 4095) == 0) {
            // Renormalize the oscillators and update the slow envelope.
            for (int i = 0; i < 3; ++i) {
                const double g = 1.0 / std::sqrt(m_re[i] * m_re[i] + m_im[i] * m_im[i]);
                m_re[i] *= g;
                m_im[i] *= g;
            }
            const double t = double(n) / kRate;
            m_swell = 0.55 + 0.45 * std::sin(2.0 * M_PI * t / 7.3);
            // Pause every 97 s: 400 ms fade out, 2 s near silence, 400 ms fade in.
            const double p = std::fmod(t, 97.0);
            const double target = (p >= 50.0 && p < 52.4) ? 0.0 : 1.0;
            m_gate = target;
        }
        const double step = 1.0 / (0.4 * kRate);
        if (m_g < m_gate) m_g = std::min(m_gate, m_g + step);
        else if (m_g > m_gate) m_g = std::max(m_gate, m_g - step);
        // Hit every 1.7 s: instant attack, 40 ms decay.
        if (n % uint64_t(1.7 * kRate) == 0) m_hit = 0.6;
        m_hit *= 0.99948;  // exp(-1 / (0.04 * 48000))
        m_noise = m_noise * 1664525u + 1013904223u;
        const double nz = (double(m_noise >> 8) / double(1u << 24) - 0.5);
        const double music = m_swell * (0.12 * tone[0] + 0.08 * tone[1] + 0.05 * tone[2]) + m_hit * nz + 0.01 * nz;
        const double floor = 0.0002 * nz;  // tape hiss under the pause, about -75 dBFS
        l = m_g * music + floor;
        r = m_g * (0.8 * music + 0.05 * m_swell * tone[1]) + floor;
    }
    uint64_t pos() const { return m_n; }

private:
    double m_c[3], m_s[3], m_re[3], m_im[3];
    double m_swell = 1.0, m_gate = 1.0, m_g = 1.0, m_hit = 0.0;
    uint32_t m_noise;
    uint64_t m_n = 0;
};

using Modifier = std::function<void(uint64_t n, double& l, double& r)>;

std::vector<AudioEvent> run(double seconds, const Modifier& mod, AudioCheck* keep = nullptr, uint32_t seed = 7) {
    AudioCheck c;
    c.reset(kRate, 2);
    Music m(seed);
    std::vector<int32_t> buf(2 * 4800);
    const uint64_t total = uint64_t(seconds * kRate);
    std::vector<AudioEvent> out;
    for (uint64_t done = 0; done < total;) {
        const size_t n = size_t(std::min<uint64_t>(4800, total - done));
        for (size_t i = 0; i < n; ++i) {
            double l, r;
            m.next(l, r);
            if (mod) mod(done + i, l, r);
            buf[2 * i] = int32_t(std::lrint(std::clamp(l, -1.0, 1.0) * 2147483000.0));
            buf[2 * i + 1] = int32_t(std::lrint(std::clamp(r, -1.0, 1.0) * 2147483000.0));
        }
        c.push(buf.data(), n);
        for (auto& e : c.take()) out.push_back(e);
        done += n;
    }
    c.finish();
    for (auto& e : c.take()) out.push_back(e);
    if (keep) *keep = c;
    return out;
}

uint64_t at(double s) { return uint64_t(s * kRate); }

bool near(uint64_t sample, double sec, double tolMs) { return std::fabs(double(sample) / kRate - sec) * 1000.0 <= tolMs; }

void dump(const std::vector<AudioEvent>& ev) {
    for (const auto& e : ev)
        std::printf("    %s at %.3f-%.3f s: %s\n", audioFaultLabel(e.kind), double(e.startSample) / kRate, double(e.endSample) / kRate,
                    e.detail.c_str());
}

}  // namespace

TEST_CASE("dropouts: three gaps and a burst found once each, near-misses ignored") {
    struct Gap {
        double at, ms;
    };
    const Gap gaps[] = {{20.0, 10}, {40.0, 50}, {60.0, 150}};
    const auto ev = run(110.0, [&](uint64_t n, double& l, double& r) {
        for (const Gap& g : gaps)
            if (n >= at(g.at) && n < at(g.at + g.ms / 1000.0)) {
                l *= 0.0005;  // about 66 dB down, with hiss left in: not digital silence
                r *= 0.0005;
            }
        // Too short to count.
        if (n >= at(30.0) && n < at(30.003)) l = r = 0.0001;
        // Too long to count: the program itself stopped.
        if (n >= at(70.0) && n < at(70.4)) {
            l *= 0.0005;
            r *= 0.0005;
        }
        // A buzz burst in the pause (pause runs 50.0 to 52.4 s, silent from about 50.4).
        if (n >= at(51.0) && n < at(51.02)) {
            const double b = 0.5 * std::sin(double(n) * 0.37);
            l += b;
            r += b;
        }
    });
    if (ev.size() != 4) dump(ev);
    CHECK_EQ(ev.size(), size_t(4));
    int gapsFound = 0, bursts = 0;
    for (const AudioEvent& e : ev) {
        if (e.kind == AudioFault::Burst) {
            ++bursts;
            CHECK(near(e.startSample, 51.0, 8));
            continue;
        }
        CHECK(e.kind == AudioFault::Dropout);
        for (const Gap& g : gaps)
            if (near(e.startSample, g.at, 8)) {
                ++gapsFound;
                const double ms = 1000.0 * double(e.endSample - e.startSample) / kRate;
                CHECK_MSG(std::fabs(ms - g.ms) <= 10, "gap at %.0f s measured %.1f ms, injected %.0f ms", g.at, ms, g.ms);
            }
    }
    CHECK_EQ(gapsFound, 3);
    CHECK_EQ(bursts, 1);
}

TEST_CASE("dropouts: exact digital silence is an xrun fill, and so is an ignored window") {
    const auto ev = run(30.0, [](uint64_t n, double& l, double& r) {
        if (n >= at(10.0) && n < at(10.05)) l = r = 0.0;  // xrun fill straight from the audio thread
    });
    CHECK(ev.empty());
    // Through the resampler the fill is no longer exact zeros: the sync engine's xrun report covers it.
    AudioCheck d;
    d.reset(kRate, 2);
    d.ignore(at(9.8), at(10.25));
    Music m;
    std::vector<int32_t> buf(2 * 4800);
    std::vector<AudioEvent> got;
    for (uint64_t done = 0; done < at(30.0); done += 4800) {
        for (size_t i = 0; i < 4800; ++i) {
            double l, r;
            m.next(l, r);
            if (done + i >= at(10.0) && done + i < at(10.05)) l = r = 1e-5;
            buf[2 * i] = int32_t(l * 2147483000.0);
            buf[2 * i + 1] = int32_t(r * 2147483000.0);
        }
        d.push(buf.data(), 4800);
        for (auto& e : d.take()) got.push_back(e);
    }
    d.finish();
    for (auto& e : d.take()) got.push_back(e);
    if (!got.empty()) dump(got);
    CHECK(got.empty());
}

TEST_CASE("dead channel: right channel 46 dB down for 15 s, found once") {
    const auto ev = run(40.0, [](uint64_t n, double& l, double& r) {
        (void)l;
        if (n >= at(5.0) && n < at(20.0)) r *= 0.005;
    });
    if (ev.size() != 1) dump(ev);
    CHECK_EQ(ev.size(), size_t(1));
    if (!ev.empty()) {
        CHECK(ev[0].kind == AudioFault::DeadChannel);
        CHECK_EQ(ev[0].channel, 1);
        CHECK(near(ev[0].startSample, 5.0, 1000));
        CHECK(near(ev[0].endSample, 20.0, 1000));
        CHECK(ev[0].value >= 30.0);
    }
}

TEST_CASE("phase: right channel inverted for 15 s, found once") {
    const auto ev = run(40.0, [](uint64_t n, double& l, double& r) {
        if (n >= at(5.0) && n < at(20.0)) r = -l;
    });
    if (ev.size() != 1) dump(ev);
    CHECK_EQ(ev.size(), size_t(1));
    if (!ev.empty()) {
        CHECK(ev[0].kind == AudioFault::Phase);
        CHECK(near(ev[0].startSample, 5.0, 1000));
        CHECK(near(ev[0].endSample, 20.0, 1000));
        CHECK(ev[0].value < -0.9);
    }
}

TEST_CASE("clean program: six simulated hours, zero events") {
    const auto ev = run(6.0 * 3600.0, {}, nullptr, 12345);
    if (!ev.empty()) dump(ev);
    CHECK(ev.empty());
}

TEST_MAIN()
