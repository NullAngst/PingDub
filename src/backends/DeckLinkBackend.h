// SPDX-License-Identifier: GPL-3.0-or-later
// Blackmagic DeckLink / Intensity cards with analog inputs. Blackmagic does
// not use V4L2; the DeckLink API is loaded at runtime from Desktop Video's
// libDeckLinkAPI.so, so the backend simply does not appear without it.
// EXPERIMENTAL: untested on real hardware.
#pragma once

#include "AudioBackend.h"
#include "VideoBackend.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class IDeckLink;
class IDeckLinkInput;
class IDeckLinkConfiguration;

namespace pd {

struct DeckLinkCardInfo {
    int index = 0;
    std::string name;
    bool composite = false;
    bool svideo = false;
};

class DeckLinkBackend;
class DeckLinkCallback;

class DeckLinkAudio : public AudioBackend {
public:
    explicit DeckLinkAudio(DeckLinkBackend* d) : m_dev(d) {}
    std::string backendName() const override { return "DeckLink"; }
    std::string deviceName() const override { return "DeckLink analog audio input"; }
    bool open(int preferRate, AudioFormat& out, std::string& err) override;
    bool start(std::string& err) override { return true; }
    void stop() override {}
    void close() override {}
    int read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) override;

private:
    DeckLinkBackend* m_dev;
};

class DeckLinkBackend : public VideoBackend {
public:
    DeckLinkBackend();
    ~DeckLinkBackend() override;

    static bool available();  // libDeckLinkAPI.so loads
    static std::vector<DeckLinkCardInfo> enumerate();

    bool open(int index, std::string& err);

    std::string backendName() const override { return "DeckLink"; }
    std::string deviceName() const override { return m_name; }
    std::string driverName() const override { return "Blackmagic Desktop Video"; }
    std::string devicePath() const override { return "decklink:" + std::to_string(m_index); }
    bool experimental() const override { return true; }

    std::vector<VideoInputInfo> inputs() override;
    int currentInput() override { return m_input; }
    bool setInput(int index, std::string& err) override;
    std::vector<VideoStandardInfo> standards() override;
    uint64_t currentStandard() override { return m_std; }
    bool setStandard(uint64_t id, std::string& err) override;
    std::vector<VideoFormatOption> formats() override;
    bool configure(PixFmt pixfmt, VideoFormat& out, std::string& err) override;
    SignalStatus signalStatus() override;
    bool start(int bufferCount, std::string& err) override;
    void stop() override;
    int dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) override;
    AudioBackend* embeddedAudio() override { return &m_audio; }
    std::string diagnostics() override;
    std::string profileKey() const override { return "decklink-" + m_name; }

    // from the callback thread
    void onFrame(const uint8_t* data, size_t size, int64_t streamTimeNs, bool noSignal, const int32_t* audio,
                 size_t audioFrames, int64_t audioTimeNs);
    int readAudio(AudioChunk& out, int timeoutMs, std::string& err);

private:
    IDeckLink* m_card = nullptr;
    IDeckLinkInput* m_input_ = nullptr;
    IDeckLinkConfiguration* m_config = nullptr;
    DeckLinkCallback* m_cb = nullptr;
    DeckLinkAudio m_audio{this};
    std::string m_name;
    int m_index = 0;
    int m_input = 0;
    bool m_hasComposite = false, m_hasSvideo = false;
    uint64_t m_std = 0;
    PixFmt m_pixfmt = PixFmt::UYVY;
    VideoFormat m_fmt;
    bool m_running = false;
    int64_t m_clockOffset = 0;  // card stream time -> CLOCK_MONOTONIC, fixed at the first frame
    bool m_haveOffset = false;
    std::atomic<bool> m_noSignal{false};

    std::mutex m_mutex;
    std::condition_variable m_cvV, m_cvA;
    struct QFrame {
        std::vector<uint8_t> data;
        int64_t ts;
        uint32_t seq;
    };
    std::deque<QFrame> m_frames;
    std::deque<AudioChunk> m_audioQ;
    uint32_t m_seq = 0;
};

}  // namespace pd
