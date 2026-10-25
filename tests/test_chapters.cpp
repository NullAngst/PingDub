// SPDX-License-Identifier: GPL-3.0-or-later
// Chapter rules, then real Matroska files checked with ffprobe -show_chapters:
// interval only, boundaries only, both, and split into segments. Every mark
// has to land exactly on a frame. AVI gets the .chapters.txt sidecar.
#include "capture_util.h"
#include "engine/Chapters.h"
#include "testing.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace pd;
using namespace pdtest;

namespace {

const Rational kNtsc{30000, 1001};

int64_t slotAt(int h, int m, int s) { return std::llround((h * 3600.0 + m * 60.0 + s) * kNtsc.toDouble()); }

struct Probed {
    double start = 0, end = 0;
    std::string title;
};

std::vector<Probed> probeChapters(const std::string& file) {
    std::vector<Probed> out;
    const std::string txt = shell(ffprobePath() + " -v error -show_chapters -of compact '" + file + "'");
    std::istringstream in(txt);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("chapter|", 0) != 0) continue;
        Probed p;
        std::istringstream f(line);
        std::string kv;
        while (std::getline(f, kv, '|')) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "start_time") p.start = std::atof(v.c_str());
            else if (k == "end_time") p.end = std::atof(v.c_str());
            else if (k == "tag:title") p.title = v;
        }
        out.push_back(p);
    }
    return out;
}

// A probed time sits on a frame: it converts back to a whole slot.
bool onFrame(double sec, int64_t slot) {
    const double f = sec * kNtsc.toDouble();
    return std::fabs(f - double(slot)) < 0.05;
}

}  // namespace

TEST_CASE("rules: interval chapters and their names") {
    ChapterSettings s;
    s.intervalMinutes = 10;
    const auto ch = planChapters(s, {}, slotAt(1, 5, 0), kNtsc);
    CHECK_EQ(ch.size(), size_t(7));
    if (ch.size() == 7) {
        CHECK(ch[0].title == "Chapter 01 (00:00:00)");
        CHECK(ch[2].title == "Chapter 03 (00:20:00)");
        CHECK_EQ(ch[2].start, slotAt(0, 20, 0));
        CHECK_EQ(ch[6].end, slotAt(1, 5, 0));
    }
}

TEST_CASE("rules: boundaries, short chapters merge into the one before") {
    ChapterSettings s;
    s.boundaries = true;
    const int64_t total = slotAt(1, 0, 0);
    // 100 s and 110 s: the 10 s chapter at 100 s merges back, so the recording starts at 110 s.
    const auto ch = planChapters(s, {slotAt(0, 1, 40), slotAt(0, 1, 50), slotAt(0, 41, 13)}, total, kNtsc);
    CHECK_EQ(ch.size(), size_t(3));
    if (ch.size() == 3) {
        CHECK(ch[0].title == "Recording 1 (00:00:00)");
        CHECK_EQ(ch[1].start, slotAt(0, 1, 50));
        CHECK(ch[2].title == "Recording 3 (00:41:13)");
    }
    // A boundary 10 s in: the first chapter takes it in. One 10 s before the end: merges back.
    const auto ch2 = planChapters(s, {slotAt(0, 0, 10), total - slotAt(0, 0, 10)}, total, kNtsc);
    CHECK_EQ(ch2.size(), size_t(1));
    // A glitchy run of boundaries leaves one mark, where it settled.
    std::vector<int64_t> run;
    for (int i = 0; i < 6; ++i) run.push_back(slotAt(0, 10, 0) + i * 60);
    const auto ch3 = planChapters(s, run, total, kNtsc);
    CHECK_EQ(ch3.size(), size_t(2));
    if (ch3.size() == 2) CHECK_EQ(ch3[1].start, run.back());
}

TEST_CASE("rules: interval mark near a boundary is dropped, names stay right") {
    ChapterSettings s;
    s.intervalMinutes = 5;
    s.boundaries = true;
    const int64_t total = slotAt(0, 16, 0);
    const auto ch = planChapters(s, {slotAt(0, 5, 6), slotAt(0, 7, 30)}, total, kNtsc);
    // 0 Recording 1, 5:06 Recording 2, 7:30 Recording 3, 10:00 Chapter 03, 15:00 Chapter 04.
    CHECK_EQ(ch.size(), size_t(5));
    if (ch.size() == 5) {
        CHECK(ch[1].title == "Recording 2 (00:05:06)");
        CHECK(ch[3].title == "Chapter 03 (00:10:00)");
        CHECK(ch[4].title == "Chapter 04 (00:15:00)");
    }
}

TEST_CASE("rules: ranges and ffmetadata") {
    std::vector<Chapter> all{{0, 100, "A"}, {100, 300, "B"}, {300, 400, "C"}};
    const auto r = chaptersInRange(all, 150, 350);
    CHECK_EQ(r.size(), size_t(2));
    if (r.size() == 2) {
        CHECK_EQ(r[0].start, int64_t(0));
        CHECK_EQ(r[0].end, int64_t(150));
        CHECK(r[0].title == "B");
        CHECK_EQ(r[1].start, int64_t(150));
    }
    const std::string t = ffmetadataChapters({{0, 900, "Chapter 01 (00:00:00)"}}, kNtsc);
    CHECK(t.rfind(";FFMETADATA1\n", 0) == 0);
    CHECK(t.find("TIMEBASE=1001/30000\nSTART=0\nEND=900\ntitle=Chapter 01 (00:00:00)\n") != std::string::npos);
}

TEST_CASE("boundary detector: half a second of gap, then picture") {
    BoundaryDetector b;
    b.reset(kNtsc);
    int64_t slot = 0;
    for (int i = 0; i < 20; ++i) b.add(slot++, BoundaryDetector::Slot::Gap);  // before any picture: no mark
    for (int i = 0; i < 100; ++i) CHECK(!b.add(slot++, BoundaryDetector::Slot::Picture));
    for (int i = 0; i < 10; ++i) b.add(slot++, BoundaryDetector::Slot::Gap);  // a third of a second: no mark
    CHECK(!b.add(slot++, BoundaryDetector::Slot::Picture));
    for (int i = 0; i < 15; ++i) b.add(slot++, BoundaryDetector::Slot::Gap);
    b.add(slot++, BoundaryDetector::Slot::Unknown);
    const int64_t expect = slot;
    CHECK(b.add(slot++, BoundaryDetector::Slot::Picture));
    CHECK_EQ(b.marks().size(), size_t(1));
    if (!b.marks().empty()) CHECK_EQ(b.marks()[0], expect);
}

TEST_CASE("matroska: interval chapters, frame accurate") {
    ChapterBook book;
    ChapterSettings s;
    s.intervalMinutes = 5;
    book.reset(s, kNtsc);
    WriteOptions o;
    o.width = 96;
    o.height = 64;
    o.audio = false;
    o.frames = int(slotAt(0, 11, 0));
    o.chapters = [&](int64_t a, int64_t b, bool f) { return book.forSegment(a, b, f); };
    const auto w = writeTestCapture("chapters_interval", o);
    CHECK_EQ(w.files.size(), size_t(1));
    if (w.files.empty()) return;
    const auto p = probeChapters(w.files[0]);
    CHECK_EQ(p.size(), size_t(3));
    if (p.size() == 3) {
        CHECK(p[0].title == "Chapter 01 (00:00:00)");
        CHECK(p[1].title == "Chapter 02 (00:05:00)");
        CHECK(p[2].title == "Chapter 03 (00:10:00)");
        CHECK(onFrame(p[1].start, slotAt(0, 5, 0)));
        CHECK(onFrame(p[2].start, slotAt(0, 10, 0)));
        CHECK(onFrame(p[2].end, o.frames));
    }
}

TEST_CASE("matroska: boundary chapters only") {
    ChapterBook book;
    ChapterSettings s;
    s.boundaries = true;
    book.reset(s, kNtsc);
    book.addBoundary(2001);
    book.addBoundary(6007);
    WriteOptions o;
    o.width = 96;
    o.height = 64;
    o.audio = false;
    o.frames = 9000;
    o.chapters = [&](int64_t a, int64_t b, bool f) { return book.forSegment(a, b, f); };
    const auto w = writeTestCapture("chapters_bounds", o);
    if (w.files.empty()) return;
    const auto p = probeChapters(w.files[0]);
    CHECK_EQ(p.size(), size_t(3));
    if (p.size() == 3) {
        CHECK(p[0].title == "Recording 1 (00:00:00)");
        CHECK(p[1].title == "Recording 2 (00:01:07)");
        CHECK(onFrame(p[1].start, 2001));
        CHECK(onFrame(p[2].start, 6007));
    }
}

TEST_CASE("matroska: both sources together, and across segments") {
    for (double seg : {0.0, 200.0}) {
        ChapterBook book;
        ChapterSettings s;
        s.intervalMinutes = 5;
        s.boundaries = true;
        book.reset(s, kNtsc);
        book.addBoundary(9000);   // 10 s from the 5:00 interval mark: that mark is dropped
        book.addBoundary(13000);
        WriteOptions o;
        o.width = 96;
        o.height = 64;
        o.audio = false;
        o.frames = int(slotAt(0, 11, 0));
        o.segmentSeconds = seg;
        o.chapters = [&](int64_t a, int64_t b, bool f) { return book.forSegment(a, b, f); };
        const auto w = writeTestCapture(seg > 0 ? "chapters_both_seg" : "chapters_both", o);
        if (w.files.empty()) continue;
        // Collect every chapter start back on the capture timeline.
        std::vector<std::pair<int64_t, std::string>> marks;
        for (const SegmentRecord& sr : w.segments) {
            const auto p = probeChapters(sr.path);
            CHECK_MSG(!p.empty(), "no chapters in %s", sr.path.c_str());
            for (const Probed& c : p) {
                const int64_t slot = sr.startSlot + std::llround(c.start * kNtsc.toDouble());
                CHECK(onFrame(c.start, slot - sr.startSlot));
                if (marks.empty() || marks.back().second != c.title) marks.push_back({slot, c.title});
            }
        }
        CHECK_MSG(marks.size() == 4, "%zu marks with %zu segments", marks.size(), w.segments.size());
        if (marks.size() == 4) {
            CHECK_EQ(marks[1].first, int64_t(9000));
            CHECK_EQ(marks[2].first, int64_t(13000));
            CHECK(marks[3].second == "Chapter 03 (00:10:00)");
        }
        if (seg > 0) CHECK(w.segments.size() >= 3);
    }
}

TEST_CASE("avi: chapters go to a .chapters.txt sidecar") {
    ChapterBook book;
    ChapterSettings s;
    s.boundaries = true;
    book.reset(s, kNtsc);
    book.addBoundary(1500);
    WriteOptions o;
    o.container = Container::AVI;
    o.codec = VideoCodec::Huff;
    o.width = 96;
    o.height = 64;
    o.audio = false;
    o.frames = 3000;
    o.chapters = [&](int64_t a, int64_t b, bool f) { return book.forSegment(a, b, f); };
    const auto w = writeTestCapture("chapters_avi", o);
    CHECK_EQ(w.segments.size(), size_t(1));
    if (w.segments.empty()) return;
    CHECK(!w.segments[0].chaptersTxt.empty());
    std::ifstream f(w.segments[0].chaptersTxt);
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(ss.str().rfind(";FFMETADATA1", 0) == 0);
    CHECK(ss.str().find("START=1500\n") != std::string::npos);
    CHECK(ss.str().find("title=Recording 2 (00:00:50)") != std::string::npos);
}

TEST_MAIN()
