// SPDX-License-Identifier: GPL-3.0-or-later
// Line 21 closed captions: SCC output from the raw byte pairs, and SRT text
// from zvbi's caption decoder. zvbi also decodes XDS (program name, rating).
#pragma once

#include "Types.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct vbi_decoder;
struct vbi_event;

namespace pd {

// Collects field-1 byte pairs per timeline frame and writes Scenarist SCC.
// SCC carries CC1 and CC2 together; that is what tools expect in a .cc1.scc.
class SccWriter {
public:
    void setRate(const Rational& fps) { m_fps = fps; }
    void add(int64_t slot, uint8_t b1, uint8_t b2);  // bytes as received, parity intact
    bool empty() const { return m_pairs.empty(); }
    std::string text() const;
    static std::string timecode(int64_t frame, const Rational& fps);  // drop-frame for 29.97

private:
    Rational m_fps{30000, 1001};
    std::vector<std::pair<int64_t, uint16_t>> m_pairs;
};

struct SrtCue {
    double start = 0;
    double end = 0;
    std::string text;
};

struct XdsInfo {
    std::string title;
    std::string rating;
};

// Feeds sliced caption lines to zvbi and turns screen changes into SRT cues
// for CC1..CC4.
class CaptionTracker {
public:
    CaptionTracker();
    ~CaptionTracker();
    CaptionTracker(const CaptionTracker&) = delete;
    CaptionTracker& operator=(const CaptionTracker&) = delete;

    // field: 1 or 2. Bytes with parity as received.
    void feed(double timeSec, const std::vector<std::pair<int, std::pair<uint8_t, uint8_t>>>& lines);
    void finish(double timeSec);

    // Cues per channel 1..4 (CC1..CC4).
    const std::map<int, std::vector<SrtCue>>& cues() const { return m_cues; }
    static std::string srt(const std::vector<SrtCue>& cues);
    std::string liveText(int channel) const;  // current screen of a channel
    XdsInfo xds() const;

    void onEvent(const vbi_event* ev);

private:
    std::string pageText(int pgno);
    vbi_decoder* m_dec = nullptr;
    double m_now = 0;
    std::map<int, std::string> m_screen;
    std::map<int, double> m_since;
    std::map<int, std::vector<SrtCue>> m_cues;
    mutable std::mutex m_mutex;
    std::map<int, std::string> m_live;
    XdsInfo m_xds;
};

// Odd parity helper for building test streams and checking bytes.
uint8_t withOddParity(uint8_t b7);
bool oddParityOk(uint8_t b);

}  // namespace pd
