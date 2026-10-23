// SPDX-License-Identifier: GPL-3.0-or-later
// Raw V4L2 capture: MMAP streaming, per-frame sequence numbers, timestamps
// and error flags, no libv4l2 conversion layer. Asks the driver for the full
// crop bounds and the largest unscaled size, never for scaling.
#pragma once

#include "VideoBackend.h"
#include "V4l2Logic.h"

#include <string>
#include <vector>

namespace pd {

class V4l2Backend : public VideoBackend {
public:
    V4l2Backend();
    ~V4l2Backend() override;

    bool open(const std::string& path, std::string& err);
    void close();
    bool isOpen() const { return m_fd >= 0; }

    std::string backendName() const override { return "V4L2"; }
    std::string deviceName() const override { return m_card; }
    std::string driverName() const override { return m_driver; }
    std::string devicePath() const override { return m_path; }

    std::vector<VideoInputInfo> inputs() override;
    int currentInput() override;
    bool setInput(int index, std::string& err) override;

    std::vector<VideoStandardInfo> standards() override;
    uint64_t currentStandard() override;
    bool setStandard(uint64_t id, std::string& err) override;
    std::string detectStandard();

    std::vector<VideoFormatOption> formats() override;
    bool configure(PixFmt pixfmt, VideoFormat& out, std::string& err) override;

    std::vector<VideoControl> controls() override;
    bool setControl(uint32_t id, int64_t value, std::string& err) override;
    SignalStatus signalStatus() override;

    bool start(int bufferCount, std::string& err) override;
    void stop() override;
    int dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) override;

    std::string diagnostics() override;
    std::string cropSummary() const override { return m_cropSummary; }
    std::string vbiNode() const override;
    int usbSpeedMbps() const override;
    std::string profileKey() const override;
    bool hardwareId(bool& usb, uint16_t& vendor, uint16_t& product) const override;

    bool timestampsMonotonic() const { return m_monotonic; }

private:
    int m_fd = -1;
    std::string m_path, m_card, m_driver, m_bus;
    uint32_t m_caps = 0;
    struct Map {
        void* ptr = nullptr;
        size_t len = 0;
    };
    std::vector<Map> m_maps;
    bool m_streaming = false;
    VideoFormat m_fmt;
    FieldDecision m_field;
    uint32_t m_fourcc = 0;
    std::string m_cropSummary;
    std::string m_detectedBeforeStart;
    bool m_monotonic = true;
    bool m_warnedClock = false;
    std::vector<uint8_t> m_weaveTmp;
};

}  // namespace pd
