// SPDX-License-Identifier: GPL-3.0-or-later
// Lossless repacking from what the driver delivers to what the encoder takes.
// Every function here moves samples around; none of them changes a value.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pd {

struct Planes8 {
    uint8_t* y;
    uint8_t* u;
    uint8_t* v;
    int ys, us, vs;  // strides in bytes
};

struct Planes16 {
    uint16_t* y;
    uint16_t* u;
    uint16_t* v;
    int ys, us, vs;  // strides in samples
};

// Packed 4:2:2 8-bit to planar 4:2:2.
void yuyvToPlanar(const uint8_t* src, int srcStride, int w, int h, const Planes8& dst);
void uyvyToPlanar(const uint8_t* src, int srcStride, int w, int h, const Planes8& dst);

// Y210: 16-bit little-endian words Y0 U Y1 V with the value in the top 10 bits.
void y210ToPlanar10(const uint8_t* src, int srcStride, int w, int h, const Planes16& dst);

// v210: 6 pixels in four 32-bit little-endian words; rows padded to 128 bytes.
void v210ToPlanar10(const uint8_t* src, int srcStride, int w, int h, const Planes16& dst);
int v210Stride(int w);

// NV12 (Y plane then interleaved UV at half height) to planar 4:2:0.
void nv12ToPlanar(const uint8_t* src, int srcStride, int w, int h, const Planes8& dst);

// HM12, the cx18/ivtv "MPEG tiled" 4:2:0: luma in 16x16 blocks, then
// interleaved UV in 16x8 blocks, blocks in raster order.
void hm12ToPlanar(const uint8_t* src, int w, int h, const Planes8& dst);
size_t hm12Size(int w, int h);

// Weaves two sequential fields (top field buffer first or bottom first) into
// one interleaved frame. Lossless, used for V4L2_FIELD_SEQ_TB/BT.
void weaveFields(const uint8_t* src, int lineBytes, int h, bool topFirst, uint8_t* dst);

// Mean and variance of luma over a sparse grid, used for the static-picture
// stop condition and the histogram.
struct LumaStats {
    double mean = 0;
    double variance = 0;
};
enum class LumaLayout { YUYV, UYVY, Planar8, Y210, V210 };
LumaStats lumaStats(const uint8_t* src, int stride, int w, int h, LumaLayout layout, int step = 8);

}  // namespace pd
