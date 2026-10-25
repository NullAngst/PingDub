// SPDX-License-Identifier: GPL-3.0-or-later
// libavformat on its own writer thread. Writes go through a custom AVIO so
// PingDub sees every byte: disk-full is caught at once, data is synced to
// disk once a second, and the byte count feeds the stats and stop conditions.
#pragma once

#include "CaptureConfig.h"
#include "RingBuffer.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace pd {

using ParPtr = std::shared_ptr<AVCodecParameters>;
ParPtr copyParams(const AVCodecParameters* src);

struct MuxStream {
    ParPtr par;
    AVRational encoderTb{1, 1};
    std::string title;
};

// A chapter in the first stream's encoder time base (frames).
struct MuxChapter {
    int64_t start = 0, end = 0;
    std::string title;
};

struct SegmentSpec {
    std::string path;
    Container container = Container::MKV;
    std::vector<MuxStream> streams;
    std::map<std::string, std::string> tags;
};

// Placeholder written into a tag so its value can be filled in after the
// capture ends. The muxer writes tags in the header, before the totals exist.
std::string tagPlaceholder(const std::string& key);
constexpr size_t kTagPlaceholderWidth = 72;

// Rewrites placeholders in a finished Matroska file in place and fixes the
// CRC-32 of the Tags element that holds them.
bool patchMatroskaTags(const std::string& path, const std::map<std::string, std::string>& values,
                       std::string& err);

class Muxer {
public:
    Muxer();
    ~Muxer();

    void start(size_t queueBytes = 256u << 20);
    // The calls below queue work for the writer thread, in order.
    void beginSegment(const SegmentSpec& spec);
    void writePacket(AVPacket* pkt, int stream);  // takes ownership
    // Matroska only: chapters go in with the trailer. FFmpeg's muxer writes
    // chapters added after the header there, so the list can be final.
    void endSegment(std::vector<MuxChapter> chapters = {});
    // Drains the queue, closes any open segment and joins the thread.
    void stop();

    bool failed() const { return m_failed.load(); }
    std::string error() const;
    uint64_t bytesWritten() const { return m_total.load(); }
    uint64_t segmentBytes() const { return m_segBytes.load(); }
    std::vector<std::string> files() const;
    std::string currentFile() const;

    // AVIO callbacks
    int ioWrite(const uint8_t* buf, int size);
    int64_t ioSeek(int64_t offset, int whence);

private:
    enum class CmdType { Begin, Packet, End };
    struct Cmd {
        CmdType type = CmdType::Packet;
        AVPacket* pkt = nullptr;
        int stream = 0;
        std::shared_ptr<SegmentSpec> spec;
        std::vector<MuxChapter> chapters;
    };
    void run();
    void doBegin(const SegmentSpec& spec);
    void doPacket(AVPacket* pkt, int stream);
    void doEnd(const std::vector<MuxChapter>& chapters = {});
    void fail(const std::string& msg);

    std::unique_ptr<BlockingQueue<Cmd>> m_queue;
    std::thread m_thread;
    bool m_running = false;

    AVFormatContext* m_fmt = nullptr;
    AVIOContext* m_avio = nullptr;
    int m_fd = -1;
    std::vector<AVRational> m_encTb;
    std::shared_ptr<SegmentSpec> m_cur;
    int64_t m_lastSyncNs = 0;
    bool m_headerWritten = false;

    std::atomic<bool> m_failed{false};
    std::atomic<uint64_t> m_total{0};
    std::atomic<uint64_t> m_segBytes{0};
    mutable std::mutex m_mutex;
    std::string m_error;
    std::vector<std::string> m_files;
    std::string m_currentFile;
};

}  // namespace pd
