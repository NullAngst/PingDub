// SPDX-License-Identifier: GPL-3.0-or-later
// The decisions the V4L2 backend makes, kept free of ioctls so they can be
// unit tested: which pixel format to take, what field order a driver's field
// type means, what pixel aspect a capture has, which frame size is native.
#pragma once

#include "engine/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pd {

PixFmt pixFmtFromFourcc(uint32_t fourcc);
uint32_t fourccFromPixFmt(PixFmt f);
std::string fourccString(uint32_t fourcc);

// Lower is better: 10-bit 4:2:2, YUYV, UYVY, other native formats, MJPEG last.
int pixFmtRank(PixFmt f);
PixFmt choosePixFmt(const std::vector<PixFmt>& offered);

struct FieldDecision {
    FieldOrder order = FieldOrder::Unknown;
    bool weave = false;         // sequential fields: weave into one frame (lossless)
    bool topFieldFirstInBuffer = true;
    bool supported = true;      // V4L2_FIELD_ALTERNATE etc. cannot be used
};
FieldDecision fieldFromV4l2(uint32_t v4l2Field, bool lines525);
// The field type a device really sends, for drivers known to report the wrong
// one. Returns `v4l2Field` unchanged for everything else.
uint32_t correctedField(const std::string& driver, uint32_t v4l2Field);
const char* v4l2FieldName(uint32_t f);

// Pixel aspect from how the device samples a line. BT.601's 13.5 MHz gives
// 10:11 (525) or 12:11 (625) whatever the width; square-pixel devices
// (640 or 768 wide) are 1:1. `chipOverride` wins when valid.
Rational sarFor(int width, bool lines525, const Rational& chipOverride = {});

struct StdInfo {
    std::string name;
    Rational fps;
    bool lines525;
};
StdInfo describeStd(uint64_t v4l2StdId);
// A QUERYSTD answer worth showing: the name when the mask fits one standard
// or family, "" when the driver could not narrow it down (no signal, still
// detecting, or every standard it supports).
std::string detectedStdName(uint64_t v4l2StdId);
bool stdIs525(uint64_t v4l2StdId);

struct Size {
    int w = 0, h = 0;
    bool operator==(const Size& o) const { return w == o.w && h == o.h; }
};
// The size to ask for: the crop bounds if known (no scaling), else the
// largest offered size for the standard's line count, else the standard default.
Size chooseFrameSize(const Size& cropBounds, const std::vector<Size>& offered, bool lines525);

}  // namespace pd
