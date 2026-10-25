// SPDX-License-Identifier: GPL-3.0-or-later
// Level Measure and Set Levels.
//
// LevelAccumulator adds up the histograms of every locked, non-blank frame
// while the tape plays, and reads black and white points as the 0.1st and
// 99.9th percentile of luma, and peak saturation as the 99.9th percentile of
// chroma distance from neutral.
//
// LevelSolver turns a measurement into brightness, contrast and saturation
// values. Most decoder chips apply brightness as an offset and contrast as a
// gain, but the step per control unit differs per chip, and some add the
// offset before the gain, some after. The solver starts from a guess, then
// fits every measurement it gets to a model that covers both orders:
//   black = aB + bB * contrast + g * brightness + d * contrast * brightness
//   white = aW + bW * contrast + g * brightness + d * contrast * brightness
// With the first reading the guess fills the gaps; by the third reading the
// model is fully determined by measurements alone.
#pragma once

#include "FrameAnalyzer.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pd {

struct LevelTargets {
    int black = 16;
    int white = 235;
    int chroma = FrameAnalyzer::kDefaultChromaLimit;
};

struct LevelReading {
    double seconds = 0;
    uint64_t frames = 0;    // frames that counted
    uint64_t skipped = 0;   // frames left out (no lock, damaged, blank)
    uint64_t pixels = 0;
    int black = -1;         // -1: no data
    int white = -1;
    int peakSat = -1;       // -1: no data or no chroma
    bool blackClipped = false;
    bool whiteClipped = false;
    bool narrow = false;    // never got near both black and white
    bool valid() const { return frames > 0 && black >= 0 && white >= 0; }
};

class LevelAccumulator {
public:
    void reset();
    void add(const FrameStats& s, double frameSeconds);
    void skip(double frameSeconds);
    LevelReading reading() const;
    const std::array<uint64_t, 256>& luma() const { return m_luma; }

    // Smallest value whose cumulative count reaches `fraction` of the total.
    static int percentile(const std::array<uint64_t, 256>& h, uint64_t total, double fraction);
    // A hard pile at the lowest (or highest) occupied code: the chip clipped there.
    static bool pileLow(const std::array<uint64_t, 256>& h, uint64_t total);
    static bool pileHigh(const std::array<uint64_t, 256>& h, uint64_t total);

    static constexpr int kNarrowRange = 110;  // white - black below this is too little to stretch from

private:
    std::array<uint64_t, 256> m_luma{};
    std::array<uint64_t, 256> m_sat{};
    uint64_t m_pixels = 0, m_chroma = 0, m_frames = 0, m_skipped = 0;
    double m_seconds = 0;
};

struct ProcControl {
    bool present = false;
    uint32_t id = 0;
    std::string name;
    int64_t minimum = 0, maximum = 255, step = 1, defaultValue = 128, value = 128;
    int64_t clamp(double v) const;
};

struct ProcAmp {
    ProcControl brightness, contrast, saturation;
};

struct LevelProposal {
    enum class Kind {
        Apply,          // set these values and measure again
        Done,           // already on target
        RaiseBrightness,  // blacks crushed: true black unknown, raise and measure more
        LowerContrast,  // whites clipped: true white unknown, lower and measure more
        NeedData,       // no usable reading
        NoControls,     // driver offers neither brightness nor contrast
    };
    Kind kind = Kind::NeedData;
    int64_t brightness = 0, contrast = 0, saturation = 0;
    bool brightnessChanged = false, contrastChanged = false, saturationChanged = false;
    bool narrow = false;       // reading never got near both black and white
    bool limited = false;      // a value hit the end of the driver's range
    double predictedBlack = 0, predictedWhite = 0;
    std::string message;
};

class LevelSolver {
public:
    void begin(const ProcAmp& amp, const LevelTargets& t = {});
    // First estimate, from a reading taken at the values passed to begin().
    LevelProposal propose(const LevelReading& r);
    // After a proposal was applied: the reading at the new values.
    LevelProposal correct(const LevelReading& r);
    bool converged(const LevelReading& r) const;
    int corrections() const { return m_corrections; }
    const ProcAmp& current() const { return m_amp; }
    // Records that the caller set these values (after Apply or a clip step).
    void applied(const LevelProposal& p);

    static constexpr int kBlackTolerance = 2;
    static constexpr int kWhiteTolerance = 3;
    static constexpr int kMaxCorrections = 2;
    // The first estimate rests on guessed step sizes, so it goes this share of
    // the way: a chip that steps harder than guessed then lands past the target
    // instead of crushing into its rail, and the reading after it pins the
    // steps down for the corrections.
    static constexpr double kFirstStep = 0.5;
    static constexpr double kUnsureStep = 0.8;
    static constexpr int kRailMargin = 4;

private:
    struct Obs {
        double b, c, s;       // control values
        double black, white;  // readings
        bool blackOk, whiteOk;
        double sat;           // -1 if none
    };
    LevelProposal solve(double damp = 1.0);
    LevelProposal clipStep(const LevelReading& r, bool crushed);
    double brightnessPrior() const;
    void record(const LevelReading& r);

    ProcAmp m_amp;
    LevelTargets m_t;
    std::vector<Obs> m_obs;
    int m_corrections = 0;
};

}  // namespace pd
