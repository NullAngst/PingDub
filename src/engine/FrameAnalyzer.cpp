// SPDX-License-Identifier: GPL-3.0-or-later
#include "FrameAnalyzer.h"

#include "PixelConvert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pd {

namespace {

// Rounded distance from neutral for every (Cb, Cr) pair.
struct DistanceTable {
    uint8_t d[256][256];
    DistanceTable() {
        for (int a = 0; a < 256; ++a)
            for (int b = 0; b < 256; ++b) {
                const double x = a - 128, y = b - 128;
                d[a][b] = uint8_t(std::min(255L, std::lround(std::sqrt(x * x + y * y))));
            }
    }
};

const DistanceTable& distances() {
    static const DistanceTable t;
    return t;
}

// Columns or rows darker than this, on average and at their brightest, count as border.
constexpr double kBorderMean = 28.0;
constexpr int kBorderMax = 48;

}  // namespace

int FrameAnalyzer::chromaDistance(int cb, int cr) { return distances().d[cb & 255][cr & 255]; }

void FrameAnalyzer::setFormat(const VideoFormat& f) {
    m_fmt = f;
    const int w = f.width, h = f.height;
    switch (f.pixfmt) {
    case PixFmt::YUYV:
    case PixFmt::UYVY:
    case PixFmt::Y210:
    case PixFmt::V210:
        m_cw = w / 2;
        m_ch = h;
        break;
    case PixFmt::NV12:
    case PixFmt::HM12:
        m_cw = w / 2;
        m_ch = h / 2;
        break;
    default:
        m_cw = m_ch = 0;
        break;
    }
    m_y.assign(size_t(std::max(0, w)) * size_t(std::max(0, h)), 16);
    m_cb.assign(size_t(m_cw) * size_t(m_ch), 128);
    m_cr.assign(size_t(m_cw) * size_t(m_ch), 128);
    if (f.pixfmt == PixFmt::Y210 || f.pixfmt == PixFmt::V210) {
        m_y16.resize(m_y.size());
        m_cb16.resize(m_cb.size());
        m_cr16.resize(m_cr.size());
    } else {
        m_y16.clear();
        m_cb16.clear();
        m_cr16.clear();
    }
}

bool FrameAnalyzer::supported() const {
    switch (m_fmt.pixfmt) {
    case PixFmt::YUYV:
    case PixFmt::UYVY:
    case PixFmt::Y210:
    case PixFmt::V210:
    case PixFmt::NV12:
    case PixFmt::HM12:
    case PixFmt::GREY: return m_fmt.width > 0 && m_fmt.height > 0;
    default: return false;
    }
}

void FrameAnalyzer::loadBlack() {
    std::fill(m_y.begin(), m_y.end(), uint8_t(16));
    std::fill(m_cb.begin(), m_cb.end(), uint8_t(128));
    std::fill(m_cr.begin(), m_cr.end(), uint8_t(128));
}

bool FrameAnalyzer::load(const uint8_t* src, size_t size) {
    if (!supported() || !src) return false;
    const int w = m_fmt.width, h = m_fmt.height;
    int stride = m_fmt.bytesPerLine;
    if (stride <= 0) {
        switch (m_fmt.pixfmt) {
        case PixFmt::YUYV:
        case PixFmt::UYVY: stride = w * 2; break;
        case PixFmt::Y210: stride = w * 4; break;
        case PixFmt::V210: stride = v210Stride(w); break;
        default: stride = w; break;
        }
    }
    size_t need = size_t(stride) * size_t(h);
    if (m_fmt.pixfmt == PixFmt::NV12) need = size_t(stride) * size_t(h) * 3 / 2;
    if (m_fmt.pixfmt == PixFmt::HM12) need = hm12Size(w, h);
    if (size < need) return false;
    Planes8 p8{m_y.data(), m_cb.data(), m_cr.data(), w, m_cw, m_cw};
    switch (m_fmt.pixfmt) {
    case PixFmt::YUYV: yuyvToPlanar(src, stride, w, h, p8); break;
    case PixFmt::UYVY: uyvyToPlanar(src, stride, w, h, p8); break;
    case PixFmt::NV12: nv12ToPlanar(src, stride, w, h, p8); break;
    case PixFmt::HM12: hm12ToPlanar(src, w, h, p8); break;
    case PixFmt::GREY:
        for (int r = 0; r < h; ++r) std::memcpy(m_y.data() + size_t(r) * size_t(w), src + size_t(r) * size_t(stride), size_t(w));
        break;
    case PixFmt::Y210:
    case PixFmt::V210: {
        Planes16 p16{m_y16.data(), m_cb16.data(), m_cr16.data(), w, m_cw, m_cw};
        if (m_fmt.pixfmt == PixFmt::Y210) y210ToPlanar10(src, stride, w, h, p16);
        else v210ToPlanar10(src, stride, w, h, p16);
        // The levels are judged in 8-bit terms: 10-bit 64 is 8-bit 16.
        for (size_t i = 0; i < m_y.size(); ++i) m_y[i] = uint8_t(std::min<int>(255, m_y16[i] >> 2));
        for (size_t i = 0; i < m_cb.size(); ++i) {
            m_cb[i] = uint8_t(std::min<int>(255, m_cb16[i] >> 2));
            m_cr[i] = uint8_t(std::min<int>(255, m_cr16[i] >> 2));
        }
        break;
    }
    default: return false;
    }
    return true;
}

PlaneView FrameAnalyzer::planes() const {
    PlaneView v;
    v.y = m_y.data();
    v.width = m_fmt.width;
    v.height = m_fmt.height;
    if (m_cw) {
        v.cb = m_cb.data();
        v.cr = m_cr.data();
        v.cwidth = m_cw;
        v.cheight = m_ch;
    }
    return v;
}

ActiveArea FrameAnalyzer::detectArea() const {
    const int w = m_fmt.width, h = m_fmt.height;
    ActiveArea a;
    a.x0 = 0;
    a.x1 = w;
    a.y0 = 0;
    a.y1 = std::max(1, h - kHeadSwitchLines);
    if (w < 16 || a.y1 < 16) return a;
    // Column and row brightness over the picture without the bottom lines.
    std::vector<uint32_t> colSum(size_t(w), 0), colMax(size_t(w), 0), rowSum(size_t(a.y1), 0), rowMax(size_t(a.y1), 0);
    uint32_t frameMax = 0;
    for (int r = 0; r < a.y1; ++r) {
        const uint8_t* row = m_y.data() + size_t(r) * size_t(w);
        uint32_t rs = 0, rm = 0;
        for (int x = 0; x < w; ++x) {
            const uint32_t v = row[x];
            colSum[size_t(x)] += v;
            if (v > colMax[size_t(x)]) colMax[size_t(x)] = v;
            rs += v;
            if (v > rm) rm = v;
        }
        rowSum[size_t(r)] = rs;
        rowMax[size_t(r)] = rm;
        if (rm > frameMax) frameMax = rm;
    }
    // A frame that is dark all over has no border to find.
    if (frameMax <= uint32_t(kBorderMax)) return a;
    auto colDark = [&](int x) {
        return double(colSum[size_t(x)]) / double(a.y1) < kBorderMean && colMax[size_t(x)] < uint32_t(kBorderMax);
    };
    auto rowDark = [&](int r) {
        return double(rowSum[size_t(r)]) / double(w) < kBorderMean && rowMax[size_t(r)] < uint32_t(kBorderMax);
    };
    // Blanking borders are narrow; a wide dark band at an edge is picture (a black bar).
    const int maxSide = w / 24;
    while (a.x0 < maxSide && colDark(a.x0)) ++a.x0;
    while (w - a.x1 < maxSide && colDark(a.x1 - 1)) --a.x1;
    const int maxTop = h / 24;
    while (a.y0 < maxTop && rowDark(a.y0)) ++a.y0;
    // Chroma pairs stay whole: start on an even column.
    a.x0 &= ~1;
    if (a.x1 - a.x0 < 8) {
        a.x0 = 0;
        a.x1 = w;
    }
    return a;
}

void FrameAnalyzer::measure(FrameStats& out) const {
    const int w = m_fmt.width;
    const ActiveArea a = detectArea();
    out = FrameStats{};
    out.analyzed = true;
    out.area = a;
    uint64_t sum = 0, sum2 = 0;
    for (int r = a.y0; r < a.y1; ++r) {
        const uint8_t* row = m_y.data() + size_t(r) * size_t(w);
        for (int x = a.x0; x < a.x1; ++x) {
            const uint32_t v = row[x];
            ++out.luma[v];
            sum += v;
            sum2 += uint64_t(v) * v;
        }
    }
    const uint64_t n = uint64_t(a.width()) * uint64_t(a.height());
    out.pixels = n;
    for (int v = 0; v < 16; ++v) out.below += out.luma[size_t(v)];
    for (int v = 236; v < 256; ++v) out.above += out.luma[size_t(v)];
    if (n) {
        out.mean = double(sum) / double(n);
        out.stddev = std::sqrt(std::max(0.0, double(sum2) / double(n) - out.mean * out.mean));
    }
    out.blank = out.stddev < kBlankStddev;
    if (!m_cw) return;
    out.hasChroma = true;
    const int cx0 = a.x0 / 2, cx1 = std::max(cx0 + 1, a.x1 / 2);
    const bool half = m_ch < m_fmt.height;
    const int cy0 = half ? a.y0 / 2 : a.y0, cy1 = half ? (a.y1 + 1) / 2 : a.y1;
    const DistanceTable& t = distances();
    for (int r = cy0; r < cy1 && r < m_ch; ++r) {
        const uint8_t* cb = m_cb.data() + size_t(r) * size_t(m_cw);
        const uint8_t* cr = m_cr.data() + size_t(r) * size_t(m_cw);
        for (int x = cx0; x < cx1 && x < m_cw; ++x) {
            const uint8_t d = t.d[cb[x]][cr[x]];
            ++out.sat[d];
            ++out.chromaSamples;
            if (d > m_limit) ++out.chromaIllegal;
        }
    }
}

void FrameAnalyzer::downsample(const ActiveArea& a, ScopeFrame& s) const {
    const int w = m_fmt.width, h = m_fmt.height;
    const int sx = w > 400 ? 2 : 1;
    const int sy = h > 300 ? 2 : 1;
    s.width = w / sx;
    s.height = h / sy;
    s.y.resize(size_t(s.width) * size_t(s.height));
    for (int r = 0; r < s.height; ++r) {
        const uint8_t* src = m_y.data() + size_t(r * sy) * size_t(w);
        uint8_t* dst = s.y.data() + size_t(r) * size_t(s.width);
        for (int x = 0; x < s.width; ++x) dst[x] = src[x * sx];
    }
    s.area = ActiveArea{a.x0 / sx, a.x1 / sx, a.y0 / sy, a.y1 / sy};
    if (!m_cw) {
        s.cwidth = s.cheight = 0;
        s.cb.clear();
        s.cr.clear();
        return;
    }
    const bool half = m_ch < h;
    const int csy = half ? std::max(1, sy / 2) : sy;
    s.cwidth = m_cw / sx;
    s.cheight = m_ch / csy;
    s.cb.resize(size_t(s.cwidth) * size_t(s.cheight));
    s.cr.resize(s.cb.size());
    for (int r = 0; r < s.cheight; ++r) {
        const size_t srow = size_t(r * csy) * size_t(m_cw);
        for (int x = 0; x < s.cwidth; ++x) {
            s.cb[size_t(r) * size_t(s.cwidth) + size_t(x)] = m_cb[srow + size_t(x * sx)];
            s.cr[size_t(r) * size_t(s.cwidth) + size_t(x)] = m_cr[srow + size_t(x * sx)];
        }
    }
}

bool FrameAnalyzer::analyze(const uint8_t* data, size_t size, FrameStats& out, ScopeFrame* scope) {
    if (!load(data, size)) {
        out = FrameStats{};
        return false;
    }
    measure(out);
    if (scope) downsample(out.area, *scope);
    return true;
}

}  // namespace pd
