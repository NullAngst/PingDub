// SPDX-License-Identifier: GPL-3.0-or-later
// Core data types shared by backends, the sync engine and the output path.
// Nothing in engine/ includes Qt, so tests and --self-test link it directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pd {

struct Rational {
    int num = 0;
    int den = 1;
    double toDouble() const { return den ? double(num) / double(den) : 0.0; }
    bool valid() const { return num > 0 && den > 0; }
    bool operator==(const Rational& o) const { return int64_t(num) * o.den == int64_t(o.num) * den; }
};

// Pixel formats PingDub can capture. Everything here maps to a lossless
// encoder input without touching pixel values.
enum class PixFmt {
    Unknown,
    YUYV,   // packed 4:2:2 8-bit, Y0 U Y1 V
    UYVY,   // packed 4:2:2 8-bit, U Y0 V Y1
    Y210,   // packed 4:2:2 16-bit words, 10 MSBs used, Y0 U Y1 V
    V210,   // packed 4:2:2 10-bit, 6 pixels in 16 bytes (DeckLink bmdFormat10BitYUV)
    HM12,   // cx18/ivtv macroblock tiled 4:2:0
    NV12,   // semi-planar 4:2:0
    GREY,   // 8-bit luma only
    MJPEG,  // compressed by the device, stored as-is
};

const char* pixFmtName(PixFmt f);
bool pixFmtIs10Bit(PixFmt f);
bool pixFmtIs420(PixFmt f);

enum class FieldOrder { Unknown, Progressive, TFF, BFF };
const char* fieldOrderName(FieldOrder f);

// Audio sample formats as delivered by the device. Internally every sample is
// carried as int32 with the device's bits in the most significant positions.
enum class SampleFmt { S16, S24, S32 };
int sampleFmtBits(SampleFmt f);
const char* sampleFmtName(SampleFmt f);

enum class SyncMode { VideoMaster, AudioMaster, NoCorrection };
const char* syncModeName(SyncMode m);
enum class GapFill { LastFrame, Black };

struct VideoFormat {
    PixFmt pixfmt = PixFmt::Unknown;
    int width = 0;
    int height = 0;
    int bytesPerLine = 0;   // stride of plane 0 as delivered
    size_t frameBytes = 0;  // max size of one frame buffer
    Rational fps{30000, 1001};
    FieldOrder field = FieldOrder::Unknown;
    bool lines525 = true;   // 525-line (NTSC-like) vs 625-line timing
    Rational sar{10, 11};
    std::string standardName = "NTSC-M";
};

struct AudioFormat {
    int rate = 48000;
    int channels = 2;
    SampleFmt fmt = SampleFmt::S16;
    int bytesPerSample() const { return fmt == SampleFmt::S16 ? 2 : 4; }
    int bytesPerFrame() const { return bytesPerSample() * channels; }
};

// Reference counted byte buffer, recycled through BufferPool.
using Bytes = std::vector<uint8_t>;
using BytesPtr = std::shared_ptr<Bytes>;

enum VideoFlags : uint32_t {
    VF_None = 0,
    VF_Error = 1u << 0,      // driver set V4L2_BUF_FLAG_ERROR: damaged, kept and logged
    VF_Fill = 1u << 1,       // synthetic slot from the stall watchdog, no data
};

struct VideoFrame {
    BytesPtr data;           // null for VF_Fill
    size_t size = 0;
    int64_t tsNs = 0;        // CLOCK_MONOTONIC nanoseconds
    uint32_t seq = 0;        // driver sequence number
    uint32_t flags = 0;
    uint32_t ringDropsBefore = 0;  // frames discarded at the ring right before this one
};

enum AudioFlags : uint32_t {
    AF_None = 0,
    AF_XrunFill = 1u << 0,   // silence inserted by the audio thread to cover an xrun
};

// Interleaved int32 samples, device bits left-justified.
struct AudioChunk {
    std::shared_ptr<std::vector<int32_t>> samples;
    uint32_t frames = 0;     // sample frames (samples / channels)
    int64_t tsNs = 0;        // monotonic time of the first sample
    uint32_t flags = 0;
};

// Raw VBI or sliced VBI buffer, mapped to the timeline by timestamp.
struct VbiBuffer {
    BytesPtr data;
    size_t size = 0;
    int64_t tsNs = 0;
    uint32_t seq = 0;
    bool sliced = false;
};

}  // namespace pd
