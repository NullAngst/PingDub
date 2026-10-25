// SPDX-License-Identifier: GPL-3.0-or-later
// A grid of thumbnails saved next to the capture, so what is on a tape shows
// without opening it. One thumbnail every N seconds (30 by default), 320
// pixels wide with the aspect corrected, the timecode under each, 6 across
// and 10 down, so a 2-hour tape fills four JPEG pages. Thumbnails come from
// the frames the analyzer already has in memory during the capture; nothing
// is decoded again afterward. JPEGs are written with libavcodec's MJPEG encoder.
#pragma once

#include "FrameAnalyzer.h"
#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;

namespace pd {

struct ContactThumb {
    int64_t slot = 0;
    int page = 0;   // 0-based
    int index = 0;  // position on the page, row-major
};

struct ContactSheetInfo {
    double intervalSec = 0;
    int tileWidth = 320, tileHeight = 240;
    int columns = 6, rows = 10;
    int gap = 8, labelHeight = 22;
    std::vector<std::string> pages;
    std::vector<ContactThumb> thumbs;
    bool empty() const { return thumbs.empty(); }
    // Top-left corner of a thumbnail on its page.
    int tileX(int index) const { return gap + (index % columns) * (tileWidth + gap); }
    int tileY(int index) const { return gap + (index / columns) * (tileHeight + labelHeight + gap); }
};

class ContactSheet {
public:
    static constexpr int kColumns = 6, kRows = 10, kTileWidth = 320, kGap = 8, kLabel = 22;

    ~ContactSheet();
    // basePath is the capture path without extension; pages become
    // <basePath>.contact01.jpg and so on.
    bool begin(const std::string& basePath, const VideoFormat& fmt, double intervalSec, std::string& err);
    bool active() const { return m_active; }
    bool due(int64_t slot) const { return m_active && slot >= m_next; }
    // Adds the thumbnail for `slot` from the planes of that frame.
    void add(const PlaneView& p, int64_t slot);
    // Writes the last, partly filled page.
    void finish();
    const ContactSheetInfo& info() const { return m_info; }
    std::string error() const { return m_error; }

    static int tileHeightFor(const VideoFormat& fmt, int tileWidth);
    // Draws "HH:MM:SS" with the built-in digit font, 2x scale, into an 8-bit plane.
    static void drawClock(uint8_t* plane, int stride, int x, int y, const std::string& text, uint8_t value);
    static int clockWidth(const std::string& text);

private:
    bool writePage(int rowsUsed);
    void clearPage();

    bool m_active = false;
    std::string m_base;
    Rational m_fps{30000, 1001};
    int64_t m_interval = 900;
    int64_t m_next = 0;
    int m_pageW = 0, m_pageH = 0;
    int m_onPage = 0;
    std::vector<uint8_t> m_py, m_pu, m_pv;  // full-range 4:2:0 page
    ContactSheetInfo m_info;
    std::string m_error;
};

}  // namespace pd
