// SPDX-License-Identifier: GPL-3.0-or-later
#include "Chapters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pd {

std::string slotClock(int64_t slot, const Rational& fps) {
    // Nearest second: slot 35964 is 19:59.999 at 29.97 fps and reads as 00:20:00.
    const int64_t sec = fps.valid() ? int64_t(std::llround(double(std::max<int64_t>(0, slot)) * fps.den / fps.num)) : 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld", (long long)(sec / 3600), (long long)((sec / 60) % 60),
                  (long long)(sec % 60));
    return buf;
}

void BoundaryDetector::reset(const Rational& fps, double minGapSec) {
    *this = BoundaryDetector{};
    m_minGap = std::max<int64_t>(1, std::llround(minGapSec * (fps.valid() ? fps.toDouble() : 29.97)));
}

bool BoundaryDetector::add(int64_t slot, Slot kind) {
    if (kind == Slot::Unknown) return false;
    if (kind == Slot::Gap) {
        if (!m_inGap) {
            m_inGap = true;
            m_gapStart = slot;
        }
        m_gapSlots = slot - m_gapStart + 1;
        return false;
    }
    bool mark = false;
    if (m_inGap && m_seenPicture && slot - m_gapStart >= m_minGap) {
        m_marks.push_back(slot);
        mark = true;
    }
    m_inGap = false;
    m_seenPicture = true;
    return mark;
}

namespace {

struct Mark {
    int64_t slot;
    bool boundary;
};

void mergeShort(std::vector<Mark>& m, int64_t total, int64_t minSlots, bool openEnd, const std::vector<int64_t>& keep) {
    auto kept = [&](int64_t s) { return std::binary_search(keep.begin(), keep.end(), s); };
    size_t i = 1;
    while (i < m.size()) {
        const bool last = i + 1 == m.size();
        const int64_t end = last ? total : m[i + 1].slot;
        if (!(last && openEnd) && end - m[i].slot < minSlots && !kept(m[i].slot)) {
            m.erase(m.begin() + long(i));  // merges into the chapter before
            continue;
        }
        ++i;
    }
    // The first chapter has nothing before it: it takes in the next one instead.
    while (m.size() > 1 && m[1].slot - m[0].slot < minSlots && !kept(m[1].slot)) {
        const bool last = m.size() == 2;
        if (last && openEnd) break;
        m.erase(m.begin() + 1);
    }
}

}  // namespace

std::vector<Chapter> planChapters(const ChapterSettings& s, std::vector<int64_t> boundaries, int64_t total,
                                  const Rational& fps, bool openEnd, const std::vector<int64_t>& keepIn) {
    std::vector<Chapter> out;
    if (!s.enabled() || total <= 0 || !fps.valid()) return out;
    std::vector<int64_t> keep = keepIn;
    std::sort(keep.begin(), keep.end());
    const double rate = fps.toDouble();
    const int64_t minSlots = std::llround(s.minSeconds * rate);
    const int64_t near = std::llround(s.intervalNearBoundarySec * rate);

    std::vector<Mark> marks{{0, s.boundaries}};
    if (s.boundaries) {
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        for (int64_t b : boundaries)
            if (b > 0 && b < total) marks.push_back({b, true});
        mergeShort(marks, total, minSlots, openEnd, keep);
    }
    int64_t step = 0;
    if (s.intervalMinutes > 0) {
        step = std::llround(double(s.intervalMinutes) * 60.0 * rate);
        std::vector<Mark> withInterval = marks;
        for (int64_t k = 1; step > 0 && k * step < total; ++k) {
            const int64_t m = k * step;
            bool drop = false;
            for (const Mark& b : marks)
                if (b.boundary && b.slot > 0 && std::llabs(b.slot - m) <= near) drop = true;
            if (!drop) withInterval.push_back({m, false});
        }
        std::sort(withInterval.begin(), withInterval.end(), [](const Mark& a, const Mark& b) { return a.slot < b.slot; });
        marks.swap(withInterval);
        mergeShort(marks, total, minSlots, openEnd, keep);
    }
    int rec = 1;
    for (size_t i = 0; i < marks.size(); ++i) {
        Chapter c;
        c.start = marks[i].slot;
        c.end = i + 1 < marks.size() ? marks[i + 1].slot : total;
        char buf[64];
        const std::string clock = slotClock(c.start, fps);
        if (i == 0) {
            if (s.boundaries) std::snprintf(buf, sizeof buf, "Recording 1 (%s)", clock.c_str());
            else std::snprintf(buf, sizeof buf, "Chapter 01 (%s)", clock.c_str());
        } else if (marks[i].boundary) {
            std::snprintf(buf, sizeof buf, "Recording %d (%s)", ++rec, clock.c_str());
        } else {
            std::snprintf(buf, sizeof buf, "Chapter %02lld (%s)", (long long)(step ? c.start / step + 1 : int64_t(i) + 1),
                          clock.c_str());
        }
        c.title = buf;
        out.push_back(c);
    }
    return out;
}

std::vector<Chapter> chaptersInRange(const std::vector<Chapter>& all, int64_t start, int64_t end) {
    std::vector<Chapter> out;
    for (const Chapter& c : all) {
        if (c.end <= start || c.start >= end) continue;
        Chapter x = c;
        x.start = std::max(c.start, start) - start;
        x.end = std::min(c.end, end) - start;
        if (x.end > x.start) out.push_back(x);
    }
    return out;
}

std::string ffmetadataChapters(const std::vector<Chapter>& ch, const Rational& fps) {
    std::string out = ";FFMETADATA1\n";
    for (const Chapter& c : ch) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "\n[CHAPTER]\nTIMEBASE=%d/%d\nSTART=%lld\nEND=%lld\n", fps.den, fps.num,
                      (long long)c.start, (long long)c.end);
        out += buf;
        std::string t;
        for (char x : c.title) {
            if (x == '=' || x == ';' || x == '#' || x == '\\' || x == '\n') t += '\\';
            t += x;
        }
        out += "title=" + t + "\n";
    }
    return out;
}

void ChapterBook::reset(const ChapterSettings& s, const Rational& fps) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_s = s;
    m_fps = fps;
    m_bounds.clear();
    m_frozen.clear();
}

void ChapterBook::addBoundary(int64_t slot) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_bounds.push_back(slot);
}

std::vector<int64_t> ChapterBook::boundaries() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_bounds;
}

std::vector<Chapter> ChapterBook::forSegment(int64_t start, int64_t end, bool final) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::vector<Chapter> plan = planChapters(m_s, m_bounds, end, m_fps, !final, m_frozen);
    if (!final)
        for (const Chapter& c : plan)
            if (c.start >= start && c.start < end) m_frozen.push_back(c.start);
    return chaptersInRange(plan, start, end);
}

std::vector<Chapter> ChapterBook::all(int64_t total) {
    std::lock_guard<std::mutex> lk(m_mutex);
    return planChapters(m_s, m_bounds, total, m_fps, false, m_frozen);
}

}  // namespace pd
