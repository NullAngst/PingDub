// SPDX-License-Identifier: GPL-3.0-or-later
// Plain settings structs. The UI fills them from profiles; the engine and
// tests use them directly.
#pragma once

#include "Types.h"

#include <cstdint>
#include <map>
#include <string>

namespace pd {

enum class Container { MKV, AVI };
enum class VideoCodec { FFV1, UTVideo, Huff, Raw };
enum class AudioCodec { PCM, FLAC };

const char* containerName(Container c);
const char* videoCodecName(VideoCodec c);
const char* audioCodecName(AudioCodec c);

struct OutputSettings {
    Container container = Container::MKV;
    VideoCodec vcodec = VideoCodec::FFV1;
    AudioCodec acodec = AudioCodec::PCM;
    std::string folder;               // default ~/Videos/PingDub
    std::string overflowFolder;       // empty: no spill
    uint64_t overflowFreeBytes = 10ull << 30;  // switch when the current drive drops below this
    std::string label;                // tape label, goes into file name and tags
    uint64_t segmentBytes = 0;        // 0: no size segmenting
    double segmentSeconds = 0;        // 0: no duration segmenting
    int threads = 0;                  // 0: all cores
};

struct StopConditions {
    double durationSec = 0;           // 0: off
    uint64_t fileBytes = 0;           // 0: off
    uint64_t minFreeBytes = 2ull << 30;  // always on: stop when disk free falls below this
    double maxDropPerMin = 0;         // 0: off. inserted+dropped+overflow per minute
    double signalLostSec = 0;         // 0: off
    double staticSec = 0;             // 0: off
    double staticStddev = 2.5;        // luma stddev below this counts as a static picture
};

struct TimingSettings {
    SyncMode sync = SyncMode::VideoMaster;
    GapFill gapFill = GapFill::LastFrame;
    int v4l2Buffers = 32;
    uint64_t ringBytes = 2ull << 30;
};

// What gets written into the file's tags and the JSON sidecar besides totals.
struct CaptureInfo {
    std::string device;
    std::string driver;
    std::string input;
    std::string standard;
    std::string audioDevice;
    std::string backend;
    std::map<std::string, std::string> controls;  // control name -> value at capture start
};

}  // namespace pd
