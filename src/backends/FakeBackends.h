// SPDX-License-Identifier: GPL-3.0-or-later
// Test-pattern sources that behave like real devices: they pace themselves on
// CLOCK_MONOTONIC, stamp frames and chunks the way drivers do, and can inject
// sequence gaps, damaged frames, a signal loss, xruns and clock drift. Used by
// --self-test, the test suite, and the UI when PINGDUB_FAKE_DEVICES=1.
#pragma once

#include "AudioBackend.h"
#include "VideoBackend.h"

#include <atomic>

namespace pd {

struct FakeVideoOptions {
    bool ntsc = true;
    double dropEverySec = 0;     // skip a sequence number this often (0: never)
    double lossAtSec = -1;       // stop delivering at this time...
    double lossLenSec = 0;       // ...for this long
};

class FakeVideoBackend : public VideoBackend {
public:
    explicit FakeVideoBackend(FakeVideoOptions o = {});
    std::string backendName() const override { return "Test"; }
    std::string deviceName() const override { return "PingDub test pattern"; }
    std::string driverName() const override { return "pingdub-fake"; }
    std::string devicePath() const override { return "fake:video"; }

    std::vector<VideoInputInfo> inputs() override { return {{0, "Color bars"}, {1, "Gray ramp"}}; }
    int currentInput() override { return m_input; }
    bool setInput(int index, std::string& err) override;
    std::vector<VideoStandardInfo> standards() override;
    uint64_t currentStandard() override;
    bool setStandard(uint64_t id, std::string& err) override;
    std::vector<VideoFormatOption> formats() override { return {{PixFmt::YUYV, 0, "YUYV 4:2:2", true}}; }
    bool configure(PixFmt pixfmt, VideoFormat& out, std::string& err) override;
    std::vector<VideoControl> controls() override;
    bool setControl(uint32_t id, int64_t value, std::string& err) override;
    SignalStatus signalStatus() override;
    bool start(int bufferCount, std::string& err) override;
    void stop() override { m_running = false; }
    int dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) override;
    std::string diagnostics() override { return "Test pattern device, no hardware involved.\n"; }

    uint64_t framesDelivered() const { return m_delivered; }
    uint64_t framesSkipped() const { return m_skipped; }

private:
    void render(uint8_t* dst, uint64_t frame);
    FakeVideoOptions m_opt;
    VideoFormat m_fmt;
    int m_input = 0;
    int m_brightness = 0;
    bool m_running = false;
    int64_t m_start = 0;
    uint64_t m_next = 0;
    uint32_t m_seq = 0;
    uint64_t m_delivered = 0, m_skipped = 0;
    std::atomic<bool> m_inLoss{false};
};

struct FakeAudioOptions {
    int rate = 48000;
    int channels = 2;
    SampleFmt fmt = SampleFmt::S16;
    double ppm = 0;              // clock error of the simulated ADC
    double xrunEverySec = 0;     // lose 50 ms of audio this often (0: never)
};

class FakeAudioBackend : public AudioBackend {
public:
    explicit FakeAudioBackend(FakeAudioOptions o = {}) : m_opt(o) {}
    std::string backendName() const override { return "Test"; }
    std::string deviceName() const override { return "PingDub test tone"; }
    bool open(int preferRate, AudioFormat& out, std::string& err) override;
    bool start(std::string& err) override;
    void stop() override { m_running = false; }
    void close() override {}
    int read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) override;

private:
    FakeAudioOptions m_opt;
    bool m_running = false;
    int64_t m_start = 0;
    uint64_t m_pos = 0;
    double m_nextXrun = 0;
};

}  // namespace pd
