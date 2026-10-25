// SPDX-License-Identifier: GPL-3.0-or-later
// Frame analyzer on synthetic frames with known content (ramps, color bars,
// black borders, head-switching noise): exact percentiles and the right
// active area. Then Set Levels against simulated proc amps with different
// step sizes per control, an offset either side of the gain, and clipping
// rails: it has to land on 16 and 235 within two corrections.
#include "engine/FrameAnalyzer.h"
#include "engine/LevelWatch.h"
#include "engine/Levels.h"
#include "engine/PixelConvert.h"
#include "testing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace pd;

namespace {

const uint8_t kBars[8][3] = {{180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
                             {84, 184, 198},  {65, 100, 212}, {35, 212, 114}, {16, 128, 128}};

// Picture description: luma and chroma per pixel, borders, noise rows.
struct Synth {
    int w = 720, h = 480;
    int left = 12, right = 8, top = 0;
    bool bars = false;  // chroma from color bars over the active width
    std::vector<uint8_t> Y, U, V;  // full resolution; U/V per pixel
    void build(uint32_t seed) {
        Y.assign(size_t(w) * size_t(h), 16);
        U.assign(Y.size(), 128);
        V.assign(Y.size(), 128);
        std::mt19937 rng(seed);
        const int aw = w - left - right;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                if (y >= h - FrameAnalyzer::kHeadSwitchLines) {
                    Y[i] = uint8_t(rng() & 255);  // head-switching noise: full range garbage
                    U[i] = uint8_t(rng() & 255);
                    V[i] = uint8_t(rng() & 255);
                    continue;
                }
                if (x < left || x >= w - right || y < top) continue;
                const int ax = x - left;
                if (bars) {
                    // Seven bars, black left out: a black bar against a black border is border.
                    const int b = std::min(6, ax * 7 / aw);
                    Y[i] = kBars[b][0];
                    U[i] = kBars[b][1];
                    V[i] = kBars[b][2];
                } else {
                    Y[i] = uint8_t(20 + (ax + 3 * y) % 201);  // diagonal ramp 20..220
                }
            }
    }
    std::vector<uint8_t> yuyv() const {
        std::vector<uint8_t> o(size_t(w) * size_t(h) * 2);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; x += 2) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                uint8_t* p = o.data() + (size_t(y) * size_t(w) + size_t(x)) * 2;
                p[0] = Y[i];
                p[1] = U[i];
                p[2] = Y[i + 1];
                p[3] = V[i];
            }
        return o;
    }
    std::vector<uint8_t> uyvy() const {
        std::vector<uint8_t> o = yuyv();
        for (size_t i = 0; i < o.size(); i += 2) std::swap(o[i], o[i + 1]);
        return o;
    }
    std::vector<uint8_t> nv12() const {
        std::vector<uint8_t> o(size_t(w) * size_t(h) * 3 / 2);
        std::copy(Y.begin(), Y.end(), o.begin());
        uint8_t* c = o.data() + size_t(w) * size_t(h);
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w / 2; ++x) {
                const size_t i = size_t(2 * y) * size_t(w) + size_t(2 * x);
                c[size_t(y) * size_t(w) + size_t(2 * x)] = U[i];
                c[size_t(y) * size_t(w) + size_t(2 * x) + 1] = V[i];
            }
        return o;
    }
    // Expected numbers over the active area, the slow way.
    int lumaPct(double frac) const {
        std::vector<uint8_t> v;
        for (int y = top; y < h - FrameAnalyzer::kHeadSwitchLines; ++y)
            for (int x = left; x < w - right; ++x) v.push_back(Y[size_t(y) * size_t(w) + size_t(x)]);
        std::sort(v.begin(), v.end());
        const size_t need = std::max<size_t>(1, size_t(std::ceil(frac * double(v.size()))));
        return v[need - 1];
    }
    int satPct(double frac, bool halfRows) const {
        std::vector<int> v;
        for (int y = top; y < h - FrameAnalyzer::kHeadSwitchLines; y += halfRows ? 2 : 1)
            for (int x = left; x < w - right; x += 2) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                v.push_back(FrameAnalyzer::chromaDistance(U[i], V[i]));
            }
        std::sort(v.begin(), v.end());
        const size_t need = std::max<size_t>(1, size_t(std::ceil(frac * double(v.size()))));
        return v[need - 1];
    }
};

VideoFormat fmtFor(PixFmt pf, int w = 720, int h = 480) {
    VideoFormat f;
    f.pixfmt = pf;
    f.width = w;
    f.height = h;
    f.bytesPerLine = (pf == PixFmt::YUYV || pf == PixFmt::UYVY) ? w * 2 : w;
    f.frameBytes = pf == PixFmt::NV12 ? size_t(w) * size_t(h) * 3 / 2 : size_t(f.bytesPerLine) * size_t(h);
    return f;
}

LevelReading readingOf(const FrameStats& st) {
    LevelAccumulator acc;
    acc.add(st, 1.0 / 30);
    return acc.reading();
}

}  // namespace

TEST_CASE("analyzer: ramp with black borders and head-switching noise, YUYV") {
    Synth s;
    s.build(1);
    FrameAnalyzer an;
    an.setFormat(fmtFor(PixFmt::YUYV));
    const auto buf = s.yuyv();
    FrameStats st;
    ScopeFrame sc;
    CHECK(an.analyze(buf.data(), buf.size(), st, &sc));
    CHECK_EQ(st.area.x0, 12);
    CHECK_EQ(st.area.x1, 712);
    CHECK_EQ(st.area.y0, 0);
    CHECK_EQ(st.area.y1, 472);
    const LevelReading r = readingOf(st);
    CHECK_EQ(r.black, s.lumaPct(0.001));
    CHECK_EQ(r.white, s.lumaPct(0.999));
    CHECK_EQ(st.below, uint64_t(0));
    CHECK_EQ(st.above, uint64_t(0));
    CHECK(!r.blackClipped && !r.whiteClipped && !r.narrow);
    CHECK_EQ(sc.width, 360);
    CHECK_EQ(sc.height, 240);
    CHECK_EQ(sc.cwidth, 180);
}

TEST_CASE("analyzer: color bars give the exact peak saturation, YUYV and UYVY") {
    Synth s;
    s.bars = true;
    s.left = 16;
    s.right = 6;
    s.build(2);
    for (PixFmt pf : {PixFmt::YUYV, PixFmt::UYVY}) {
        FrameAnalyzer an;
        an.setFormat(fmtFor(pf));
        const auto buf = pf == PixFmt::YUYV ? s.yuyv() : s.uyvy();
        FrameStats st;
        CHECK(an.analyze(buf.data(), buf.size(), st, nullptr));
        CHECK_EQ(st.area.x0, 16);
        CHECK_EQ(st.area.x1, 714);
        const LevelReading r = readingOf(st);
        CHECK_EQ(r.peakSat, s.satPct(0.999, false));
        CHECK_EQ(r.black, s.lumaPct(0.001));
        CHECK_EQ(r.white, s.lumaPct(0.999));
        // 75% bars sit well inside the legal limit.
        CHECK_EQ(st.chromaIllegal, uint64_t(0));
        // Flat bars with nothing just next to them: not a clip.
        CHECK(!r.blackClipped && !r.whiteClipped);
    }
}

TEST_CASE("analyzer: NV12 and GREY read the same luma, 4:2:0 chroma rows") {
    Synth s;
    s.bars = true;
    s.top = 6;
    s.build(3);
    {
        FrameAnalyzer an;
        an.setFormat(fmtFor(PixFmt::NV12));
        const auto buf = s.nv12();
        FrameStats st;
        CHECK(an.analyze(buf.data(), buf.size(), st, nullptr));
        CHECK_EQ(st.area.y0, 6);
        CHECK_EQ(st.area.x0, 12);
        const LevelReading r = readingOf(st);
        CHECK_EQ(r.black, s.lumaPct(0.001));
        CHECK_EQ(r.white, s.lumaPct(0.999));
        CHECK_EQ(r.peakSat, s.satPct(0.999, true));
    }
    {
        FrameAnalyzer an;
        an.setFormat(fmtFor(PixFmt::GREY));
        FrameStats st;
        CHECK(an.analyze(s.Y.data(), s.Y.size(), st, nullptr));
        CHECK(!st.hasChroma);
        const LevelReading r = readingOf(st);
        CHECK_EQ(r.black, s.lumaPct(0.001));
        CHECK_EQ(r.white, s.lumaPct(0.999));
        CHECK_EQ(r.peakSat, -1);
    }
}

TEST_CASE("analyzer: crushed blacks and clipped whites are seen as piles") {
    Synth s;
    s.left = s.right = 0;
    s.build(4);
    // Stretch the ramp past both rails, the way a chip with too much contrast does.
    for (int y = 0; y < s.h - 8; ++y)
        for (int x = 0; x < s.w; ++x) {
            uint8_t& v = s.Y[size_t(y) * size_t(s.w) + size_t(x)];
            v = uint8_t(std::clamp(int(v) * 3 / 2 - 50, 4, 250));
        }
    FrameAnalyzer an;
    an.setFormat(fmtFor(PixFmt::GREY));
    FrameStats st;
    CHECK(an.analyze(s.Y.data(), s.Y.size(), st, nullptr));
    const LevelReading r = readingOf(st);
    CHECK(r.blackClipped);
    CHECK(r.whiteClipped);
    CHECK(st.belowPct() > 0.5);
    CHECK(st.abovePct() > 0.5);
}

TEST_CASE("analyzer: blank and dark frames") {
    FrameAnalyzer an;
    an.setFormat(fmtFor(PixFmt::GREY));
    std::vector<uint8_t> blue(720 * 480, 41);  // a VCR's blue screen in luma
    FrameStats st;
    CHECK(an.analyze(blue.data(), blue.size(), st, nullptr));
    CHECK(st.blank);
    // A frame dark all over keeps its whole width (no border to find).
    std::vector<uint8_t> dark(720 * 480);
    for (size_t i = 0; i < dark.size(); ++i) dark[i] = uint8_t(16 + (i * 7) % 20);
    CHECK(an.analyze(dark.data(), dark.size(), st, nullptr));
    CHECK_EQ(st.area.x0, 0);
    CHECK_EQ(st.area.x1, 720);
    CHECK(!st.blank);
}

TEST_CASE("level watch: an episode needs 3 seconds and ends after a second clear") {
    LevelWatch w;
    LevelWatchSettings s;
    w.reset(s, {30, 1});
    FrameStats bad, good;
    bad.analyzed = good.analyzed = true;
    bad.pixels = good.pixels = 1000;
    bad.below = 10;  // 1%
    good.below = 2;  // 0.2%
    int64_t slot = 0;
    for (int i = 0; i < 60; ++i) w.add(slot++, bad);   // 2 s: not yet
    for (int i = 0; i < 5; ++i) w.add(slot++, good);   // breaks the run
    CHECK(!w.live().active[0]);
    const int64_t start = slot;
    for (int i = 0; i < 100; ++i) w.add(slot++, bad);  // 3.3 s: an episode
    CHECK(w.live().active[0]);
    const int64_t last = slot - 1;
    for (int i = 0; i < 10; ++i) w.add(slot++, good);  // short break: still the same episode
    CHECK(w.take().empty());
    for (int i = 0; i < 40; ++i) w.add(slot++, good);  // over a second clear: ends
    const auto eps = w.take();
    CHECK_EQ(eps.size(), size_t(1));
    if (!eps.empty()) {
        CHECK_EQ(eps[0].startSlot, start);
        CHECK_EQ(eps[0].endSlot, last);
        CHECK(std::fabs(eps[0].worstPct - 1.0) < 1e-9);
    }
}

// ---------------------------------------------------------------------------
// Set Levels against simulated decoder chips.

namespace {

struct Plant {
    bool offsetBeforeGain = false;
    double pivot = 16;
    double kb = 1.0;            // luma codes per brightness unit
    double bRef = 0;            // brightness value that adds nothing
    double cz = 0, cRef = 100;  // gain = (c - cz) / (cRef - cz)
    double srcBlack = 30, srcWhite = 200;  // at gain 1, no offset
    double srcSat = 90;         // chroma distance peak at saturation sRef
    double sRef = 128;
    int railLo = 1, railHi = 254;

    double out(double y, double b, double c) const {
        const double g = (c - cz) / (cRef - cz);
        if (offsetBeforeGain) return pivot + g * (y + kb * (b - bRef) - pivot);
        return pivot + g * (y - pivot) + kb * (b - bRef);
    }
    LevelReading measure(const ProcAmp& a) const {
        FrameStats st;
        st.analyzed = true;
        st.hasChroma = true;
        // Source: flat between black and white with short tails, like real picture.
        auto addY = [&](double y, uint32_t n) {
            const int v = std::clamp(int(std::lround(out(y, double(a.brightness.value), double(a.contrast.value)))), railLo, railHi);
            st.luma[size_t(v)] += n;
            st.pixels += n;
        };
        for (double y = srcBlack; y <= srcWhite; y += 0.25) addY(y, 250);
        for (int k = 1; k <= 4; ++k) {
            addY(srcBlack - k, 200u >> k);
            addY(srcWhite + k, 200u >> k);
        }
        const double sg = a.saturation.present ? double(a.saturation.value) / sRef : 1.0;
        for (double d = 0; d <= srcSat; d += 0.5) {
            const int v = std::clamp(int(std::lround(d * sg)), 0, 255);
            st.sat[size_t(v)] += 100;
            st.chromaSamples += 100;
        }
        LevelAccumulator acc;
        acc.add(st, 3.0);
        return acc.reading();
    }
};

ProcControl ctl(int64_t mn, int64_t mx, int64_t step, int64_t value) {
    ProcControl c;
    c.present = true;
    c.minimum = mn;
    c.maximum = mx;
    c.step = step;
    c.defaultValue = value;
    c.value = value;
    return c;
}

void apply(ProcAmp& a, const LevelProposal& p) {
    a.brightness.value = p.brightness;
    a.contrast.value = p.contrast;
    a.saturation.value = p.saturation;
    if (std::getenv("PD_TRACE"))
        std::printf("    apply b=%lld c=%lld s=%lld (%s) predicted %.1f/%.1f\n", (long long)p.brightness, (long long)p.contrast,
                    (long long)p.saturation, p.message.c_str(), p.predictedBlack, p.predictedWhite);
}

// The same loop the Set Levels dialog runs. Returns corrections used, or -1.
int runSetLevels(const Plant& plant, ProcAmp amp, LevelReading* last = nullptr) {
    LevelSolver solver;
    solver.begin(amp);
    LevelReading r = plant.measure(amp);
    LevelProposal p = solver.propose(r);
    for (int i = 0; i < 8 && (p.kind == LevelProposal::Kind::RaiseBrightness || p.kind == LevelProposal::Kind::LowerContrast); ++i) {
        apply(amp, p);
        solver.begin(amp);
        r = plant.measure(amp);
        p = solver.propose(r);
    }
    if (p.kind == LevelProposal::Kind::Done) {
        if (last) *last = r;
        return solver.converged(r) ? 0 : -1;
    }
    if (p.kind != LevelProposal::Kind::Apply) return -1;
    apply(amp, p);
    solver.applied(p);
    r = plant.measure(amp);
    if (std::getenv("PD_TRACE")) std::printf("    read %d/%d clip %d/%d\n", r.black, r.white, r.blackClipped, r.whiteClipped);
    while (!solver.converged(r) && solver.corrections() < LevelSolver::kMaxCorrections) {
        p = solver.correct(r);
        if (p.kind == LevelProposal::Kind::Done) break;
        apply(amp, p);
        solver.applied(p);
        r = plant.measure(amp);
        if (std::getenv("PD_TRACE")) std::printf("    read %d/%d clip %d/%d\n", r.black, r.white, r.blackClipped, r.whiteClipped);
    }
    if (last) *last = r;
    return solver.converged(r) ? solver.corrections() : -1;
}

}  // namespace

TEST_CASE("set levels: offset after the gain, 1 code per unit (TW9910-like)") {
    Plant p;
    ProcAmp a;
    a.brightness = ctl(-128, 127, 1, 0);
    a.contrast = ctl(0, 255, 1, 100);
    a.saturation = ctl(0, 255, 1, 128);
    LevelReading r;
    const int n = runSetLevels(p, a, &r);
    CHECK_MSG(n >= 0 && n <= 2, "corrections %d, black %d white %d", n, r.black, r.white);
}

TEST_CASE("set levels: offset before the gain, 1.25 codes per unit, pivot 32 (saa711x-like ranges)") {
    Plant p;
    p.offsetBeforeGain = true;
    p.pivot = 32;
    p.kb = 1.25;
    p.bRef = 128;
    p.cz = -20;
    p.cRef = 64;
    p.srcBlack = 45;
    p.srcWhite = 185;
    ProcAmp a;
    a.brightness = ctl(0, 255, 1, 128);
    a.contrast = ctl(0, 127, 1, 64);
    a.saturation = ctl(0, 127, 1, 64);
    p.sRef = 64;
    LevelReading r;
    const int n = runSetLevels(p, a, &r);
    CHECK_MSG(n >= 0 && n <= 2, "corrections %d, black %d white %d", n, r.black, r.white);
}

TEST_CASE("set levels: brightness half the guess, contrast pivoting at zero") {
    Plant p;
    p.pivot = 0;
    p.kb = 0.5;
    p.srcBlack = 8;
    p.srcWhite = 170;
    ProcAmp a;
    a.brightness = ctl(-128, 127, 1, 0);
    a.contrast = ctl(0, 255, 1, 100);
    LevelReading r;
    const int n = runSetLevels(p, a, &r);
    CHECK_MSG(n >= 0 && n <= 2, "corrections %d, black %d white %d", n, r.black, r.white);
}

TEST_CASE("set levels: 16-bit controls with coarse steps, brightness twice the guess (bttv-like)") {
    Plant p;
    p.kb = 2.0 * 255.0 / 65535.0;
    p.bRef = 32768;
    p.cz = 0;
    p.cRef = 27648;
    p.srcBlack = 24;
    p.srcWhite = 214;
    p.sRef = 32768;
    ProcAmp a;
    a.brightness = ctl(0, 65535, 256, 32768);
    a.contrast = ctl(0, 65535, 128, 27648);
    a.saturation = ctl(0, 65535, 256, 32768);
    LevelReading r;
    const int n = runSetLevels(p, a, &r);
    CHECK_MSG(n >= 0 && n <= 2, "corrections %d, black %d white %d", n, r.black, r.white);
}

TEST_CASE("set levels: starts crushed at the rail, raises brightness first") {
    Plant p;
    p.srcBlack = 10;
    p.srcWhite = 225;
    ProcAmp a;
    a.brightness = ctl(-128, 127, 1, -30);
    a.contrast = ctl(0, 255, 1, 100);
    a.saturation = ctl(0, 255, 1, 128);
    const LevelReading first = p.measure(a);
    CHECK(first.blackClipped);
    LevelSolver s;
    s.begin(a);
    CHECK(s.propose(first).kind == LevelProposal::Kind::RaiseBrightness);
    LevelReading r;
    const int n = runSetLevels(p, a, &r);
    CHECK_MSG(n >= 0 && n <= 2, "corrections %d, black %d white %d", n, r.black, r.white);
}

TEST_CASE("set levels: saturation past the limit comes down, never up") {
    Plant p;
    p.srcSat = 130;
    ProcAmp a;
    a.brightness = ctl(-128, 127, 1, 0);
    a.contrast = ctl(0, 255, 1, 100);
    a.saturation = ctl(0, 255, 1, 128);
    LevelReading r;
    const int n = runSetLevels(p, a, &r);
    CHECK_MSG(n >= 0 && n <= 2 && r.peakSat <= FrameAnalyzer::kDefaultChromaLimit, "corrections %d sat %d", n, r.peakSat);
    // A pale tape stays pale: saturation is not pushed up to the limit.
    Plant pale;
    pale.srcSat = 60;
    LevelSolver s;
    s.begin(a);
    const LevelProposal pr = s.propose(pale.measure(a));
    CHECK(!pr.saturationChanged);
}

TEST_CASE("set levels: a narrow sample is flagged") {
    Plant p;
    p.srcBlack = 40;
    p.srcWhite = 110;
    ProcAmp a;
    a.brightness = ctl(-128, 127, 1, 0);
    a.contrast = ctl(0, 255, 1, 100);
    const LevelReading r = p.measure(a);
    CHECK(r.narrow);
    LevelSolver s;
    s.begin(a);
    CHECK(s.propose(r).narrow);
}

TEST_MAIN()
