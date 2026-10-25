// SPDX-License-Identifier: GPL-3.0-or-later
// libavcodec wrappers. Video is always lossless and keeps the capture's own
// chroma layout and bit depth; audio is PCM or FLAC at the device's depth.
#pragma once

#include "CaptureConfig.h"
#include "Types.h"

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace pd {

// The pixel format handed to the encoder for a given capture format.
AVPixelFormat encoderPixFmt(PixFmt src, VideoCodec codec);
// libavcodec encoder name for the codec in this container.
const char* encoderName(VideoCodec codec, Container container, PixFmt src);
// Checks the codec accepts the pixel format without conversion.
bool videoCodecSupports(VideoCodec codec, Container container, PixFmt src, std::string& why);

class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool open(const VideoFormat& fmt, VideoCodec codec, Container container, int threads, std::string& err);
    const AVCodecParameters* params() const { return m_par; }
    AVRational timeBase() const { return m_tb; }

    // Encodes one timeline slot. A frame with null data becomes black.
    // `pts` is in frame units. Packets are appended to `out` and owned by the caller.
    bool encode(const VideoFrame& f, int64_t pts, std::vector<AVPacket*>& out, std::string& err);
    bool flush(std::vector<AVPacket*>& out);
    std::string description() const { return m_desc; }
    // MD5 of each frame's raw picture before encoding, for the .framemd5 sidecar.
    void setFrameHashes(bool on) { m_hash = on; }
    // The hash of the frame passed to the last encode(); false for MJPEG passthrough.
    bool lastFrameHash(uint8_t md5[16], int& size) const;
    bool passthrough() const { return m_passthrough; }

private:
    bool fillFrame(AVFrame* fr, const VideoFrame& f);
    void fillBlack(AVFrame* fr);
    bool drain(std::vector<AVPacket*>& out, int64_t pts);

    VideoFormat m_fmt;
    AVCodecContext* m_ctx = nullptr;
    AVCodecParameters* m_par = nullptr;
    AVRational m_tb{1001, 30000};
    bool m_passthrough = false;  // MJPEG stored as delivered
    // Every codec here is intra-only, so a repeated input gives a repeated
    // packet. Inserted frames reuse the last packet instead of re-encoding.
    const void* m_lastSrc = reinterpret_cast<const void*>(-1);
    AVPacket* m_lastPkt = nullptr;
    std::string m_desc;
    bool m_hash = false;
    bool m_hashValid = false;
    uint8_t m_md5[16] = {};
    int m_md5Size = 0;
};

class AudioEncoder {
public:
    AudioEncoder() = default;
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    bool open(const AudioFormat& fmt, AudioCodec codec, Container container, std::string& err);
    const AVCodecParameters* params() const { return m_par; }
    AVRational timeBase() const { return AVRational{1, m_fmt.rate}; }

    // Interleaved int32, device bits left-justified. pts counts samples from
    // the start of this encoder's segment.
    bool push(const int32_t* samples, size_t frames, std::vector<AVPacket*>& out);
    bool flush(std::vector<AVPacket*>& out);
    uint64_t samplesIn() const { return m_total; }
    std::string description() const { return m_desc; }

private:
    bool encodeFrames(const int32_t* s, size_t frames, std::vector<AVPacket*>& out);
    bool drain(std::vector<AVPacket*>& out);

    AudioFormat m_fmt;
    AVCodecContext* m_ctx = nullptr;
    AVCodecParameters* m_par = nullptr;
    int m_frameSize = 1024;
    std::vector<int32_t> m_fifo;
    int64_t m_pts = 0;
    uint64_t m_total = 0;
    std::string m_desc;
};

std::string avErr(int code);

}  // namespace pd
