// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pd {

const char* audioFaultEvent(AudioFault f) {
    switch (f) {
    case AudioFault::Dropout:
    case AudioFault::Burst: return "audio_dropout";
    case AudioFault::DeadChannel: return "audio_dead_channel";
    case AudioFault::Phase: return "audio_phase";
    }
    return "audio";
}

const char* audioFaultLabel(AudioFault f) {
    switch (f) {
    case AudioFault::Dropout: return "Hi-fi dropout (gap)";
    case AudioFault::Burst: return "Hi-fi dropout (burst)";
    case AudioFault::DeadChannel: return "Dead channel";
    case AudioFault::Phase: return "Phase problem";
    }
    return "Audio";
}

static double toDb(double pow) { return pow > 1e-14 ? 10.0 * std::log10(pow) : -140.0; }

void AudioCheck::reset(int rate, int channels, const AudioCheckSettings& s) {
    *this = AudioCheck{};
    m_s = s;
    m_rate = rate > 0 ? rate : 48000;
    m_ch = std::max(1, channels);
    m_hop = std::max(1, m_rate / 1000);
}

void AudioCheck::ignore(uint64_t start, uint64_t end) {
    m_ignore.push_back({start, end});
    // Old windows behind the current position no longer matter.
    while (m_ignore.size() > 64) m_ignore.erase(m_ignore.begin());
}

void AudioCheck::report(AudioEvent e) {
    if (e.kind == AudioFault::Dropout || e.kind == AudioFault::Burst) {
        for (const auto& w : m_ignore)
            if (e.startSample < w.second && e.endSample > w.first) return;
    }
    char buf[200];
    const double ms = 1000.0 * double(e.endSample - e.startSample) / double(m_rate);
    switch (e.kind) {
    case AudioFault::Dropout: std::snprintf(buf, sizeof buf, "gap of %.0f ms, %.0f dB down", ms, e.value); break;
    case AudioFault::Burst: std::snprintf(buf, sizeof buf, "burst of %.0f ms, %.0f dB up", ms, e.value); break;
    case AudioFault::DeadChannel:
        std::snprintf(buf, sizeof buf, "%s channel %.0f dB below the other for %.0f s", e.channel == 1 ? "right" : "left", e.value,
                      ms / 1000.0);
        break;
    case AudioFault::Phase: std::snprintf(buf, sizeof buf, "correlation down to %.2f for %.0f s", e.value, ms / 1000.0); break;
    }
    e.detail = buf;
    m_events.push_back(e);
    ++m_count;
}

void AudioCheck::discontinuity(uint64_t skippedFrames) {
    m_pos += skippedFrames;
    m_hopPow = 0;
    m_hopZero = true;
    m_inHop = 0;
    m_winFill = 0;
    m_haveBase = false;
    m_warm = 0;
    m_st = St::Normal;
}

void AudioCheck::endHop() {
    const double hopMean = m_hopPow / double(m_hop * std::min(m_ch, 2));
    m_win[size_t(m_hops % 4)] = hopMean;
    const uint64_t hop = m_hops++;
    const bool zero = m_hopZero;
    m_hopPow = 0;
    m_hopZero = true;
    m_inHop = 0;
    if (m_winFill < 4) {
        ++m_winFill;
        m_hist[size_t(hop % 16)] = -140;
        m_histZero[size_t(hop % 16)] = zero;
        return;
    }
    double p = 0;
    for (double x : m_win) p += x;
    const double level = toDb(p / 4.0);
    m_hist[size_t(hop % 16)] = level;
    m_histZero[size_t(hop % 16)] = zero;

    if (!m_haveBase) {
        m_base = level;
        m_haveBase = true;
        m_warm = 0;
    }
    const double alpha = 1.0 - std::exp(-1.0 / 200.0);  // 200 ms time constant
    auto sharpStart = [&](double limit) {
        // Was the level within 6 dB of the baseline at most edgeMs hops ago?
        for (int k = 1; k <= m_s.edgeMs && uint64_t(k) <= hop; ++k) {
            const double l = m_hist[size_t((hop - uint64_t(k)) % 16)];
            if (std::fabs(l - limit) <= 6.0) return hop - uint64_t(k) + 1;
        }
        return uint64_t(0);
    };
    if (m_st == St::Normal) {
        ++m_warm;
        const double d = level - m_base;
        // The first 100 ms only settle the baseline.
        if (m_warm > 100) {
            if (d <= -m_s.stepDb && m_base > m_s.silenceDb) {
                const uint64_t e = sharpStart(m_base);
                if (e) {
                    m_st = St::Low;
                    m_candBase = m_base;
                    m_edgeHop = e;
                    m_firstDeep = m_lastDeep = hop;
                    m_deepCount = 1;
                    m_deepNonZero = 0;
                    m_extreme = d;
                    for (uint64_t k = (hop >= 3 ? hop - 3 : 0); k <= hop; ++k) m_deepNonZero += m_histZero[size_t(k % 16)] ? 0 : 1;
                    return;
                }
            } else if (d >= m_s.stepDb && level > m_s.silenceDb + 10.0) {
                const uint64_t e = sharpStart(m_base);
                if (e) {
                    m_st = St::High;
                    m_candBase = m_base;
                    m_edgeHop = e;
                    m_firstDeep = m_lastDeep = hop;
                    m_deepCount = 1;
                    m_deepNonZero = 1;
                    m_extreme = d;
                    return;
                }
            }
        }
        m_base += alpha * (level - m_base);
        return;
    }

    const double d = level - m_candBase;
    const bool low = m_st == St::Low;
    const bool deep = low ? d <= -m_s.stepDb : d >= m_s.stepDb;
    const bool back = low ? d >= -m_s.returnDb : d <= m_s.returnDb;
    if (deep) {
        m_lastDeep = hop;
        ++m_deepCount;
        if (!zero) ++m_deepNonZero;
        m_extreme = low ? std::min(m_extreme, d) : std::max(m_extreme, d);
    }
    const double durMs = double(m_lastDeep - m_firstDeep + 1 + 3);
    if (back) {
        const bool sharpEnd = hop - m_lastDeep <= uint64_t(m_s.edgeMs);
        if (sharpEnd && durMs >= m_s.minMs && durMs <= m_s.maxMs && m_deepNonZero > 0) {
            AudioEvent e;
            e.kind = low ? AudioFault::Dropout : AudioFault::Burst;
            e.startSample = m_edgeHop * uint64_t(m_hop);
            e.endSample = hop * uint64_t(m_hop);
            e.value = std::fabs(m_extreme);
            report(e);
        }
        m_st = St::Normal;
        m_base = m_candBase;
        return;
    }
    if (double(hop - m_firstDeep) > m_s.maxMs + 10.0) {
        // Too long for a dropout: the program itself changed level.
        m_st = St::Normal;
        m_base = level;
    }
}

void AudioCheck::runStep(Run& r, AudioFault kind, int state, double value, int channel, uint64_t secStart, uint64_t secEnd,
                         double needSec) {
    if (state < 0) return;  // near silence: neither counts nor breaks
    if (state > 0) {
        if (!r.running) {
            r = Run{};
            r.running = true;
            r.start = secStart;
            r.worst = value;
            r.channel = channel;
        }
        ++r.seconds;
        r.end = secEnd;
        r.worst = kind == AudioFault::Phase ? std::min(r.worst, value) : std::max(r.worst, value);
        if (double(r.seconds) >= needSec) r.active = true;
        return;
    }
    if (r.running && r.active) {
        AudioEvent e;
        e.kind = kind;
        e.startSample = r.start;
        e.endSample = r.end;
        e.value = r.worst;
        e.channel = r.channel;
        report(e);
    }
    r = Run{};
}

void AudioCheck::endSecond() {
    const uint64_t start = m_secStart, end = m_pos;
    if (m_ch >= 2 && m_secFrames) {
        const double l = m_secPow[0] / double(m_secFrames), r = m_secPow[1] / double(m_secFrames);
        const double ld = toDb(l), rd = toDb(r);
        const double loud = std::max(ld, rd);
        int deadState = -1, phaseState = -1;
        double diff = std::fabs(ld - rd);
        if (loud > m_s.silenceDb) {
            deadState = diff >= m_s.deadDb ? 1 : 0;
            if (deadState == 0 && std::min(ld, rd) > m_s.silenceDb) {
                const double corr = m_secCross / std::sqrt(std::max(1e-30, m_secPow[0] * m_secPow[1]));
                phaseState = corr < m_s.phaseCorr ? 1 : 0;
                runStep(m_phase, AudioFault::Phase, phaseState, corr, -1, start, end, m_s.phaseSec);
            }
        }
        if (deadState == 1 && m_dead.running && m_dead.channel != (ld < rd ? 0 : 1)) {
            // The other channel went quiet: that is a separate fault.
            runStep(m_dead, AudioFault::DeadChannel, 0, 0, -1, start, end, m_s.deadSec);
        }
        runStep(m_dead, AudioFault::DeadChannel, deadState, diff, ld < rd ? 0 : 1, start, end, m_s.deadSec);
    }
    m_secPow[0] = m_secPow[1] = 0;
    m_secCross = 0;
    m_secFrames = 0;
    m_secStart = m_pos;
}

void AudioCheck::push(const int32_t* s, size_t frames) {
    const double k = 1.0 / 2147483648.0;
    const int use = std::min(m_ch, 2);
    for (size_t i = 0; i < frames; ++i) {
        const int32_t* f = s + i * size_t(m_ch);
        const double a = double(f[0]) * k;
        const double b = m_ch >= 2 ? double(f[1]) * k : 0.0;
        m_hopPow += a * a + (use == 2 ? b * b : 0.0);
        if (f[0] != 0 || (m_ch >= 2 && f[1] != 0)) m_hopZero = false;
        m_secPow[0] += a * a;
        m_secPow[1] += b * b;
        m_secCross += a * b;
        ++m_secFrames;
        ++m_pos;
        if (++m_inHop >= m_hop) endHop();
        if (m_secFrames >= uint64_t(m_rate)) endSecond();
    }
}

void AudioCheck::finish() {
    if (m_secFrames >= uint64_t(m_rate / 2)) endSecond();
    runStep(m_dead, AudioFault::DeadChannel, 0, 0, -1, m_pos, m_pos, m_s.deadSec);
    runStep(m_phase, AudioFault::Phase, 0, 0, -1, m_pos, m_pos, m_s.phaseSec);
}

std::vector<AudioEvent> AudioCheck::take() {
    std::vector<AudioEvent> out;
    out.swap(m_events);
    return out;
}

}  // namespace pd
