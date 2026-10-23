// SPDX-License-Identifier: GPL-3.0-or-later
// VBI capture and closed captions. When the driver exposes a VBI node under
// the same parent as the video node, PingDub captures it during recording:
// raw lines when offered (keeps everything in the blanking interval), sliced
// line 21 otherwise. Every buffer is mapped to a timeline frame through its
// timestamp, written to <name>.vbi, and decoded by zvbi into SCC and SRT.
#pragma once

#include "Cea608.h"
#include "Types.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct vbi_raw_decoder;

namespace pd {

struct VbiParams {
    bool sliced = false;
    int samplingRate = 27000000;
    int offset = 0;
    int samplesPerLine = 1440;
    int start[2] = {10, 273};
    int count[2] = {12, 12};
    bool interlaced = false;  // V4L2_VBI_INTERLACED: lines of both fields alternate
    bool lines525 = true;
};

// Device-independent half: takes buffers with their timeline slot.
class VbiProcessor {
public:
    VbiProcessor();
    ~VbiProcessor();
    bool begin(const VbiParams& p, const std::string& basePath, const Rational& fps);
    // Raw: count[0]+count[1] lines of samplesPerLine bytes. Sliced: an array
    // of struct v4l2_sliced_vbi_data.
    void process(const uint8_t* data, size_t size, int64_t slot);
    // Writes .scc and .srt sidecars for channels that carried data.
    std::vector<std::string> finish(int64_t endSlot);
    std::string liveText(int channel = 1) const { return m_cc.liveText(channel); }
    XdsInfo xds() const { return m_cc.xds(); }
    uint64_t buffers() const { return m_buffers; }
    uint64_t captionBytes() const { return m_ccPairs; }

private:
    VbiParams m_p;
    std::string m_base;
    Rational m_fps;
    FILE* m_raw = nullptr;
    std::unique_ptr<vbi_raw_decoder> m_rd;
    SccWriter m_scc;
    CaptionTracker m_cc;
    uint64_t m_buffers = 0;
    uint64_t m_ccPairs = 0;
    int64_t m_lastSccSlot = -1;
};

class VbiCapture {
public:
    ~VbiCapture();
    // Opens and configures the node; fails if it offers no caption service.
    bool open(const std::string& devPath, bool lines525, std::string& err);
    bool isOpen() const { return m_fd >= 0; }
    std::string description() const { return m_desc; }
    bool start(std::function<int64_t(int64_t)> slotForTs, const std::string& basePath, const Rational& fps);
    std::vector<std::string> stop(int64_t endSlot);
    std::string liveText() const { return m_proc ? m_proc->liveText(1) : std::string(); }
    XdsInfo xds() const { return m_proc ? m_proc->xds() : XdsInfo{}; }

private:
    void run();
    void close();
    int m_fd = -1;
    std::string m_desc;
    VbiParams m_params;
    bool m_streaming = false;
    struct Map {
        void* ptr;
        size_t len;
    };
    std::vector<Map> m_maps;
    size_t m_bufSize = 0;
    std::function<int64_t(int64_t)> m_slotFor;
    std::unique_ptr<VbiProcessor> m_proc;
    std::thread m_thread;
    std::atomic<bool> m_run{false};
    struct Pending {
        std::vector<uint8_t> data;
        int64_t ts;
    };
    std::deque<Pending> m_pending;
};

// Finds a /dev/vbiN sharing the parent device of /dev/videoN, or "".
std::string findVbiNodeFor(const std::string& videoNode);

}  // namespace pd
