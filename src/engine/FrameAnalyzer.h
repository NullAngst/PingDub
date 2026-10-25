// SPDX-License-Identifier: GPL-3.0-or-later
// Reads one video frame into 8-bit planes and measures it: luma and chroma
// histograms over the active picture, pixels outside 16-235, chroma past the
// legal limit, mean and spread. Also keeps a downsampled copy for the scopes.
//
// The active picture leaves out the bottom 8 lines (head-switching noise) and
// dark border columns at the sides (up to 1/24 of the width) and dark rows at
// the top (up to 1/24 of the height), so a few lines of blanking never
// decide where black is.
#pragma once

#include "Types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace pd {

struct ActiveArea {
    int x0 = 0, x1 = 0;  // columns [x0, x1)
    int y0 = 0, y1 = 0;  // rows [y0, y1)
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool operator==(const ActiveArea& o) const { return x0 == o.x0 && x1 == o.x1 && y0 == o.y0 && y1 == o.y1; }
};

struct FrameStats {
    bool analyzed = false;
    bool hasChroma = false;
    ActiveArea area;
    uint64_t pixels = 0;         // luma samples in the active area
    uint64_t below = 0;          // luma < 16
    uint64_t above = 0;          // luma > 235
    uint64_t chromaSamples = 0;  // chroma pairs in the active area
    uint64_t chromaIllegal = 0;  // chroma distance from neutral above the limit
    double mean = 0;
    double stddev = 0;
    bool blank = false;          // featureless picture (blue or black screen)
    std::array<uint32_t, 256> luma{};
    std::array<uint32_t, 256> sat{};  // chroma distance from neutral, rounded

    double belowPct() const { return pixels ? 100.0 * double(below) / double(pixels) : 0.0; }
    double abovePct() const { return pixels ? 100.0 * double(above) / double(pixels) : 0.0; }
    double illegalPct() const { return chromaSamples ? 100.0 * double(chromaIllegal) / double(chromaSamples) : 0.0; }
};

// Downsampled copy of a frame for the waveform monitor and vectorscope.
struct ScopeFrame {
    int width = 0, height = 0;    // luma
    int cwidth = 0, cheight = 0;  // chroma, 0 for a gray source
    std::vector<uint8_t> y, cb, cr;
    ActiveArea area;              // in the downsampled coordinates
};

// 8-bit planes of the last analyzed frame, for the contact sheet.
struct PlaneView {
    const uint8_t* y = nullptr;
    const uint8_t* cb = nullptr;  // null for a gray source
    const uint8_t* cr = nullptr;
    int width = 0, height = 0;
    int cwidth = 0, cheight = 0;
};

class FrameAnalyzer {
public:
    static constexpr int kHeadSwitchLines = 8;
    static constexpr double kBlankStddev = 3.0;
    static constexpr int kDefaultChromaLimit = 112;

    void setFormat(const VideoFormat& f);
    void setChromaLimit(int limit) { m_limit = limit; }
    int chromaLimit() const { return m_limit; }
    // MJPEG is compressed and has no planes to read here.
    bool supported() const;

    // Reads the frame into planes only (no statistics).
    bool load(const uint8_t* data, size_t size);
    // Reads and measures the frame. `scope` may be null.
    bool analyze(const uint8_t* data, size_t size, FrameStats& out, ScopeFrame* scope);
    // Fills the planes with black, for a slot the device sent nothing for.
    void loadBlack();
    PlaneView planes() const;

    // Detects the active picture on the loaded luma plane.
    ActiveArea detectArea() const;

    static int chromaDistance(int cb, int cr);

private:
    void measure(FrameStats& out) const;
    void downsample(const ActiveArea& a, ScopeFrame& s) const;

    VideoFormat m_fmt;
    int m_limit = kDefaultChromaLimit;
    int m_cw = 0, m_ch = 0;  // chroma plane size, 0 for gray
    std::vector<uint8_t> m_y, m_cb, m_cr;
    std::vector<uint16_t> m_y16, m_cb16, m_cr16;  // scratch for 10-bit sources
};

}  // namespace pd
