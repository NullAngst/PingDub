// SPDX-License-Identifier: GPL-3.0-or-later
// Owns a capture: the device threads, the RAM rings, the sync thread, the
// output writer, VBI capture, stop conditions and the numbers on screen.
//
// Devices stream from the moment they are opened, so preview, meters and the
// histogram work before recording. Starting a capture only attaches the sync
// engine and the writer; the device keeps its lock and nothing restarts.
#pragma once

#include "BufferPool.h"
#include "CaptureConfig.h"
#include "EventLog.h"
#include "Output.h"
#include "RingBuffer.h"
#include "SyncEngine.h"
#include "VbiCapture.h"
#include "backends/AudioBackend.h"
#include "backends/VideoBackend.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pd {

struct SessionConfig {
    int input = -1;                    // -1: leave as is
    uint64_t standard = 0;             // 0: leave as is
    PixFmt pixfmt = PixFmt::Unknown;   // Unknown: most native the driver offers
    FieldOrder fieldOverride = FieldOrder::Unknown;
    Rational sarOverride{0, 1};
    std::map<uint32_t, int64_t> controls;
    bool audioEnabled = true;
    int audioRate = 48000;
    TimingSettings timing;
    OutputSettings output;
    StopConditions stop;
    bool captureVbi = true;
    bool realtime = true;
    std::string audioDeviceName;       // for tags and logs
};

struct AudioLevels {
    int channels = 0;
    float peakDb[8] = {-120, -120, -120, -120, -120, -120, -120, -120};
    float rmsDb[8] = {-120, -120, -120, -120, -120, -120, -120, -120};
    bool clipped[8] = {};
    uint64_t chunks = 0;
};

enum class SessionState { Closed, Previewing, Capturing, Stopping };

struct SessionStats {
    SessionState state = SessionState::Closed;
    double elapsedSec = 0;       // timeline length
    double wallSec = 0;
    SyncCounters sync;
    uint64_t bytesWritten = 0;
    std::string currentFile;
    int segments = 0;
    uint64_t diskFree = 0;
    double dataRate = 0;         // bytes per second, smoothed
    double timeLeftSec = -1;
    double ringFill = 0;
    double ringFillPeak = 0;
    uint64_t ringBytes = 0;
    SignalStatus signal;
    bool signalLocked = true;
    double lumaStddev = -1;
    std::string ccLive;
    bool vbiActive = false;
    std::string vbiDescription;
    std::string stopReason;
    std::string error;
};

struct CaptureSummary {
    bool ok = false;
    std::string stopReason;
    std::vector<std::string> files;
    std::vector<std::string> sidecars;
    SyncCounters sync;
    double seconds = 0;
    std::string error;
};

struct PreflightItem {
    enum Level { Ok, Warn, Bad } level = Ok;
    std::string text;
};

class CaptureSession {
public:
    CaptureSession();
    ~CaptureSession();

    // Takes ownership. `audio` may be null: then the video backend's embedded
    // audio is used if it has one, else video only.
    bool open(std::unique_ptr<VideoBackend> video, std::unique_ptr<AudioBackend> audio, const SessionConfig& cfg,
              std::string& err);
    void close();
    bool isOpen() const { return m_video != nullptr; }

    VideoBackend* video() { return m_video.get(); }
    AudioBackend* audio() { return m_audioPtr; }
    const VideoFormat& videoFormat() const { return m_vfmt; }
    const AudioFormat& audioFormat() const { return m_afmt; }
    bool hasAudio() const { return m_audioPtr != nullptr; }
    const SessionConfig& config() const { return m_cfg; }

    // Output, timing, stop and VBI settings that apply without reopening the
    // device. Ignored while capturing (the side panel is read-only then).
    bool setRuntimeSettings(const OutputSettings& out, const TimingSettings& timing, const StopConditions& stop,
                            bool captureVbi);
    // Changing a control while previewing or capturing.
    bool setControl(uint32_t id, int64_t value, std::string& err);

    std::vector<PreflightItem> preflight(double plannedSeconds);
    bool startCapture(const std::string& label, std::string& err);
    // Stops, finalizes files and writes sidecars. Safe from any thread.
    CaptureSummary stopCapture(const std::string& reason = "stopped by user");
    bool capturing() const { return m_capturing.load(); }

    SessionStats stats();
    AudioLevels levels();
    void resetClip();  // the clip lights latch until the user clicks them
    // Latest frame for preview; returns false if none newer than `serial`.
    bool latestFrame(VideoFrame& out, uint64_t& serial);
    std::vector<std::string> warnings();

    // Called on the monitor thread when a capture ends without the user
    // (stop condition, device lost, disk full). The summary is final.
    std::function<void(const CaptureSummary&)> onAutoStop;
    // Called when the device disappears or errors while previewing.
    std::function<void(const std::string&)> onDeviceError;

    static std::string sanitizeLabel(const std::string& label);
    static double estimatedBytesPerSecond(const VideoFormat& v, const AudioFormat& a, bool haveAudio,
                                          VideoCodec codec);

private:
    void videoLoop();
    void audioLoop();
    void syncLoop();
    void monitorLoop();
    void warn(const std::string& w);
    std::map<std::string, std::string> finalTags(const CaptureSummary& s);
    void writeJsonSidecar(const CaptureSummary& s);

    SessionConfig m_cfg;
    std::unique_ptr<VideoBackend> m_video;
    std::unique_ptr<AudioBackend> m_audioOwned;
    AudioBackend* m_audioPtr = nullptr;
    VideoFormat m_vfmt;
    AudioFormat m_afmt;
    std::shared_ptr<BufferPool> m_pool;

    std::thread m_vthread, m_athread, m_sthread, m_mthread;
    std::atomic<bool> m_run{false};
    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_stopSync{false};
    std::atomic<bool> m_deviceFailed{false};
    std::string m_deviceError;

    RingBuffer<VideoFrame> m_vring;
    RingBuffer<AudioChunk> m_aring;
    std::atomic<uint32_t> m_pendingRingDrops{0};
    std::atomic<int64_t> m_lastVideoArrival{0};
    std::atomic<int64_t> m_lastRealArrival{0};
    std::atomic<int64_t> m_lastAudioArrival{0};
    StallWatchdog m_watchdog;
    AudioGapDetector m_gap;

    std::unique_ptr<SyncEngine> m_sync;
    std::unique_ptr<OutputWriter> m_out;
    std::unique_ptr<EventLog> m_log;
    std::unique_ptr<VbiCapture> m_vbi;
    std::string m_basePath;  // folder/stem of the first file, for sidecars
    std::string m_label;
    int64_t m_captureStartReal = 0;
    int64_t m_captureStartMono = 0;
    std::mutex m_captureMutex;  // start/stop

    // preview
    std::mutex m_previewMutex;
    VideoFrame m_preview;
    uint64_t m_previewSerial = 0;

    // meters
    std::mutex m_levelMutex;
    AudioLevels m_levels;

    // monitor state
    std::mutex m_statsMutex;
    SignalStatus m_signal;
    std::atomic<double> m_lumaStddev{-1.0};
    int64_t m_staticSince = 0;
    int64_t m_lostSince = 0;
    std::deque<std::pair<int64_t, uint64_t>> m_dropHistory;
    double m_dataRate = 0;
    uint64_t m_lastBytes = 0;
    int64_t m_lastRateNs = 0;
    std::string m_stopReason;
    std::string m_error;

    std::mutex m_warnMutex;
    std::vector<std::string> m_warnings;
    std::atomic<bool> m_rtWarned{false};
};

// Runs a 10-second capture through the fake backends, writes a file into a
// temporary folder, verifies it, prints a report. Returns the process exit code.
int runSelfTest(bool verbose);

}  // namespace pd
