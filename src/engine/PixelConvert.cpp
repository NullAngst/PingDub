// SPDX-License-Identifier: GPL-3.0-or-later
#include "PixelConvert.h"

#include <cstring>

namespace pd {

void yuyvToPlanar(const uint8_t* src, int srcStride, int w, int h, const Planes8& d) {
    const int pairs = w / 2;
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + size_t(row) * srcStride;
        uint8_t* y = d.y + size_t(row) * d.ys;
        uint8_t* u = d.u + size_t(row) * d.us;
        uint8_t* v = d.v + size_t(row) * d.vs;
        for (int i = 0; i < pairs; ++i) {
            y[2 * i] = s[4 * i + 0];
            u[i] = s[4 * i + 1];
            y[2 * i + 1] = s[4 * i + 2];
            v[i] = s[4 * i + 3];
        }
    }
}

void uyvyToPlanar(const uint8_t* src, int srcStride, int w, int h, const Planes8& d) {
    const int pairs = w / 2;
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + size_t(row) * srcStride;
        uint8_t* y = d.y + size_t(row) * d.ys;
        uint8_t* u = d.u + size_t(row) * d.us;
        uint8_t* v = d.v + size_t(row) * d.vs;
        for (int i = 0; i < pairs; ++i) {
            u[i] = s[4 * i + 0];
            y[2 * i] = s[4 * i + 1];
            v[i] = s[4 * i + 2];
            y[2 * i + 1] = s[4 * i + 3];
        }
    }
}

static inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void y210ToPlanar10(const uint8_t* src, int srcStride, int w, int h, const Planes16& d) {
    const int pairs = w / 2;
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + size_t(row) * srcStride;
        uint16_t* y = d.y + size_t(row) * d.ys;
        uint16_t* u = d.u + size_t(row) * d.us;
        uint16_t* v = d.v + size_t(row) * d.vs;
        for (int i = 0; i < pairs; ++i) {
            y[2 * i] = rd16(s + 8 * i + 0) >> 6;
            u[i] = rd16(s + 8 * i + 2) >> 6;
            y[2 * i + 1] = rd16(s + 8 * i + 4) >> 6;
            v[i] = rd16(s + 8 * i + 6) >> 6;
        }
    }
}

int v210Stride(int w) { return ((w + 47) / 48) * 128; }

void v210ToPlanar10(const uint8_t* src, int srcStride, int w, int h, const Planes16& d) {
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + size_t(row) * srcStride;
        uint16_t* y = d.y + size_t(row) * d.ys;
        uint16_t* u = d.u + size_t(row) * d.us;
        uint16_t* v = d.v + size_t(row) * d.vs;
        int px = 0;
        // Each 16-byte group holds Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5.
        for (; px + 6 <= w; px += 6, s += 16) {
            const uint32_t w0 = rd32(s), w1 = rd32(s + 4), w2 = rd32(s + 8), w3 = rd32(s + 12);
            const int c = px / 2;
            u[c] = w0 & 0x3ff;
            y[px] = (w0 >> 10) & 0x3ff;
            v[c] = (w0 >> 20) & 0x3ff;
            y[px + 1] = w1 & 0x3ff;
            u[c + 1] = (w1 >> 10) & 0x3ff;
            y[px + 2] = (w1 >> 20) & 0x3ff;
            v[c + 1] = w2 & 0x3ff;
            y[px + 3] = (w2 >> 10) & 0x3ff;
            u[c + 2] = (w2 >> 20) & 0x3ff;
            y[px + 4] = w3 & 0x3ff;
            v[c + 2] = (w3 >> 10) & 0x3ff;
            y[px + 5] = (w3 >> 20) & 0x3ff;
        }
        if (px < w) {
            // Partial last group (width not a multiple of 6).
            uint16_t ys[6], cb[3], cr[3];
            const uint32_t w0 = rd32(s), w1 = rd32(s + 4), w2 = rd32(s + 8), w3 = rd32(s + 12);
            cb[0] = w0 & 0x3ff; ys[0] = (w0 >> 10) & 0x3ff; cr[0] = (w0 >> 20) & 0x3ff;
            ys[1] = w1 & 0x3ff; cb[1] = (w1 >> 10) & 0x3ff; ys[2] = (w1 >> 20) & 0x3ff;
            cr[1] = w2 & 0x3ff; ys[3] = (w2 >> 10) & 0x3ff; cb[2] = (w2 >> 20) & 0x3ff;
            ys[4] = w3 & 0x3ff; cr[2] = (w3 >> 10) & 0x3ff; ys[5] = (w3 >> 20) & 0x3ff;
            for (int i = 0; px + i < w; ++i) {
                y[px + i] = ys[i];
                if ((i & 1) == 0) {
                    u[(px + i) / 2] = cb[i / 2];
                    v[(px + i) / 2] = cr[i / 2];
                }
            }
        }
    }
}

void nv12ToPlanar(const uint8_t* src, int srcStride, int w, int h, const Planes8& d) {
    for (int row = 0; row < h; ++row) std::memcpy(d.y + size_t(row) * d.ys, src + size_t(row) * srcStride, size_t(w));
    const uint8_t* uv = src + size_t(srcStride) * h;
    for (int row = 0; row < h / 2; ++row) {
        const uint8_t* s = uv + size_t(row) * srcStride;
        uint8_t* u = d.u + size_t(row) * d.us;
        uint8_t* v = d.v + size_t(row) * d.vs;
        for (int i = 0; i < w / 2; ++i) {
            u[i] = s[2 * i];
            v[i] = s[2 * i + 1];
        }
    }
}

size_t hm12Size(int w, int h) {
    const int aw = (w + 15) & ~15, ah = (h + 15) & ~15;
    const int ach = ((h / 2) + 15) & ~15;
    return size_t(aw) * ah + size_t(aw) * ach;
}

void hm12ToPlanar(const uint8_t* src, int w, int h, const Planes8& d) {
    const int aw = (w + 15) & ~15, ah = (h + 15) & ~15;
    const uint8_t* s = src;
    // Luma: 16x16 tiles, each tile 16 lines of 16 bytes.
    for (int ty = 0; ty < ah; ty += 16) {
        for (int tx = 0; tx < aw; tx += 16) {
            for (int i = 0; i < 16; ++i, s += 16) {
                const int row = ty + i;
                if (row >= h || tx >= w) continue;
                const int n = (tx + 16 <= w) ? 16 : (w - tx);
                std::memcpy(d.y + size_t(row) * d.ys + tx, s, size_t(n));
            }
        }
    }
    // Chroma: interleaved CbCr plane, half height, cut into 16-byte by
    // 16-line tiles (8 CbCr pairs wide).
    s = src + size_t(aw) * ah;
    const int ch = h / 2, ach = (ch + 15) & ~15;
    for (int ty = 0; ty < ach; ty += 16) {
        for (int tx = 0; tx < aw; tx += 16) {
            for (int i = 0; i < 16; ++i, s += 16) {
                const int row = ty + i;
                if (row >= ch) continue;
                for (int j = 0; j < 8; ++j) {
                    const int cx = tx / 2 + j;
                    if (cx >= w / 2) break;
                    d.u[size_t(row) * d.us + cx] = s[2 * j];
                    d.v[size_t(row) * d.vs + cx] = s[2 * j + 1];
                }
            }
        }
    }
}

void weaveFields(const uint8_t* src, int lineBytes, int h, bool topFirst, uint8_t* dst) {
    const int fieldLines = h / 2;
    const uint8_t* first = src;
    const uint8_t* second = src + size_t(lineBytes) * fieldLines;
    const uint8_t* top = topFirst ? first : second;
    const uint8_t* bottom = topFirst ? second : first;
    for (int i = 0; i < fieldLines; ++i) {
        std::memcpy(dst + size_t(2 * i) * lineBytes, top + size_t(i) * lineBytes, size_t(lineBytes));
        std::memcpy(dst + size_t(2 * i + 1) * lineBytes, bottom + size_t(i) * lineBytes, size_t(lineBytes));
    }
}

LumaStats lumaStats(const uint8_t* src, int stride, int w, int h, LumaLayout layout, int step) {
    double sum = 0, sum2 = 0;
    uint64_t n = 0;
    for (int row = 0; row < h; row += step) {
        const uint8_t* s = src + size_t(row) * stride;
        for (int x = 0; x < w; x += step) {
            double yv = 0;
            switch (layout) {
            case LumaLayout::YUYV: yv = s[2 * x]; break;
            case LumaLayout::UYVY: yv = s[2 * x + 1]; break;
            case LumaLayout::Planar8: yv = s[x]; break;
            case LumaLayout::Y210: yv = (rd16(s + 4 * x) >> 6) / 4.0; break;
            case LumaLayout::V210: {
                // luma position inside the 6-pixel group, coarse but fine for stats
                const uint8_t* g = s + (x / 6) * 16;
                yv = ((rd32(g) >> 10) & 0x3ff) / 4.0;
                break;
            }
            }
            sum += yv;
            sum2 += yv * yv;
            ++n;
        }
    }
    LumaStats st;
    if (n) {
        st.mean = sum / double(n);
        st.variance = sum2 / double(n) - st.mean * st.mean;
    }
    return st;
}

}  // namespace pd
