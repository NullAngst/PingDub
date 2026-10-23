// SPDX-License-Identifier: GPL-3.0-or-later
// Interface every video source implements: V4L2 devices, the Somagic
// userspace driver, DeckLink cards, and the fake backend used by tests.
#pragma once

#include "engine/BufferPool.h"
#include "engine/Types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pd {

class AudioBackend;

struct VideoInputInfo {
    int index = 0;
    std::string name;
};

struct VideoStandardInfo {
    uint64_t id = 0;
    std::string name;
    Rational fps{30000, 1001};
    bool lines525 = true;
};

struct VideoFormatOption {
    PixFmt pixfmt = PixFmt::Unknown;
    uint32_t fourcc = 0;
    std::string description;
    bool archival = true;
};

struct VideoControl {
    enum class Type { Integer, Boolean, Menu, IntegerMenu, Button, Integer64, Other };
    uint32_t id = 0;
    std::string name;
    Type type = Type::Integer;
    int64_t minimum = 0, maximum = 0, step = 1, defaultValue = 0, value = 0;
    std::vector<std::pair<int64_t, std::string>> menu;
    bool readOnly = false;
    bool inactive = false;
    bool privateControl = false;  // driver-specific (comb filter, chroma AGC, ...)
};

struct SignalStatus {
    bool known = false;
    bool noPower = false;
    bool noSignal = false;
    bool noColor = false;
    bool noHLock = false;
    std::string detected;  // detected standard name, if the driver can tell
};

class VideoBackend {
public:
    virtual ~VideoBackend() = default;

    virtual std::string backendName() const = 0;  // "V4L2", "Somagic", "DeckLink", "Test"
    virtual std::string deviceName() const = 0;
    virtual std::string driverName() const = 0;
    virtual std::string devicePath() const = 0;
    virtual bool experimental() const { return false; }

    virtual std::vector<VideoInputInfo> inputs() = 0;
    virtual int currentInput() = 0;
    virtual bool setInput(int index, std::string& err) = 0;

    virtual std::vector<VideoStandardInfo> standards() = 0;
    virtual uint64_t currentStandard() = 0;
    virtual bool setStandard(uint64_t id, std::string& err) = 0;

    virtual std::vector<VideoFormatOption> formats() = 0;
    // Asks the driver for the full frame at the given pixel format and reports
    // what it actually delivers.
    virtual bool configure(PixFmt pixfmt, VideoFormat& out, std::string& err) = 0;

    virtual std::vector<VideoControl> controls() { return {}; }
    virtual bool setControl(uint32_t id, int64_t value, std::string& err) {
        err = "not supported";
        return false;
    }

    virtual SignalStatus signalStatus() { return {}; }

    virtual bool start(int bufferCount, std::string& err) = 0;
    virtual void stop() = 0;
    // 1: frame delivered, 0: timeout, -1: error or device gone (err set).
    virtual int dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) = 0;

    // Audio carried in the same stream (Somagic, DeckLink), or null.
    virtual AudioBackend* embeddedAudio() { return nullptr; }

    virtual std::string diagnostics() { return {}; }
    virtual std::string cropSummary() const { return {}; }
    virtual std::string vbiNode() const { return {}; }
    // USB link speed in Mbit/s, 0 if not USB or unknown.
    virtual int usbSpeedMbps() const { return 0; }
    // Bus IDs of the hardware, for the known-device quirk table. False if unknown.
    virtual bool hardwareId(bool& usb, uint16_t& vendor, uint16_t& product) const { return false; }
    // Stable key for profiles: VID:PID plus serial, or USB port path, or PCI slot.
    virtual std::string profileKey() const { return devicePath(); }
};

}  // namespace pd
