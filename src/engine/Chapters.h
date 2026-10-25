// SPDX-License-Identifier: GPL-3.0-or-later
// Chapters from two sources: a fixed interval, and recording boundaries
// (where someone pressed stop, then record). Boundaries come from the frame
// analyzer: a sync loss or blank stretch of 0.5 s or more, followed by
// picture again. Every mark sits on a timeline slot.
//
// Rules:
//   - a chapter shorter than 30 s merges into the one before it
//   - with both sources on, an interval mark within 10 s of a boundary is dropped
//   - names: "Chapter 03 (00:20:00)" for interval marks, "Recording 2 (00:41:13)"
//     for boundaries; the first chapter is "Recording 1" when boundaries are on
#pragma once

#include "Types.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pd {

struct ChapterSettings {
    int intervalMinutes = 0;   // 0: off; else 5, 10, 15 or 30
    bool boundaries = false;
    double minSeconds = 30.0;
    double intervalNearBoundarySec = 10.0;
    bool enabled() const { return intervalMinutes > 0 || boundaries; }
};

struct Chapter {
    int64_t start = 0;  // timeline slots
    int64_t end = 0;
    std::string title;
};

// Watches the timeline slot by slot for recording boundaries.
class BoundaryDetector {
public:
    enum class Slot { Picture, Gap, Unknown };
    void reset(const Rational& fps, double minGapSec = 0.5);
    // Returns true when this slot starts a new recording.
    bool add(int64_t slot, Slot kind);
    const std::vector<int64_t>& marks() const { return m_marks; }

private:
    int64_t m_minGap = 15;
    bool m_seenPicture = false;
    bool m_inGap = false;
    int64_t m_gapStart = 0;
    int64_t m_gapSlots = 0;
    std::vector<int64_t> m_marks;
};

// Builds the final chapter list. `total` is the timeline length in slots.
// With openEnd the last chapter is not judged short (a segment that the
// capture continues past).
std::vector<Chapter> planChapters(const ChapterSettings& s, std::vector<int64_t> boundaries, int64_t total,
                                  const Rational& fps, bool openEnd = false, const std::vector<int64_t>& keep = {});

// The part of `all` inside [start, end), shifted so `start` is 0. A chapter
// that began before `start` continues under its own title.
std::vector<Chapter> chaptersInRange(const std::vector<Chapter>& all, int64_t start, int64_t end);

// ";FFMETADATA1" text with one [CHAPTER] per entry, time base = one frame.
std::string ffmetadataChapters(const std::vector<Chapter>& ch, const Rational& fps);

// "HH:MM:SS" of a slot.
std::string slotClock(int64_t slot, const Rational& fps);

// Collects boundary marks during a capture and hands out per-segment chapter
// lists. Thread safe: the analysis thread adds marks, the writer thread asks.
class ChapterBook {
public:
    void reset(const ChapterSettings& s, const Rational& fps);
    void addBoundary(int64_t slot);
    // Chapters for the segment [start, end). Not final: later segments follow.
    std::vector<Chapter> forSegment(int64_t start, int64_t end, bool final);
    std::vector<Chapter> all(int64_t total);
    std::vector<int64_t> boundaries() const;
    const ChapterSettings& settings() const { return m_s; }

private:
    mutable std::mutex m_mutex;
    ChapterSettings m_s;
    Rational m_fps{30000, 1001};
    std::vector<int64_t> m_bounds;
    std::vector<int64_t> m_frozen;  // marks already written into a closed segment
};

}  // namespace pd
