// SPDX-License-Identifier: GPL-3.0-or-later
// Everything the capture report shows, read back from a capture's sidecars:
// the JSON (settings, totals, chapters, contact sheet, segments) and the
// .log.csv (every event with its timeline position). Captures made before
// these sidecars carried level and audio events load fine; the report just
// has less to show.
#pragma once

#include "Chapters.h"
#include "ContactSheet.h"
#include "Types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pd {

enum class ReportLane { Video, Levels, Audio, Info };

struct RecordEvent {
    std::string event;       // name as in the CSV
    int64_t slot = -1;       // timeline frame, -1 if none
    int64_t endSlot = -1;    // last slot it covers
    uint64_t count = 0;
    std::string detail;
    std::string wallclock;
    ReportLane lane = ReportLane::Info;
    std::string label;       // human name
};

struct RecordSegment {
    std::string file;
    int64_t startSlot = 0;
    int64_t frames = -1;     // -1 if unknown
};

struct CaptureRecord {
    std::string mediaPath;   // the file the user opened
    std::string basePath;    // folder/stem shared by the sidecars
    std::string jsonPath, csvPath;
    bool haveJson = false, haveCsv = false;
    Rational fps{30000, 1001};
    int64_t frames = 0;
    double seconds = 0;
    std::string label, startTime, stopReason, device, input, standard;
    std::vector<RecordSegment> segments;
    std::vector<RecordEvent> events;     // every event, Info included
    std::vector<Chapter> chapters;
    ContactSheetInfo sheet;              // page paths absolute
    std::map<std::string, double> totals;
    std::vector<std::string> warnings;
    int spots = 0;

    bool clean() const { return spots == 0; }
    std::string verdict() const;
    std::string clock(int64_t slot) const;  // HH:MM:SS.mmm
    // The file and the frame inside it for a timeline slot.
    bool locate(int64_t slot, std::string& file, int64_t& frameInFile) const;
};

// Lane and label for an event name from the CSV.
ReportLane laneOf(const std::string& event, std::string& label);
// Strips the extension and any _partNNN suffix.
std::string captureBaseFor(const std::string& path);
// Accepts the media file, its JSON or its CSV.
bool loadCaptureRecord(const std::string& path, CaptureRecord& out, std::string& err);
// Reviewable events closer than `gapSec` count as one spot.
int countSpots(const std::vector<RecordEvent>& ev, const Rational& fps, double gapSec = 5.0);
bool parseCsvLine(const std::string& line, std::vector<std::string>& fields);

struct RgbImage {
    int width = 0, height = 0;
    Rational sar{1, 1};
    std::vector<uint8_t> rgb;  // RGB888, rows packed
};
// Decodes frame `index` (0-based, in that file) to RGB with BT.601 math.
bool decodeFrameRgb(const std::string& file, int64_t index, RgbImage& out, std::string& err);

}  // namespace pd
