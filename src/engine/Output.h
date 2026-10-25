// SPDX-License-Identifier: GPL-3.0-or-later
// Receives the sync engine's timeline and turns it into files: encodes on
// its own thread, hands packets to the muxer's writer thread, splits into
// segments by size or duration, and spills to the overflow folder when the
// first drive runs low. Segments are frame- and sample-continuous.
#pragma once

#include "CaptureConfig.h"
#include "Chapters.h"
#include "Checksums.h"
#include "Encoder.h"
#include "EventLog.h"
#include "Muxer.h"
#include "RingBuffer.h"
#include "SyncEngine.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pd {

struct OutputPlan {
    OutputSettings out;
    VideoFormat vfmt;
    bool haveAudio = true;
    AudioFormat afmt;
    std::string folder;   // resolved primary folder
    std::string stem;     // file name without extension or part suffix
    std::map<std::string, std::string> tags;  // written as-is
    std::vector<std::string> deferredTags;     // filled in when the capture ends
    bool frameHashes = true;                   // write a .framemd5 next to each file
    // Chapters for the segment [start, end) when it closes; `final` for the last one.
    std::function<std::vector<Chapter>(int64_t start, int64_t end, bool final)> chapters;
};

struct SegmentRecord {
    std::string path;
    int64_t startSlot = 0;
    int64_t frames = 0;
    std::string frameMd5;          // sidecar path, empty if none
    std::vector<Chapter> chapters; // relative to the segment start
    std::string chaptersTxt;       // AVI: FFmpeg metadata sidecar
};

class OutputWriter : public SyncSink {
public:
    explicit OutputWriter(EventLog* log = nullptr);
    ~OutputWriter() override;

    bool open(const OutputPlan& plan, std::string& err);
    // Flushes everything, closes the last segment, and writes `finalTags`
    // into the deferred placeholders of every Matroska segment.
    void close(const std::map<std::string, std::string>& finalTags);

    // SyncSink, called from the sync thread. Blocks when the encoder is behind.
    void syncVideo(const VideoFrame& frame, int64_t slot, bool inserted) override;
    void syncAudio(const int32_t* samples, size_t frames) override;
    void syncEvent(const SyncEvent& ev) override;

    bool failed() const;
    std::string error() const;
    uint64_t bytesWritten() const { return m_muxer.bytesWritten(); }
    std::vector<std::string> files() const { return m_muxer.files(); }
    std::string currentFile() const { return m_muxer.currentFile(); }
    std::string currentFolder() const;
    int segmentCount() const { return m_segIndex.load(); }
    bool usingOverflow() const { return m_onOverflow.load(); }
    std::string videoDescription() const { return m_vdesc; }
    std::string audioDescription() const { return m_adesc; }
    // Frames encoded so far (for stats); lags the sync engine by the queue.
    uint64_t framesEncoded() const { return m_framesEncoded.load(); }
    size_t queueBytes() const { return m_queue ? m_queue->bytes() : 0; }
    std::string extension() const { return m_plan.out.container == Container::AVI ? ".avi" : ".mkv"; }
    // Every segment written, complete after close().
    std::vector<SegmentRecord> segments() const;

private:
    struct Item {
        enum Type { Video, Audio, End } type = Video;
        VideoFrame frame;
        int64_t slot = 0;
        std::vector<int32_t> audio;
        size_t frames = 0;
    };
    void run();
    bool openSegment(int64_t startSlot, uint64_t startSample);
    void closeSegment(bool final);
    std::string segmentPath(int index, bool overflow) const;
    bool needRoll(int64_t slot);
    void encodeVideo(const Item& it);
    void encodeAudio(const int32_t* s, size_t frames);
    void setError(const std::string& e);

    OutputPlan m_plan;
    EventLog* m_log;
    Muxer m_muxer;
    std::unique_ptr<BlockingQueue<Item>> m_queue;
    std::thread m_thread;
    bool m_open = false;

    std::unique_ptr<VideoEncoder> m_venc;
    std::unique_ptr<AudioEncoder> m_aenc;
    int64_t m_segStartSlot = 0;
    uint64_t m_segStartSample = 0;
    uint64_t m_audioCount = 0;      // global sample frames received
    bool m_segmenting = false;
    std::atomic<int> m_segIndex{0};
    std::atomic<bool> m_onOverflow{false};
    int64_t m_lastSpaceCheckSlot = -1000;

    bool m_pendingRoll = false;
    int64_t m_rollSlot = 0;
    uint64_t m_rollSample = 0;
    std::vector<Item> m_heldVideo;
    std::vector<int32_t> m_heldAudio;

    FrameMd5Writer m_md5;
    int64_t m_segFrames = 0;
    std::string m_segPath;
    mutable std::mutex m_segMutex;
    std::vector<SegmentRecord> m_segs;

    std::vector<AVPacket*> m_pkts;
    std::string m_vdesc, m_adesc;
    std::atomic<uint64_t> m_framesEncoded{0};
    mutable std::mutex m_errMutex;
    std::string m_error;
    std::atomic<bool> m_failed{false};
};

}  // namespace pd
