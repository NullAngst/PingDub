// SPDX-License-Identifier: GPL-3.0-or-later
// Userspace driver for Somagic SMI2021 EasyCaps (DC60, EasyCAP002) over
// libusb. There is no mainline kernel driver; this ports the somagic-easycap
// tools' init and capture sequence. EXPERIMENTAL: untested on real hardware.
#pragma once

#include "AudioBackend.h"
#include "SomagicParse.h"
#include "VideoBackend.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace pd {

struct SomagicDeviceInfo {
    uint16_t pid = 0;
    bool needsFirmware = false;  // 1c88:0007, before init
    int bus = 0, address = 0;
    std::string port;            // "bus-port.port"
};

class SomagicBackend;

class SomagicAudio : public AudioBackend {
public:
    explicit SomagicAudio(SomagicBackend* dev) : m_dev(dev) {}
    std::string backendName() const override { return "Somagic"; }
    std::string deviceName() const override { return "Somagic EasyCap audio (CS5340)"; }
    bool open(int preferRate, AudioFormat& out, std::string& err) override;
    bool start(std::string& err) override { return true; }
    void stop() override {}
    void close() override {}
    int read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) override;

private:
    SomagicBackend* m_dev;
};

class SomagicBackend : public VideoBackend {
public:
    SomagicBackend();
    ~SomagicBackend() override;

    static std::vector<SomagicDeviceInfo> detect();
    static std::string firmwarePath();  // ~/.local/share/pingdub/firmware/somagic_firmware.bin
    // Extracts firmware from the Windows driver file and stores it.
    static bool installFirmwareFromDriver(const std::string& driverFile, std::string& err);
    // Uploads firmware to an uninitialized device (1c88:0007) and waits for it
    // to come back as 003c/003e/003f.
    static bool initDevice(std::string& err);
    static std::string udevRuleText();

    bool open(const SomagicDeviceInfo& info, std::string& err);

    std::string backendName() const override { return "Somagic"; }
    std::string deviceName() const override;
    std::string driverName() const override { return "pingdub-somagic (userspace)"; }
    std::string devicePath() const override { return "usb:" + m_info.port; }
    bool experimental() const override { return true; }

    std::vector<VideoInputInfo> inputs() override;
    int currentInput() override { return m_input; }
    bool setInput(int index, std::string& err) override;
    std::vector<VideoStandardInfo> standards() override;
    uint64_t currentStandard() override { return m_std; }
    bool setStandard(uint64_t id, std::string& err) override;
    std::vector<VideoFormatOption> formats() override;
    bool configure(PixFmt pixfmt, VideoFormat& out, std::string& err) override;
    std::vector<VideoControl> controls() override;
    bool setControl(uint32_t id, int64_t value, std::string& err) override;
    bool start(int bufferCount, std::string& err) override;
    void stop() override;
    int dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) override;
    AudioBackend* embeddedAudio() override { return &m_audio; }
    std::string diagnostics() override;
    int usbSpeedMbps() const override { return 480; }
    std::string profileKey() const override;

    // used by SomagicAudio
    int readAudio(AudioChunk& out, int timeoutMs, std::string& err);
    void onTransfer(libusb_transfer* t);

private:
    bool writeReg(uint16_t reg, uint8_t val);
    bool writeI2c(uint8_t reg, uint8_t val);
    bool programDecoder();
    void eventLoop();
    void freeTransfers();

    libusb_context* m_ctx = nullptr;
    libusb_device_handle* m_h = nullptr;
    SomagicDeviceInfo m_info;
    int m_input = 0;
    uint64_t m_std = 0;
    bool m_lines525 = true;
    int m_brightness = 128, m_contrast = 71, m_saturation = 64, m_hue = 0;
    int m_lumaMode = 0, m_lumaAperture = 1, m_lumaPrefilter = 0;

    SomagicParser m_parser;
    SomagicAudio m_audio{this};
    std::vector<libusb_transfer*> m_transfers;
    std::vector<std::vector<uint8_t>> m_isoBufs;
    std::thread m_thread;
    std::atomic<bool> m_run{false};
    std::atomic<int> m_pending{0};
    std::atomic<bool> m_gone{false};

    std::mutex m_mutex;
    std::condition_variable m_cvVideo, m_cvAudio;
    struct QFrame {
        std::vector<uint8_t> data;
        int64_t ts;
        uint32_t seq;
    };
    std::deque<QFrame> m_frames;
    std::deque<AudioChunk> m_audioQ;
    uint32_t m_seq = 0;
    std::string m_error;
};

}  // namespace pd
