// SPDX-License-Identifier: GPL-3.0-or-later
#include "LevelWatch.h"

#include <algorithm>

namespace pd {

const char* levelKindEvent(LevelKind k) {
    switch (k) {
    case LevelKind::Crush: return "levels_crush";
    case LevelKind::Clip: return "levels_clip";
    case LevelKind::Chroma: return "levels_chroma";
    }
    return "levels";
}

const char* levelKindLabel(LevelKind k) {
    switch (k) {
    case LevelKind::Crush: return "Blacks crushed";
    case LevelKind::Clip: return "Whites clipped";
    case LevelKind::Chroma: return "Chroma past the limit";
    }
    return "Levels";
}

void LevelWatch::reset(const LevelWatchSettings& s, const Rational& fps) {
    m_s = s;
    m_fps = fps.valid() ? fps.toDouble() : 29.97;
    for (Track& t : m_t) t = Track{};
    m_live = Live{};
    m_lastSlot = -1;
    for (uint64_t& n : m_total) n = 0;
    m_done.clear();
}

void LevelWatch::close(int idx) {
    Track& t = m_t[idx];
    if (t.active) {
        LevelEpisode e;
        e.kind = LevelKind(idx);
        e.startSlot = t.start;
        e.endSlot = t.lastOver;
        e.worstPct = t.worst;
        m_done.push_back(e);
        ++m_total[idx];
    }
    t = Track{};
    m_live.active[idx] = false;
}

void LevelWatch::step(int idx, int64_t slot, double pct) {
    Track& t = m_t[idx];
    m_live.pct[idx] = pct;
    const bool over = pct > m_s.thresholdPct;
    if (over) {
        if (!t.running) {
            t.running = true;
            t.start = slot;
            t.worst = 0;
        }
        t.lastOver = slot;
        t.worst = std::max(t.worst, pct);
        if (!t.active && double(slot - t.start + 1) / m_fps >= m_s.minSeconds) {
            t.active = true;
            m_live.active[idx] = true;
            m_live.since[idx] = t.start;
        }
        return;
    }
    if (!t.running) return;
    if (!t.active) {
        // Not long enough yet: any break starts over.
        t = Track{};
        return;
    }
    if (double(slot - t.lastOver) / m_fps >= m_s.endSeconds) close(idx);
}

void LevelWatch::add(int64_t slot, const FrameStats& st) {
    if (!st.analyzed || !st.pixels) return;
    m_lastSlot = slot;
    step(0, slot, st.belowPct());
    step(1, slot, st.abovePct());
    if (st.hasChroma) step(2, slot, st.illegalPct());
}

void LevelWatch::finish() {
    for (int i = 0; i < 3; ++i) close(i);
}

std::vector<LevelEpisode> LevelWatch::take() {
    std::vector<LevelEpisode> out;
    out.swap(m_done);
    return out;
}

}  // namespace pd
