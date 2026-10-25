// SPDX-License-Identifier: GPL-3.0-or-later
#include "Muxer.h"

#include "Clock.h"
#include "Encoder.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavutil/crc.h>
#include <libavutil/opt.h>
}

namespace pd {

ParPtr copyParams(const AVCodecParameters* src) {
    AVCodecParameters* p = avcodec_parameters_alloc();
    if (src) avcodec_parameters_copy(p, src);
    return ParPtr(p, [](AVCodecParameters* q) { avcodec_parameters_free(&q); });
}

std::string tagPlaceholder(const std::string& key) {
    std::string s = "@PINGDUB:" + key + "@";
    if (s.size() < kTagPlaceholderWidth) s.append(kTagPlaceholderWidth - s.size(), '.');
    return s;
}

// --- EBML helpers for the in-place tag patch --------------------------------

static bool readVint(const uint8_t* p, size_t avail, uint64_t& value, int& len, bool keepMarker) {
    if (!avail) return false;
    const uint8_t first = p[0];
    len = 1;
    uint8_t mask = 0x80;
    while (len <= 8 && !(first & mask)) {
        mask >>= 1;
        ++len;
    }
    if (len > 8 || size_t(len) > avail) return false;
    value = keepMarker ? first : (first & (mask - 1));
    for (int i = 1; i < len; ++i) value = (value << 8) | p[i];
    return true;
}

bool patchMatroskaTags(const std::string& path, const std::map<std::string, std::string>& values,
                       std::string& err) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        err = std::string("open: ") + std::strerror(errno);
        return false;
    }
    // Tags sit in the header, well inside the first few megabytes.
    const size_t headLen = 8u << 20;
    std::vector<uint8_t> head(headLen);
    const ssize_t got = ::pread(fd, head.data(), headLen, 0);
    if (got <= 0) {
        ::close(fd);
        err = "read failed";
        return false;
    }
    head.resize(size_t(got));

    std::vector<std::pair<size_t, size_t>> touchedTags;  // [start,end) of Tags elements
    int patched = 0;
    for (const auto& [key, value] : values) {
        const std::string ph = tagPlaceholder(key);
        auto it = std::search(head.begin(), head.end(), ph.begin(), ph.end());
        if (it == head.end()) continue;
        const size_t pos = size_t(it - head.begin());
        // Same length on disk: the value, then NUL padding (allowed for EBML strings).
        std::string v = value.substr(0, kTagPlaceholderWidth);
        std::memcpy(head.data() + pos, v.data(), v.size());
        std::memset(head.data() + pos + v.size(), 0, kTagPlaceholderWidth - v.size());
        ++patched;
        // Find the enclosing Tags element (ID 0x1254C367).
        for (size_t s = pos; s >= 4; --s) {
            const size_t id = s - 4;
            if (head[id] == 0x12 && head[id + 1] == 0x54 && head[id + 2] == 0xC3 && head[id + 3] == 0x67) {
                uint64_t size = 0;
                int vlen = 0;
                if (!readVint(head.data() + id + 4, head.size() - id - 4, size, vlen, false)) break;
                const size_t dataStart = id + 4 + size_t(vlen);
                const size_t dataEnd = dataStart + size_t(size);
                if (pos >= dataStart && pos < dataEnd && dataEnd <= head.size()) {
                    touchedTags.push_back({dataStart, dataEnd});
                    break;
                }
            }
            if (pos - s > (4u << 20)) break;
        }
    }
    // Recompute CRC-32 children (ID 0xBF, size 4) that open each Tags element.
    for (auto [start, end] : touchedTags) {
        if (end - start > 6 && head[start] == 0xBF && head[start + 1] == 0x84) {
            const AVCRC* table = av_crc_get_table(AV_CRC_32_IEEE_LE);
            const uint32_t crc = av_crc(table, UINT32_MAX, head.data() + start + 6, end - start - 6) ^ UINT32_MAX;
            head[start + 2] = uint8_t(crc);
            head[start + 3] = uint8_t(crc >> 8);
            head[start + 4] = uint8_t(crc >> 16);
            head[start + 5] = uint8_t(crc >> 24);
        }
    }
    bool ok = true;
    if (patched) {
        ok = ::pwrite(fd, head.data(), head.size(), 0) == ssize_t(head.size());
        if (ok) ::fdatasync(fd);
    }
    ::close(fd);
    if (!ok) err = "write failed";
    else if (!patched) err = "no tag placeholders found";
    return ok && patched;
}

// --- AVIO callbacks -----------------------------------------------------------

#if LIBAVFORMAT_VERSION_MAJOR >= 61
static int writeCb(void* opaque, const uint8_t* buf, int size) {
    return static_cast<Muxer*>(opaque)->ioWrite(buf, size);
}
#else
static int writeCb(void* opaque, uint8_t* buf, int size) { return static_cast<Muxer*>(opaque)->ioWrite(buf, size); }
#endif
static int64_t seekCb(void* opaque, int64_t offset, int whence) {
    return static_cast<Muxer*>(opaque)->ioSeek(offset, whence);
}

int Muxer::ioWrite(const uint8_t* buf, int size) {
    if (m_fd < 0) return AVERROR(EBADF);
    int left = size;
    while (left > 0) {
        const ssize_t n = ::write(m_fd, buf, size_t(left));
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            fail(e == ENOSPC ? "disk full" : std::string("write failed: ") + std::strerror(e));
            return AVERROR(e);
        }
        buf += n;
        left -= int(n);
        m_total += uint64_t(n);
        m_segBytes += uint64_t(n);
    }
    const int64_t now = monotonicNs();
    if (now - m_lastSyncNs > 1000000000LL) {
        // Pushes the last second to the platters, so a power cut costs at most that.
        ::fdatasync(m_fd);
        m_lastSyncNs = now;
    }
    return size;
}

int64_t Muxer::ioSeek(int64_t offset, int whence) {
    if (m_fd < 0) return -1;
    if (whence == AVSEEK_SIZE) {
        struct stat st;
        if (fstat(m_fd, &st) < 0) return -1;
        return st.st_size;
    }
    const off_t r = ::lseek(m_fd, off_t(offset), whence & ~AVSEEK_FORCE);
    return r < 0 ? AVERROR(errno) : int64_t(r);
}

// --- thread -----------------------------------------------------------------

Muxer::Muxer() = default;

Muxer::~Muxer() { stop(); }

void Muxer::start(size_t queueBytes) {
    if (m_running) return;
    m_queue = std::make_unique<BlockingQueue<Cmd>>(queueBytes);
    m_running = true;
    m_thread = std::thread([this] { run(); });
}

void Muxer::beginSegment(const SegmentSpec& spec) {
    Cmd c;
    c.type = CmdType::Begin;
    c.spec = std::make_shared<SegmentSpec>(spec);
    m_queue->push(std::move(c), 1);
}

void Muxer::writePacket(AVPacket* pkt, int stream) {
    Cmd c;
    c.type = CmdType::Packet;
    c.pkt = pkt;
    c.stream = stream;
    const size_t bytes = size_t(pkt->size) + 64;
    if (!m_queue->push(std::move(c), bytes)) av_packet_free(&pkt);
}

void Muxer::endSegment(std::vector<MuxChapter> chapters) {
    Cmd c;
    c.type = CmdType::End;
    c.chapters = std::move(chapters);
    m_queue->push(std::move(c), 1);
}

void Muxer::stop() {
    if (!m_running) return;
    m_queue->close();
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

std::string Muxer::error() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_error;
}

std::vector<std::string> Muxer::files() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_files;
}

std::string Muxer::currentFile() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_currentFile;
}

void Muxer::fail(const std::string& msg) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_error.empty()) m_error = msg;
    m_failed = true;
}

void Muxer::run() {
    Cmd c;
    while (m_queue->pop(c)) {
        switch (c.type) {
        case CmdType::Begin: doBegin(*c.spec); break;
        case CmdType::Packet: doPacket(c.pkt, c.stream); break;
        case CmdType::End: doEnd(c.chapters); break;
        }
        c = Cmd{};
    }
    if (m_fmt) doEnd();
}

void Muxer::doBegin(const SegmentSpec& spec) {
    if (m_fmt) doEnd();
    m_segBytes = 0;
    m_fd = ::open(spec.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (m_fd < 0) {
        fail("cannot create " + spec.path + ": " + std::strerror(errno));
        return;
    }
    const char* fmtName = spec.container == Container::AVI ? "avi" : "matroska";
    if (avformat_alloc_output_context2(&m_fmt, nullptr, fmtName, spec.path.c_str()) < 0 || !m_fmt) {
        fail("cannot create muxer");
        ::close(m_fd);
        m_fd = -1;
        return;
    }
    const int bufSize = 1 << 20;
    uint8_t* buf = static_cast<uint8_t*>(av_malloc(bufSize));
    m_avio = avio_alloc_context(buf, bufSize, 1, this, nullptr, writeCb, seekCb);
    m_avio->seekable = AVIO_SEEKABLE_NORMAL;
    m_fmt->pb = m_avio;
    m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    m_encTb.clear();
    for (const MuxStream& ms : spec.streams) {
        AVStream* st = avformat_new_stream(m_fmt, nullptr);
        // Keep the encoder's fourcc: UT Video and raw YUV need it to be read back right.
        avcodec_parameters_copy(st->codecpar, ms.par.get());
        st->time_base = ms.encoderTb;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            st->sample_aspect_ratio = st->codecpar->sample_aspect_ratio;
            st->avg_frame_rate = av_inv_q(ms.encoderTb);
            st->r_frame_rate = st->avg_frame_rate;
        }
        if (!ms.title.empty()) av_dict_set(&st->metadata, "title", ms.title.c_str(), 0);
        m_encTb.push_back(ms.encoderTb);
    }
    for (const auto& [k, v] : spec.tags) av_dict_set(&m_fmt->metadata, k.c_str(), v.c_str(), 0);

    AVDictionary* opts = nullptr;
    if (spec.container == Container::MKV) {
        // One cluster a second: a crash or power cut loses at most that.
        av_dict_set(&opts, "cluster_time_limit", "1000", 0);
        av_dict_set(&opts, "cluster_size_limit", "268435456", 0);
        // Uncompressed YUV goes in as a VfW-style track; FFmpeg asks for consent.
        av_dict_set(&opts, "allow_raw_vfw", "1", 0);
    }
    m_lastSyncNs = monotonicNs();
    const int r = avformat_write_header(m_fmt, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        fail("cannot write header: " + avErr(r));
        m_headerWritten = false;
    } else {
        m_headerWritten = true;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_currentFile = spec.path;
        m_files.push_back(spec.path);
    }
    m_cur = std::make_shared<SegmentSpec>(spec);
}

void Muxer::doPacket(AVPacket* pkt, int stream) {
    if (!m_fmt || !m_headerWritten || m_failed || stream >= int(m_encTb.size())) {
        av_packet_free(&pkt);
        return;
    }
    AVStream* st = m_fmt->streams[stream];
    pkt->stream_index = stream;
    av_packet_rescale_ts(pkt, m_encTb[size_t(stream)], st->time_base);
    const int r = av_interleaved_write_frame(m_fmt, pkt);
    if (r < 0 && !m_failed) fail("write failed: " + avErr(r));
    av_packet_free(&pkt);
}

void Muxer::doEnd(const std::vector<MuxChapter>& chapters) {
    if (!m_fmt) return;
    if (m_headerWritten && !chapters.empty() && m_cur && m_cur->container == Container::MKV && !m_encTb.empty()) {
        for (size_t i = 0; i < chapters.size(); ++i) {
            AVChapter* ch = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
            if (!ch) break;
            ch->id = int64_t(i + 1);
            ch->time_base = m_encTb[0];
            ch->start = chapters[i].start;
            ch->end = chapters[i].end;
            av_dict_set(&ch->metadata, "title", chapters[i].title.c_str(), 0);
            AVChapter** grown = static_cast<AVChapter**>(av_realloc_array(m_fmt->chapters, m_fmt->nb_chapters + 1, sizeof(AVChapter*)));
            if (!grown) {
                av_dict_free(&ch->metadata);
                av_free(ch);
                break;
            }
            m_fmt->chapters = grown;
            m_fmt->chapters[m_fmt->nb_chapters++] = ch;
        }
    }
    if (m_headerWritten) {
        const int r = av_write_trailer(m_fmt);
        if (r < 0 && !m_failed) fail("cannot finalize file: " + avErr(r));
    }
    if (m_avio) {
        avio_flush(m_avio);
        av_freep(&m_avio->buffer);
        avio_context_free(&m_avio);
    }
    avformat_free_context(m_fmt);
    m_fmt = nullptr;
    if (m_fd >= 0) {
        ::fdatasync(m_fd);
        ::close(m_fd);
        m_fd = -1;
    }
    m_headerWritten = false;
    m_cur.reset();
}

}  // namespace pd
