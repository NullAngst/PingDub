// SPDX-License-Identifier: GPL-3.0-or-later
#include "Levels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pd {

// ---------------------------------------------------------------------------
// accumulator

void LevelAccumulator::reset() { *this = LevelAccumulator{}; }

void LevelAccumulator::skip(double frameSeconds) {
    (void)frameSeconds;
    ++m_skipped;
}

void LevelAccumulator::add(const FrameStats& s, double frameSeconds) {
    if (!s.analyzed || !s.pixels) {
        ++m_skipped;
        return;
    }
    for (size_t v = 0; v < 256; ++v) {
        m_luma[v] += s.luma[v];
        m_sat[v] += s.sat[v];
    }
    m_pixels += s.pixels;
    m_chroma += s.chromaSamples;
    ++m_frames;
    m_seconds += frameSeconds;
}

int LevelAccumulator::percentile(const std::array<uint64_t, 256>& h, uint64_t total, double fraction) {
    if (!total) return -1;
    const uint64_t need = std::max<uint64_t>(1, uint64_t(std::ceil(fraction * double(total))));
    uint64_t cum = 0;
    for (int v = 0; v < 256; ++v) {
        cum += h[size_t(v)];
        if (cum >= need) return v;
    }
    return 255;
}

bool LevelAccumulator::pileLow(const std::array<uint64_t, 256>& h, uint64_t total) {
    if (!total) return false;
    int lo = 0;
    while (lo < 252 && !h[size_t(lo)]) ++lo;
    const double pile = double(h[size_t(lo)]);
    const double above = double(h[size_t(lo + 1)] + h[size_t(lo + 2)] + h[size_t(lo + 3)]);
    // A spike at the lowest code with picture continuing right above it: the
    // dark end of the picture was cut off there. An isolated flat black (color
    // bars) has nothing right above it and is not a clip.
    return pile >= 0.002 * double(total) && pile >= 4.0 * double(h[size_t(lo + 1)]) && above >= 0.0005 * double(total);
}

bool LevelAccumulator::pileHigh(const std::array<uint64_t, 256>& h, uint64_t total) {
    std::array<uint64_t, 256> r;
    for (size_t i = 0; i < 256; ++i) r[i] = h[255 - i];
    return pileLow(r, total);
}

LevelReading LevelAccumulator::reading() const {
    LevelReading r;
    r.seconds = m_seconds;
    r.frames = m_frames;
    r.skipped = m_skipped;
    r.pixels = m_pixels;
    if (!m_pixels) return r;
    r.black = percentile(m_luma, m_pixels, 0.001);
    r.white = percentile(m_luma, m_pixels, 0.999);
    if (m_chroma) r.peakSat = percentile(m_sat, m_chroma, 0.999);
    r.blackClipped = pileLow(m_luma, m_pixels);
    r.whiteClipped = pileHigh(m_luma, m_pixels);
    r.narrow = r.white - r.black < kNarrowRange;
    return r;
}

// ---------------------------------------------------------------------------
// solver

int64_t ProcControl::clamp(double v) const {
    const int64_t st = std::max<int64_t>(1, step);
    const double k = std::round((v - double(minimum)) / double(st));
    int64_t out = minimum + int64_t(k) * st;
    return std::clamp(out, minimum, maximum);
}

namespace {

// Dense least squares by normal equations, small n.
bool solveLinear(std::vector<std::vector<double>> a, std::vector<double> b, std::vector<double>& x) {
    const size_t n = b.size();
    for (size_t col = 0; col < n; ++col) {
        size_t piv = col;
        for (size_t r = col + 1; r < n; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (std::fabs(a[piv][col]) < 1e-12) return false;
        std::swap(a[piv], a[col]);
        std::swap(b[piv], b[col]);
        for (size_t r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = a[r][col] / a[col][col];
            for (size_t k = col; k < n; ++k) a[r][k] -= f * a[col][k];
            b[r] -= f * b[col];
        }
    }
    x.resize(n);
    for (size_t i = 0; i < n; ++i) x[i] = b[i] / a[i][i];
    return true;
}

struct Lsq {
    explicit Lsq(size_t n) : ata(n, std::vector<double>(n, 0.0)), atb(n, 0.0) {}
    void add(const std::vector<double>& row, double y, double w) {
        for (size_t i = 0; i < row.size(); ++i) {
            atb[i] += w * row[i] * y;
            for (size_t j = 0; j < row.size(); ++j) ata[i][j] += w * row[i] * row[j];
        }
    }
    bool solve(std::vector<double>& x) { return solveLinear(ata, atb, x); }
    std::vector<std::vector<double>> ata;
    std::vector<double> atb;
};

}  // namespace

void LevelSolver::begin(const ProcAmp& amp, const LevelTargets& t) {
    m_amp = amp;
    m_t = t;
    m_obs.clear();
    m_corrections = 0;
}

double LevelSolver::brightnessPrior() const {
    const ProcControl& b = m_amp.brightness;
    const double span = double(std::max<int64_t>(1, b.maximum - b.minimum));
    // Most chips spread the brightness control over about the whole code range.
    return 255.0 / span;
}

void LevelSolver::record(const LevelReading& r) {
    Obs o;
    o.b = double(m_amp.brightness.value);
    o.c = double(m_amp.contrast.value);
    o.s = double(m_amp.saturation.value);
    o.black = r.black;
    o.white = r.white;
    // A point within a few codes of the end of the range is bent by the chip's
    // rail even when no pile shows yet, so it does not go into the fit.
    o.blackOk = r.valid() && !r.blackClipped && r.black > kRailMargin;
    o.whiteOk = r.valid() && !r.whiteClipped && r.white < 255 - kRailMargin;
    o.sat = r.peakSat;
    m_obs.push_back(o);
}

void LevelSolver::applied(const LevelProposal& p) {
    if (m_amp.brightness.present) m_amp.brightness.value = p.brightness;
    if (m_amp.contrast.present) m_amp.contrast.value = p.contrast;
    if (m_amp.saturation.present) m_amp.saturation.value = p.saturation;
}

bool LevelSolver::converged(const LevelReading& r) const {
    if (!r.valid() || r.blackClipped || r.whiteClipped) return false;
    const bool luma = std::abs(r.black - m_t.black) <= kBlackTolerance && std::abs(r.white - m_t.white) <= kWhiteTolerance;
    const bool chroma = !m_amp.saturation.present || r.peakSat < 0 || r.peakSat <= m_t.chroma;
    return luma && chroma;
}

LevelProposal LevelSolver::clipStep(const LevelReading& r, bool crushed) {
    LevelProposal p;
    p.brightness = m_amp.brightness.value;
    p.contrast = m_amp.contrast.value;
    p.saturation = m_amp.saturation.value;
    p.narrow = r.narrow;
    if (crushed) {
        p.kind = LevelProposal::Kind::RaiseBrightness;
        if (m_amp.brightness.present) {
            const double units = std::max(double(m_amp.brightness.step), 8.0 / brightnessPrior());
            p.brightness = m_amp.brightness.clamp(double(m_amp.brightness.value) + units);
            p.brightnessChanged = p.brightness != m_amp.brightness.value;
        } else if (m_amp.contrast.present) {
            // No brightness control: a little less contrast lifts the crushed end on most chips.
            const ProcControl& c = m_amp.contrast;
            p.contrast = c.clamp(double(c.value) - std::max(double(c.step), 0.06 * double(c.value - c.minimum)));
            p.contrastChanged = p.contrast != c.value;
        }
        p.message = "Blacks are crushed at the current settings, so the true black point is unknown. "
                    "Brightness goes up one step; let it measure a few more seconds.";
    } else {
        p.kind = LevelProposal::Kind::LowerContrast;
        if (m_amp.contrast.present) {
            const ProcControl& c = m_amp.contrast;
            p.contrast = c.clamp(double(c.value) - std::max(double(c.step), 0.06 * double(c.value - c.minimum)));
            p.contrastChanged = p.contrast != c.value;
        } else if (m_amp.brightness.present) {
            const double units = std::max(double(m_amp.brightness.step), 8.0 / brightnessPrior());
            p.brightness = m_amp.brightness.clamp(double(m_amp.brightness.value) - units);
            p.brightnessChanged = p.brightness != m_amp.brightness.value;
        }
        p.message = "Whites are clipped at the current settings, so the true white point is unknown. "
                    "Contrast goes down one step; let it measure a few more seconds.";
    }
    if (!p.brightnessChanged && !p.contrastChanged) {
        p.limited = true;
        p.message += " The control is already at the end of its range.";
    }
    return p;
}

LevelProposal LevelSolver::propose(const LevelReading& r) {
    LevelProposal p;
    p.brightness = m_amp.brightness.value;
    p.contrast = m_amp.contrast.value;
    p.saturation = m_amp.saturation.value;
    if (!m_amp.brightness.present && !m_amp.contrast.present) {
        p.kind = LevelProposal::Kind::NoControls;
        p.message = "The driver offers no brightness or contrast control.";
        return p;
    }
    if (!r.valid()) {
        p.kind = LevelProposal::Kind::NeedData;
        p.message = "No usable frames measured yet. Play the tape and let it measure.";
        return p;
    }
    // A pile at the end of the range, or a point bent by the rail: the true
    // black or white is somewhere past it, so step away and measure again.
    if (r.blackClipped || r.black <= kRailMargin) {
        m_obs.clear();
        return clipStep(r, true);
    }
    if (r.whiteClipped || r.white >= 255 - kRailMargin) {
        m_obs.clear();
        return clipStep(r, false);
    }
    m_obs.clear();
    m_corrections = 0;
    record(r);
    if (converged(r)) {
        p.kind = LevelProposal::Kind::Done;
        p.narrow = r.narrow;
        p.message = "Levels are already on target.";
        p.predictedBlack = r.black;
        p.predictedWhite = r.white;
        return p;
    }
    LevelProposal s = solve(kFirstStep);
    s.narrow = r.narrow;
    return s;
}

LevelProposal LevelSolver::correct(const LevelReading& r) {
    record(r);
    ++m_corrections;
    if (converged(r)) {
        LevelProposal p;
        p.kind = LevelProposal::Kind::Done;
        p.brightness = m_amp.brightness.value;
        p.contrast = m_amp.contrast.value;
        p.saturation = m_amp.saturation.value;
        p.predictedBlack = r.black;
        p.predictedWhite = r.white;
        p.message = "Levels are on target.";
        return p;
    }
    if (!m_obs.back().blackOk && !m_obs.back().whiteOk) return clipStep(r, r.blackClipped);
    // Six good points fix the model; with fewer the guess still matters, so
    // stop a little short rather than risk landing in a rail.
    int points = 0;
    for (const Obs& o : m_obs) points += (o.blackOk ? 1 : 0) + (o.whiteOk ? 1 : 0);
    return solve(points >= 6 ? 1.0 : kUnsureStep);
}

LevelProposal LevelSolver::solve(double damp) {
    LevelProposal p;
    p.kind = LevelProposal::Kind::Apply;
    p.brightness = m_amp.brightness.value;
    p.contrast = m_amp.contrast.value;
    p.saturation = m_amp.saturation.value;

    // Prior from the first reading with both points: gain proportional to
    // contrast above its minimum, contrast pivoting near black, brightness a
    // plain offset of about one code range across the control.
    const Obs* ref = nullptr;
    for (const Obs& o : m_obs)
        if (o.blackOk && o.whiteOk) {
            ref = &o;
            break;
        }
    if (!ref) {
        p.kind = LevelProposal::Kind::NeedData;
        p.message = "No reading with both black and white visible.";
        return p;
    }
    const ProcControl& cb = m_amp.brightness;
    const ProcControl& cc = m_amp.contrast;
    const double s0 = brightnessPrior();
    const double pivot = 16.0;
    const double cz = double(cc.minimum);
    const double cspan = std::max(1.0, ref->c - cz);

    // Model, linear in its parameters:
    //   black = aB + bB*c + g*b + d*c*b
    //   white = aW + bW*c + g*b + d*c*b
    // An offset after the gain has d = 0; an offset before the gain scales
    // with the gain and shows up in d. Either way six readings pin it down.
    const double bB0 = (ref->black - pivot) / cspan, bW0 = (ref->white - pivot) / cspan;
    const double aB0 = ref->black - bB0 * ref->c - s0 * ref->b;
    const double aW0 = ref->white - bW0 * ref->c - s0 * ref->b;
    Lsq ls(6);
    const size_t n = m_obs.size();
    for (size_t i = 0; i < n; ++i) {
        const Obs& o = m_obs[i];
        const double w = std::pow(0.5, double(n - 1 - i));  // newest counts most
        if (o.blackOk) ls.add({1, o.c, 0, 0, o.b, o.c * o.b}, o.black, w);
        if (o.whiteOk) ls.add({0, 0, 1, o.c, o.b, o.c * o.b}, o.white, w);
    }
    // Weak priors fill whatever the readings cannot pin down yet. A control
    // the driver does not have keeps its prior exactly.
    const double lam = 1e-3, hard = 1e6;
    const double sigU = std::max(std::fabs(bW0 - bB0), 1e-6);
    const double sigG = std::max(s0, 1e-9);
    const double sigD = sigG / std::max(1.0, std::fabs(ref->c));
    const double sigA = 100.0 + std::fabs(bW0 * ref->c) + std::fabs(s0 * ref->b);
    auto prior = [&](int k, double v, double sigma, double weight) {
        std::vector<double> row(6, 0.0);
        row[size_t(k)] = 1.0 / sigma;
        ls.add(row, v / sigma, weight);
    };
    prior(0, aB0, sigA, lam);
    prior(1, bB0, sigU, cc.present ? lam : hard);
    prior(2, aW0, sigA, lam);
    prior(3, bW0, sigU, cc.present ? lam : hard);
    prior(4, s0, sigG, cb.present ? lam : hard);
    prior(5, 0.0, sigD, (cb.present && cc.present) ? lam : hard);
    std::vector<double> th;
    if (!ls.solve(th)) {
        p.kind = LevelProposal::Kind::NeedData;
        p.message = "The measurements do not fit a level model.";
        return p;
    }
    const double aB = th[0], bB = th[1], aW = th[2], bW = th[3], g = th[4], d = th[5];
    auto blackAt = [&](double cv, double bv) { return aB + bB * cv + g * bv + d * cv * bv; };
    auto whiteAt = [&](double cv, double bv) { return aW + bW * cv + g * bv + d * cv * bv; };

    const Obs& latest = m_obs.back();
    const double tb = damp >= 1.0 ? m_t.black : latest.black + damp * (m_t.black - latest.black);
    const double tw = damp >= 1.0 ? m_t.white : latest.white + damp * (m_t.white - latest.white);
    // The range depends on contrast alone: brightness terms cancel in white - black.
    double c = double(cc.value), b = double(cb.value);
    if (cc.present && std::fabs(bW - bB) > 1e-12) c = ((tw - tb) - (aW - aB)) / (bW - bB);
    if (cc.present) {
        p.contrast = cc.clamp(c);
        p.limited |= (c < double(cc.minimum) - 0.5 * double(cc.step)) || (c > double(cc.maximum) + 0.5 * double(cc.step));
    }
    if (cb.present) {
        // Brightness for the contrast actually set, so rounding contrast does not move black.
        const double cs = double(p.contrast);
        const double slope = g + d * cs;
        if (std::fabs(slope) > 1e-12) {
            if (cc.present) b = (tb - aB - bB * cs) / slope;
            else b = ((tb + tw) / 2.0 - (aB + aW) / 2.0 - (bB + bW) / 2.0 * cs) / slope;
        }
        p.brightness = cb.clamp(b);
        p.limited |= (b < double(cb.minimum) - 0.5 * double(cb.step)) || (b > double(cb.maximum) + 0.5 * double(cb.step));
    }
    p.predictedBlack = blackAt(double(p.contrast), double(p.brightness));
    p.predictedWhite = whiteAt(double(p.contrast), double(p.brightness));

    // Saturation: only brought down when the peak is past the limit.
    const ProcControl& cs = m_amp.saturation;
    const Obs& last = m_obs.back();
    if (cs.present && last.sat > m_t.chroma) {
        const double sz = double(cs.minimum);
        double us0 = 0;
        for (const Obs& o : m_obs)
            if (o.sat > 0 && o.s - sz > 0) {
                us0 = o.sat / (o.s - sz);
                break;
            }
        if (us0 <= 0) us0 = last.sat / std::max(1.0, last.s - sz);
        const double es0 = -us0 * sz;
        Lsq lc(2);
        for (size_t i = 0; i < n; ++i) {
            const Obs& o = m_obs[i];
            if (o.sat < 0) continue;
            lc.add({o.s, 1}, o.sat, std::pow(0.5, double(n - 1 - i)));
        }
        lc.add({1.0 / std::max(us0, 1e-9), 0}, us0 / std::max(us0, 1e-9), lam);
        lc.add({0, 1.0 / 50.0}, es0 / 50.0, lam);
        std::vector<double> sv;
        if (lc.solve(sv) && sv[0] > 1e-9) {
            const double want = (double(m_t.chroma) - 2.0 - sv[1]) / sv[0];
            p.saturation = std::min(cs.value, cs.clamp(want));
        }
    }

    p.brightnessChanged = cb.present && p.brightness != cb.value;
    p.contrastChanged = cc.present && p.contrast != cc.value;
    p.saturationChanged = cs.present && p.saturation != cs.value;
    if (!p.brightnessChanged && !p.contrastChanged && !p.saturationChanged) {
        p.kind = LevelProposal::Kind::Done;
        p.message = p.limited ? "The controls are at the end of their range; this is as close as the device goes."
                              : "These are the closest values the controls allow.";
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "Expected black %.0f and white %.0f after the change.", p.predictedBlack, p.predictedWhite);
    if (p.kind == LevelProposal::Kind::Apply) p.message = buf;
    if (p.limited && p.kind == LevelProposal::Kind::Apply)
        p.message += " A control is at the end of its range, so the target may be out of reach.";
    return p;
}

}  // namespace pd
