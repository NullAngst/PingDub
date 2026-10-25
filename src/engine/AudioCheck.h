// SPDX-License-Identifier: GPL-3.0-or-later
// Watches the captured audio for VHS hi-fi faults the level meters do not
// show. Reports only; the audio is never changed.
//
// Hi-fi dropout: a sudden level step of 20 dB or more, down or up, lasting
// 5 to 200 ms with sharp edges on both sides. The level is a 4 ms RMS
// stepped every millisecond, so a natural decay (a drum, a reverb tail) is
// too slow to count as an edge. Exact digital silence is an xrun fill and is
// counted as an xrun, not a dropout, and so is a gap where the sync engine
// reported an xrun (after resampling, the fill is no longer exact zeros).
//
// Dead channel: one channel 30 dB or more below the other for 10 seconds.
// Phase problem: left/right correlation below -0.5 for 10 seconds.
// Seconds where the audio is near silence count for neither, so the gaps
// between songs on a mono tape do not split one fault into several.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pd {

struct AudioCheckSettings {
    double stepDb = 20.0;
    double returnDb = 10.0;      // back within this of the level before: the gap ended
    double minMs = 5.0;
    double maxMs = 200.0;
    int edgeMs = 6;              // an edge has to complete within this
    double deadDb = 30.0;
    double deadSec = 10.0;
    double phaseCorr = -0.5;
    double phaseSec = 10.0;
    double silenceDb = -60.0;    // quieter than this is treated as silence
};

enum class AudioFault { Dropout, Burst, DeadChannel, Phase };
const char* audioFaultEvent(AudioFault f);  // "audio_dropout", ...
const char* audioFaultLabel(AudioFault f);

struct AudioEvent {
    AudioFault kind = AudioFault::Dropout;
    uint64_t startSample = 0;
    uint64_t endSample = 0;
    double value = 0;   // depth in dB, level difference in dB, or worst correlation
    int channel = -1;   // the quiet channel for DeadChannel
    std::string detail;
};

class AudioCheck {
public:
    void reset(int rate, int channels, const AudioCheckSettings& s = {});
    // Interleaved int32 samples, device bits left-justified.
    void push(const int32_t* samples, size_t frames);
    // Samples were skipped: drop short-term state so the gap does not look like a fault.
    void discontinuity(uint64_t skippedFrames);
    // Samples in [start, end) are an xrun fill: a gap there is already counted as an xrun.
    void ignore(uint64_t start, uint64_t end);
    void finish();
    std::vector<AudioEvent> take();
    uint64_t count() const { return m_count; }
    uint64_t position() const { return m_pos; }

private:
    void endHop();
    void endSecond();
    void report(AudioEvent e);

    AudioCheckSettings m_s;
    int m_rate = 48000, m_ch = 2, m_hop = 48;
    uint64_t m_pos = 0;          // sample frames seen

    // dropout state, per 1 ms hop
    double m_hopPow = 0;
    bool m_hopZero = true;
    int m_inHop = 0;
    uint64_t m_hops = 0;
    std::array<double, 4> m_win{};
    int m_winFill = 0;
    std::array<double, 16> m_hist{};      // window level history, dB
    std::array<bool, 16> m_histZero{};
    bool m_haveBase = false;
    double m_base = -120;
    int m_warm = 0;
    enum class St { Normal, Low, High } m_st = St::Normal;
    double m_candBase = 0;
    uint64_t m_edgeHop = 0, m_firstDeep = 0, m_lastDeep = 0;
    uint64_t m_deepCount = 0, m_deepNonZero = 0;
    double m_extreme = 0;

    // per second state
    double m_secPow[2] = {0, 0};
    double m_secCross = 0;
    uint64_t m_secFrames = 0;
    uint64_t m_secStart = 0;
    struct Run {
        bool running = false, active = false;
        uint64_t start = 0, end = 0;
        int seconds = 0;
        double worst = 0;
        int channel = -1;
    };
    Run m_dead, m_phase;
    void runStep(Run& r, AudioFault kind, int state, double value, int channel, uint64_t secStart, uint64_t secEnd,
                 double needSec);

    std::vector<AudioEvent> m_events;
    std::vector<std::pair<uint64_t, uint64_t>> m_ignore;
    uint64_t m_count = 0;
};

}  // namespace pd
