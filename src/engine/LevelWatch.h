// SPDX-License-Identifier: GPL-3.0-or-later
// Crush, clip and chroma warnings during a capture. An episode starts when
// more than the threshold share of active-picture pixels sits below 16, above
// 235, or past the chroma limit for long enough without a break, and ends
// after a second back under the threshold. Each episode is one event with
// its start, end and worst value. Warnings never stop a capture.
#pragma once

#include "FrameAnalyzer.h"
#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pd {

struct LevelWatchSettings {
    double thresholdPct = 0.5;
    double minSeconds = 3.0;
    double endSeconds = 1.0;
};

enum class LevelKind { Crush, Clip, Chroma };
const char* levelKindEvent(LevelKind k);   // "levels_crush", ...
const char* levelKindLabel(LevelKind k);   // "Blacks crushed", ...

struct LevelEpisode {
    LevelKind kind = LevelKind::Crush;
    int64_t startSlot = 0;
    int64_t endSlot = 0;  // last slot over the threshold
    double worstPct = 0;
};

class LevelWatch {
public:
    void reset(const LevelWatchSettings& s, const Rational& fps);
    // One analyzed timeline slot.
    void add(int64_t slot, const FrameStats& st);
    // Ends any open episode at the last slot seen.
    void finish();
    // Episodes completed since the last call.
    std::vector<LevelEpisode> take();

    struct Live {
        bool active[3] = {false, false, false};
        int64_t since[3] = {0, 0, 0};
        double pct[3] = {0, 0, 0};
    };
    Live live() const { return m_live; }
    uint64_t total(LevelKind k) const { return m_total[int(k)]; }

private:
    struct Track {
        bool running = false, active = false;
        int64_t start = 0, lastOver = 0;
        double worst = 0;
    };
    void step(int idx, int64_t slot, double pct);
    void close(int idx);

    LevelWatchSettings m_s;
    double m_fps = 29.97;
    Track m_t[3];
    Live m_live;
    int64_t m_lastSlot = -1;
    uint64_t m_total[3] = {0, 0, 0};
    std::vector<LevelEpisode> m_done;
};

}  // namespace pd
