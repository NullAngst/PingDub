// SPDX-License-Identifier: GPL-3.0-or-later
#include "VbiCapture.h"

#include "Clock.h"
#include "EventLog.h"

#include <libzvbi.h>
#include <linux/videodev2.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace pd {

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

// --- processor ----------------------------------------------------------------

VbiProcessor::VbiProcessor() = default;

VbiProcessor::~VbiProcessor() {
    if (m_rd) vbi_raw_decoder_destroy(m_rd.get());
    if (m_raw) std::fclose(m_raw);
}

static void put32(FILE* f, uint32_t v) {
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
static void put64(FILE* f, uint64_t v) {
    put32(f, uint32_t(v));
    put32(f, uint32_t(v >> 32));
}

bool VbiProcessor::begin(const VbiParams& p, const std::string& basePath, const Rational& fps) {
    m_p = p;
    m_base = basePath;
    m_fps = fps;
    m_scc.setRate(fps);
    if (!p.sliced) {
        m_rd = std::make_unique<vbi_raw_decoder>();
        vbi_raw_decoder_init(m_rd.get());
        m_rd->scanning = p.lines525 ? 525 : 625;
        m_rd->sampling_format = VBI_PIXFMT_YUV420;  // 8-bit luma samples
        m_rd->sampling_rate = p.samplingRate;
        m_rd->bytes_per_line = p.samplesPerLine;
        m_rd->offset = p.offset;
        m_rd->start[0] = p.start[0];
        m_rd->start[1] = p.start[1];
        m_rd->count[0] = p.count[0];
        m_rd->count[1] = p.count[1];
        m_rd->interlaced = p.interlaced ? TRUE : FALSE;
        m_rd->synchronous = TRUE;
        vbi_raw_decoder_add_services(m_rd.get(), VBI_SLICED_CAPTION_525, 0);
    }
    if (!basePath.empty()) {
        m_raw = std::fopen((basePath + ".vbi").c_str(), "wb");
        if (m_raw) {
            JsonWriter j;
            j.begin()
                .str("format", p.sliced ? "v4l2_sliced_vbi_data" : "raw")
                .integer("sampling_rate", p.samplingRate)
                .integer("offset", p.offset)
                .integer("samples_per_line", p.samplesPerLine)
                .integer("start0", p.start[0])
                .integer("start1", p.start[1])
                .integer("count0", p.count[0])
                .integer("count1", p.count[1])
                .integer("scanning", p.lines525 ? 525 : 625)
                .str("fps", std::to_string(fps.num) + "/" + std::to_string(fps.den))
                .str("record", "u64le timeline_frame, u32le size, bytes")
                .end();
            const std::string hdr = j.text();
            std::fwrite("PINGDUBVBI\x01\x00", 1, 12, m_raw);
            put32(m_raw, uint32_t(hdr.size()));
            std::fwrite(hdr.data(), 1, hdr.size(), m_raw);
        }
    }
    return true;
}

void VbiProcessor::process(const uint8_t* data, size_t size, int64_t slot) {
    ++m_buffers;
    if (m_raw) {
        put64(m_raw, uint64_t(slot));
        put32(m_raw, uint32_t(size));
        std::fwrite(data, 1, size, m_raw);
    }
    std::vector<std::pair<int, std::pair<uint8_t, uint8_t>>> lines;
    if (!m_p.sliced && m_rd) {
        const size_t need = size_t(m_p.count[0] + m_p.count[1]) * size_t(m_p.samplesPerLine);
        if (size < need) return;
        vbi_sliced out[64];
        const int n = vbi_raw_decode(m_rd.get(), const_cast<uint8_t*>(data), out);
        for (int i = 0; i < n; ++i) {
            if (out[i].id & VBI_SLICED_CAPTION_525_F1) lines.push_back({1, {out[i].data[0], out[i].data[1]}});
            else if (out[i].id & VBI_SLICED_CAPTION_525_F2) lines.push_back({2, {out[i].data[0], out[i].data[1]}});
        }
    } else {
        const size_t n = size / sizeof(v4l2_sliced_vbi_data);
        const v4l2_sliced_vbi_data* d = reinterpret_cast<const v4l2_sliced_vbi_data*>(data);
        for (size_t i = 0; i < n; ++i) {
            if (!(d[i].id & V4L2_SLICED_CAPTION_525)) continue;
            lines.push_back({d[i].field ? 2 : 1, {d[i].data[0], d[i].data[1]}});
        }
    }
    const double t = double(slot) * m_fps.den / m_fps.num;
    for (const auto& [field, bytes] : lines) {
        if (field == 1 && slot != m_lastSccSlot) {
            m_scc.add(slot, bytes.first, bytes.second);
            m_lastSccSlot = slot;
            if ((bytes.first & 0x7f) || (bytes.second & 0x7f)) ++m_ccPairs;
        }
    }
    m_cc.feed(t, lines);
}

std::vector<std::string> VbiProcessor::finish(int64_t endSlot) {
    std::vector<std::string> files;
    if (m_raw) {
        std::fclose(m_raw);
        m_raw = nullptr;
        files.push_back(m_base + ".vbi");
    }
    m_cc.finish(double(endSlot) * m_fps.den / m_fps.num);
    if (m_base.empty()) return files;
    if (m_ccPairs) {
        const std::string scc = m_base + ".cc1.scc";
        if (writeTextFile(scc, m_scc.text())) files.push_back(scc);
    }
    for (const auto& [ch, cues] : m_cc.cues()) {
        if (cues.empty()) continue;
        const std::string path = m_base + ".cc" + std::to_string(ch) + ".srt";
        if (writeTextFile(path, CaptionTracker::srt(cues))) files.push_back(path);
    }
    return files;
}

// --- device -------------------------------------------------------------------

std::string findVbiNodeFor(const std::string& videoNode) {
    const std::string name = videoNode.substr(videoNode.find_last_of('/') + 1);
    char real[PATH_MAX];
    const std::string link = "/sys/class/video4linux/" + name + "/device";
    if (!realpath(link.c_str(), real)) return {};
    const std::string parent = real;
    DIR* d = opendir("/sys/class/video4linux");
    if (!d) return {};
    std::string found;
    while (dirent* e = readdir(d)) {
        if (std::strncmp(e->d_name, "vbi", 3) != 0) continue;
        const std::string l = std::string("/sys/class/video4linux/") + e->d_name + "/device";
        char r2[PATH_MAX];
        if (realpath(l.c_str(), r2) && parent == r2) {
            found = std::string("/dev/") + e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

VbiCapture::~VbiCapture() {
    stop(0);
    close();
}

void VbiCapture::close() {
    for (auto& m : m_maps) munmap(m.ptr, m.len);
    m_maps.clear();
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

bool VbiCapture::open(const std::string& devPath, bool lines525, std::string& err) {
    m_fd = ::open(devPath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        err = devPath + ": " + std::strerror(errno);
        return false;
    }
    v4l2_capability cap{};
    if (xioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        err = "VIDIOC_QUERYCAP failed on " + devPath;
        close();
        return false;
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    m_params.lines525 = lines525;
    m_streaming = caps & V4L2_CAP_STREAMING;
    if (caps & V4L2_CAP_VBI_CAPTURE) {
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VBI_CAPTURE;
        if (xioctl(m_fd, VIDIOC_G_FMT, &f) == 0 && f.fmt.vbi.sample_format == V4L2_PIX_FMT_GREY) {
            m_params.sliced = false;
            m_params.samplingRate = int(f.fmt.vbi.sampling_rate);
            m_params.offset = int(f.fmt.vbi.offset);
            m_params.samplesPerLine = int(f.fmt.vbi.samples_per_line);
            m_params.start[0] = f.fmt.vbi.start[0];
            m_params.start[1] = f.fmt.vbi.start[1];
            m_params.count[0] = int(f.fmt.vbi.count[0]);
            m_params.count[1] = int(f.fmt.vbi.count[1]);
            m_params.interlaced = f.fmt.vbi.flags & V4L2_VBI_INTERLACED;
            m_bufSize = size_t(m_params.count[0] + m_params.count[1]) * size_t(m_params.samplesPerLine);
            char d[160];
            std::snprintf(d, sizeof d, "raw VBI, %d Hz, %d samples/line, lines %d+%d and %d+%d",
                          m_params.samplingRate, m_params.samplesPerLine, m_params.start[0], m_params.count[0],
                          m_params.start[1], m_params.count[1]);
            m_desc = d;
            return true;
        }
    }
    if (caps & V4L2_CAP_SLICED_VBI_CAPTURE) {
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_SLICED_VBI_CAPTURE;
        f.fmt.sliced.service_set = V4L2_SLICED_CAPTION_525;
        if (xioctl(m_fd, VIDIOC_S_FMT, &f) == 0 && (f.fmt.sliced.service_set & V4L2_SLICED_CAPTION_525)) {
            m_params.sliced = true;
            m_bufSize = f.fmt.sliced.io_size ? f.fmt.sliced.io_size : sizeof(v4l2_sliced_vbi_data) * 2;
            m_desc = "sliced VBI, line 21 captions";
            return true;
        }
    }
    err = devPath + " offers neither raw VBI nor sliced line 21 captions";
    close();
    return false;
}

bool VbiCapture::start(std::function<int64_t(int64_t)> slotForTs, const std::string& basePath, const Rational& fps) {
    if (m_fd < 0) return false;
    m_slotFor = std::move(slotForTs);
    m_proc = std::make_unique<VbiProcessor>();
    m_proc->begin(m_params, basePath, fps);
    if (m_streaming) {
        v4l2_requestbuffers rb{};
        rb.count = 8;
        rb.type = m_params.sliced ? V4L2_BUF_TYPE_SLICED_VBI_CAPTURE : V4L2_BUF_TYPE_VBI_CAPTURE;
        rb.memory = V4L2_MEMORY_MMAP;
        if (xioctl(m_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
            m_streaming = false;
        } else {
            for (unsigned i = 0; i < rb.count; ++i) {
                v4l2_buffer b{};
                b.type = rb.type;
                b.memory = V4L2_MEMORY_MMAP;
                b.index = i;
                if (xioctl(m_fd, VIDIOC_QUERYBUF, &b) < 0) break;
                void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, b.m.offset);
                if (p == MAP_FAILED) break;
                m_maps.push_back({p, b.length});
                xioctl(m_fd, VIDIOC_QBUF, &b);
            }
            int type = int(rb.type);
            if (m_maps.empty() || xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) m_streaming = false;
        }
    }
    m_run = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void VbiCapture::run() {
    std::vector<uint8_t> buf(std::max<size_t>(m_bufSize, 4096));
    const uint32_t type = m_params.sliced ? V4L2_BUF_TYPE_SLICED_VBI_CAPTURE : V4L2_BUF_TYPE_VBI_CAPTURE;
    while (m_run) {
        pollfd pfd{m_fd, POLLIN, 0};
        const int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            Pending p;
            if (m_streaming) {
                v4l2_buffer b{};
                b.type = type;
                b.memory = V4L2_MEMORY_MMAP;
                if (xioctl(m_fd, VIDIOC_DQBUF, &b) == 0) {
                    const uint8_t* src = static_cast<const uint8_t*>(m_maps[b.index].ptr);
                    p.data.assign(src, src + b.bytesused);
                    p.ts = int64_t(b.timestamp.tv_sec) * 1000000000LL + int64_t(b.timestamp.tv_usec) * 1000;
                    if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC || p.ts == 0)
                        p.ts = monotonicNs();
                    xioctl(m_fd, VIDIOC_QBUF, &b);
                }
            } else {
                const ssize_t n = ::read(m_fd, buf.data(), buf.size());
                if (n > 0) {
                    p.data.assign(buf.begin(), buf.begin() + n);
                    p.ts = monotonicNs();
                }
            }
            if (!p.data.empty()) m_pending.push_back(std::move(p));
        } else if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
            break;  // device gone
        }
        // Map pending buffers to timeline frames once video has caught up.
        while (!m_pending.empty()) {
            Pending& head = m_pending.front();
            const int64_t slot = m_slotFor ? m_slotFor(head.ts) : -1;
            if (slot >= 0) {
                m_proc->process(head.data.data(), head.data.size(), slot);
                m_pending.pop_front();
            } else if (monotonicNs() - head.ts > 2000000000LL) {
                m_pending.pop_front();  // no frame near it (before capture start or after a gap)
            } else {
                break;
            }
        }
    }
}

std::vector<std::string> VbiCapture::stop(int64_t endSlot) {
    std::vector<std::string> files;
    if (!m_run) return files;
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_streaming && m_fd >= 0) {
        int type = m_params.sliced ? V4L2_BUF_TYPE_SLICED_VBI_CAPTURE : V4L2_BUF_TYPE_VBI_CAPTURE;
        xioctl(m_fd, VIDIOC_STREAMOFF, &type);
    }
    if (m_proc) files = m_proc->finish(endSlot);
    return files;
}

}  // namespace pd
