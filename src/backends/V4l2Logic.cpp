// SPDX-License-Identifier: GPL-3.0-or-later
#include "V4l2Logic.h"

#include <linux/videodev2.h>

#include <algorithm>

#ifndef V4L2_PIX_FMT_Y210
#define V4L2_PIX_FMT_Y210 v4l2_fourcc('Y', '2', '1', '0')
#endif
#ifndef V4L2_PIX_FMT_HM12
#define V4L2_PIX_FMT_HM12 v4l2_fourcc('H', 'M', '1', '2')
#endif

namespace pd {

PixFmt pixFmtFromFourcc(uint32_t f) {
    switch (f) {
    case V4L2_PIX_FMT_YUYV: return PixFmt::YUYV;
    case V4L2_PIX_FMT_UYVY: return PixFmt::UYVY;
    case V4L2_PIX_FMT_Y210: return PixFmt::Y210;
    case V4L2_PIX_FMT_HM12: return PixFmt::HM12;  // also NV12_16L16, same fourcc
    case V4L2_PIX_FMT_NV12: return PixFmt::NV12;
    case V4L2_PIX_FMT_GREY: return PixFmt::GREY;
    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_JPEG: return PixFmt::MJPEG;
    default: return PixFmt::Unknown;
    }
}

uint32_t fourccFromPixFmt(PixFmt f) {
    switch (f) {
    case PixFmt::YUYV: return V4L2_PIX_FMT_YUYV;
    case PixFmt::UYVY: return V4L2_PIX_FMT_UYVY;
    case PixFmt::Y210: return V4L2_PIX_FMT_Y210;
    case PixFmt::HM12: return V4L2_PIX_FMT_HM12;
    case PixFmt::NV12: return V4L2_PIX_FMT_NV12;
    case PixFmt::GREY: return V4L2_PIX_FMT_GREY;
    case PixFmt::MJPEG: return V4L2_PIX_FMT_MJPEG;
    default: return 0;
    }
}

std::string fourccString(uint32_t f) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = char((f >> (8 * i)) & 0xff);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

int pixFmtRank(PixFmt f) {
    switch (f) {
    case PixFmt::Y210:
    case PixFmt::V210: return 0;
    case PixFmt::YUYV: return 1;
    case PixFmt::UYVY: return 2;
    case PixFmt::HM12: return 3;
    case PixFmt::NV12: return 4;
    case PixFmt::GREY: return 5;
    case PixFmt::MJPEG: return 9;
    default: return 100;
    }
}

PixFmt choosePixFmt(const std::vector<PixFmt>& offered) {
    PixFmt best = PixFmt::Unknown;
    for (PixFmt f : offered)
        if (pixFmtRank(f) < pixFmtRank(best)) best = f;
    return best;
}

FieldDecision fieldFromV4l2(uint32_t f, bool lines525) {
    FieldDecision d;
    switch (f) {
    case V4L2_FIELD_INTERLACED_TB: d.order = FieldOrder::TFF; break;
    case V4L2_FIELD_INTERLACED_BT: d.order = FieldOrder::BFF; break;
    case V4L2_FIELD_INTERLACED:
        // The V4L2 spec: plain INTERLACED is bottom field first for NTSC, top
        // field first for everything else.
        d.order = lines525 ? FieldOrder::BFF : FieldOrder::TFF;
        break;
    case V4L2_FIELD_SEQ_TB:
        d.order = FieldOrder::TFF;
        d.weave = true;
        d.topFieldFirstInBuffer = true;
        break;
    case V4L2_FIELD_SEQ_BT:
        d.order = FieldOrder::BFF;
        d.weave = true;
        d.topFieldFirstInBuffer = false;
        break;
    case V4L2_FIELD_NONE: d.order = FieldOrder::Progressive; break;
    case V4L2_FIELD_TOP:
    case V4L2_FIELD_BOTTOM:
    case V4L2_FIELD_ALTERNATE:
        d.supported = false;
        d.order = FieldOrder::Unknown;
        break;
    default: d.order = FieldOrder::Unknown; break;
    }
    return d;
}

const char* v4l2FieldName(uint32_t f) {
    switch (f) {
    case V4L2_FIELD_ANY: return "ANY";
    case V4L2_FIELD_NONE: return "NONE";
    case V4L2_FIELD_TOP: return "TOP";
    case V4L2_FIELD_BOTTOM: return "BOTTOM";
    case V4L2_FIELD_INTERLACED: return "INTERLACED";
    case V4L2_FIELD_SEQ_TB: return "SEQ_TB";
    case V4L2_FIELD_SEQ_BT: return "SEQ_BT";
    case V4L2_FIELD_ALTERNATE: return "ALTERNATE";
    case V4L2_FIELD_INTERLACED_TB: return "INTERLACED_TB";
    case V4L2_FIELD_INTERLACED_BT: return "INTERLACED_BT";
    default: return "?";
    }
}

Rational sarFor(int width, bool lines525, const Rational& chipOverride) {
    if (chipOverride.valid()) return chipOverride;
    if (width == 640 || width == 768) return {1, 1};
    return lines525 ? Rational{10, 11} : Rational{12, 11};
}

bool stdIs525(uint64_t id) {
    // 60 Hz systems: NTSC family, PAL-M and PAL-60. SECAM and other PAL are 625.
    const v4l2_std_id s = v4l2_std_id(id);
    if (s & V4L2_STD_525_60) return true;
    return false;
}

StdInfo describeStd(uint64_t id) {
    const v4l2_std_id s = v4l2_std_id(id);
    StdInfo i;
    i.lines525 = (s & V4L2_STD_525_60) != 0 && !(s & V4L2_STD_625_50);
    if (s & V4L2_STD_525_60 && s & V4L2_STD_625_50) i.lines525 = false;  // ambiguous masks: assume 625
    i.fps = i.lines525 ? Rational{30000, 1001} : Rational{25, 1};
    struct N {
        v4l2_std_id mask;
        const char* name;
    };
    static const N names[] = {
        {V4L2_STD_NTSC_M, "NTSC-M"},       {V4L2_STD_NTSC_M_JP, "NTSC-J"}, {V4L2_STD_NTSC_443, "NTSC-443"},
        {V4L2_STD_NTSC_M_KR, "NTSC-M-KR"}, {V4L2_STD_PAL_M, "PAL-M"},      {V4L2_STD_PAL_60, "PAL-60"},
        {V4L2_STD_PAL_N, "PAL-N"},         {V4L2_STD_PAL_Nc, "PAL-Nc"},    {V4L2_STD_PAL_BG, "PAL-B/G"},
        {V4L2_STD_PAL_DK, "PAL-D/K"},      {V4L2_STD_PAL_I, "PAL-I"},      {V4L2_STD_PAL_H, "PAL-H"},
        {V4L2_STD_SECAM_B | V4L2_STD_SECAM_G | V4L2_STD_SECAM_H, "SECAM-B/G/H"},
        {V4L2_STD_SECAM_DK, "SECAM-D/K"},  {V4L2_STD_SECAM_L, "SECAM-L"},  {V4L2_STD_SECAM_LC, "SECAM-L'"},
        {V4L2_STD_NTSC, "NTSC"},           {V4L2_STD_PAL, "PAL"},          {V4L2_STD_SECAM, "SECAM"},
    };
    for (const N& n : names) {
        if (s && (s & ~n.mask) == 0) {
            i.name = n.name;
            return i;
        }
    }
    for (const N& n : names)
        if (s & n.mask) {
            i.name = n.name;
            return i;
        }
    i.name = "unknown";
    return i;
}

Size chooseFrameSize(const Size& bounds, const std::vector<Size>& offered, bool lines525) {
    const int stdH = lines525 ? 480 : 576;
    if (bounds.w > 0 && bounds.h >= stdH - 8) return bounds;
    Size best{};
    for (const Size& s : offered) {
        // Only sizes that belong to this line count (480-486 or 576).
        if (lines525 && (s.h < 470 || s.h > 490)) continue;
        if (!lines525 && (s.h < 560 || s.h > 580)) continue;
        if (s.h > best.h || (s.h == best.h && s.w > best.w)) best = s;
    }
    if (best.w) return best;
    return {720, stdH};
}

}  // namespace pd
