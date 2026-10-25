// SPDX-License-Identifier: GPL-3.0-or-later
// Puts video frames and audio chunks on one timeline.
//
// Video master (default): every frame delivered by the driver gets a slot,
// gaps are filled with repeats of the last good frame, and audio is resampled
// by a few ppm through libsoxr so its length follows the frame count. The
// resample ratio comes from a slow PI loop (about 30 s) on the measured A/V
// offset, so timestamp jitter never reaches the pitch.
//
// Audio master: audio passes through untouched, frames are repeated or dropped
// when the frame count drifts more than half a frame from the audio length.
//
// No correction: frames and samples as delivered, only the start is aligned.
//
// The engine is single-threaded and deterministic: the caller feeds events in
// timestamp order. CaptureSession does that from its sync thread, tests do it
// from a simulated clock.
#pragma once

#include "Types.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct soxr;

namespace pd {

struct SyncConfig {
    SyncMode mode = SyncMode::VideoMaster;
    GapFill gapFill = GapFill::LastFrame;
    Rational fps{30000, 1001};
    bool haveAudio = true;
    int audioRate = 48000;
    int channels = 2;
    int sampleBits = 16;           // device depth, for requantizing resampled audio
    double clampPpm = 5000.0;      // resample ratio limit, +-0.5%
    double loopSeconds = 30.0;     // PI loop time constant for drift correction
    double hardResyncFrames = 2.0; // offsets beyond this are fixed at once
    uint64_t maxFrames = 0;        // stop the timeline here (timed capture), 0: no limit
};

struct SyncCounters {
    uint64_t realFrames = 0;    // frames delivered by the device and kept
    uint64_t framesOut = 0;     // timeline slots written
    uint64_t inserted = 0;      // repeated or black frames added to hold the timeline
    uint64_t dropped = 0;       // frames removed by audio-master correction
    uint64_t driverDrops = 0;   // sequence number gaps reported by the driver
    uint64_t ringDrops = 0;     // frames lost because the RAM ring was full
    uint64_t errorFrames = 0;   // frames the driver flagged as damaged (kept)
    uint64_t audioIn = 0;       // sample frames received
    uint64_t audioOut = 0;      // sample frames written
    uint64_t xruns = 0;
    uint64_t xrunSilence = 0;   // silence frames inserted for xruns
    uint64_t audioResyncs = 0;
    double avOffsetMs = 0.0;    // + means audio would play late
    double driftPpm = 0.0;      // + means the audio clock runs fast
    double ratio = 1.0;         // current resampler input/output ratio
    double tsJitterMs = 0.0;    // learned spread of video timestamps
    bool clampHit = false;
    int64_t t0Ns = 0;
    bool started = false;
};

struct SyncEvent {
    enum Type { Insert, Drop, DriverDrop, RingOverflow, Xrun, AudioResync, ErrorFrame, Overfill, ClampHit };
    Type type;
    int64_t slot = 0;
    uint64_t count = 0;
    std::string detail;
};
const char* syncEventName(SyncEvent::Type t);

class SyncSink {
public:
    virtual ~SyncSink() = default;
    // frame.data is null for a black fill slot.
    virtual void syncVideo(const VideoFrame& frame, int64_t slot, bool inserted) = 0;
    // Interleaved int32 samples, device bits left-justified.
    virtual void syncAudio(const int32_t* samples, size_t frames) = 0;
    virtual void syncEvent(const SyncEvent& ev) { (void)ev; }
};

// Run by the video thread when the driver stops delivering. Emits synthetic
// slot timestamps that trail real time by a few frames, so a late real frame
// never lands on top of a fill. Elapsed time is measured from when the last
// frame arrived (CLOCK_MONOTONIC), so a device clock that drifts from the
// system clock (DeckLink stream time) cannot trigger false fills.
class StallWatchdog {
public:
    void reset(double intervalNs, int trailFrames = 3) {
        m_interval = intervalNs;
        m_trail = trailFrames;
        m_have = false;
    }
    void onEvent(int64_t tsNs, int64_t arrivalNs) {
        m_last = tsNs;
        m_arrival = arrivalNs;
        m_have = true;
    }
    // Timestamps of fill slots due by `nowNs` (monotonic).
    std::vector<int64_t> poll(int64_t nowNs);

private:
    double m_interval = 33366666.7;
    int m_trail = 3;
    int64_t m_last = 0;
    int64_t m_arrival = 0;
    bool m_have = false;
};

// Run by the audio thread. Compares each chunk's timestamp with where the
// previous chunk ended and says how much silence covers the gap.
class AudioGapDetector {
public:
    void reset(int rate) {
        m_rate = rate;
        m_have = false;
    }
    // Silence frames to insert before this chunk. `xrun` is true when the
    // driver reported an overrun before the read.
    uint32_t check(int64_t chunkTsNs, uint32_t frames, bool xrun);

private:
    int m_rate = 48000;
    int64_t m_expect = 0;
    bool m_have = false;
};

// Decides how many frames are missing before a real frame, from its
// timestamp. Drivers timestamp frames when a USB transfer completes, so on
// some devices timestamps arrive in coarse steps: the GV-USB2 completes one
// 32 ms transfer at a time, which makes the gap between two NTSC frames read
// as 32 ms most of the time and 64 ms about once a second. Comparing each
// gap with 1.5 frames turns every 64 ms step into a false missing frame.
//
// Instead each frame's lateness is measured against the timeline as a whole
// (where it arrived versus the slot it would take), the spread of lateness
// over recent frames is learned as the timestamp jitter, and a frame counts
// as missing only when lateness exceeds what that jitter explains. With
// steady timestamps this is the same as rounding the gap; with coarse ones
// it stops inventing frames.
class FrameClock {
public:
    void reset(double intervalNs) {
        m_interval = intervalNs;
        m_anchored = false;
        m_window.clear();
        m_jitter = 0.0;
        m_seen = 0;
    }
    struct Verdict {
        uint64_t missing = 0;  // frames the timestamps say are missing
        bool early = false;    // frame arrived before its slot (an earlier fill or insert overshot)
    };
    // Judge a real frame at `tsNs` that would take slot `nextSlot` with no inserts.
    Verdict judge(int64_t tsNs, int64_t nextSlot);
    // Record how many slots were inserted before the frame judged last.
    void commit(uint64_t inserted);
    double jitterFrames() const { return m_jitter; }

    static constexpr int kWindow = 90;       // frames of lateness kept (3 s of NTSC)
    static constexpr int kWarmup = 60;       // frames before the jitter estimate is trusted
    static constexpr double kMaxJitter = 0.98;

private:
    void anchor(int64_t tsNs, int64_t slot);
    double m_interval = 33366666.7;
    bool m_anchored = false;
    int64_t m_anchorTs = 0;
    int64_t m_anchorSlot = 0;
    std::deque<double> m_window;  // lateness in frames, after inserts
    double m_jitter = 0.0;        // learned spread, decays slowly
    uint64_t m_seen = 0;
    // pending state between judge() and commit()
    double m_lastLate = 0.0;
    int64_t m_lastTs = 0;
    int64_t m_lastSlot = 0;
    bool m_pending = false;
};

class SyncEngine {
public:
    SyncEngine(const SyncConfig& cfg, SyncSink* sink);
    ~SyncEngine();
    SyncEngine(const SyncEngine&) = delete;
    SyncEngine& operator=(const SyncEngine&) = delete;

    void pushVideo(const VideoFrame& f);
    void pushAudio(const AudioChunk& c);
    // Drains the resampler and pads the audio tail to the video length.
    void finish();

    SyncCounters counters() const;
    bool full() const;  // maxFrames reached
    // Timeline slot of the real frame whose timestamp is closest to tsNs, or -1.
    int64_t slotForTs(int64_t tsNs) const;
    double intervalNs() const { return m_interval; }
    const SyncConfig& config() const { return m_cfg; }

private:
    void startTimeline(const VideoFrame& f);
    void emitVideo(const VideoFrame& f, bool inserted);
    void emitInserted(uint64_t n, const char* why, bool fillRun = false);
    void flushFillRun();
    void placeAudioMaster(const VideoFrame& f, bool isFill);
    void processAudio(const AudioChunk& c);
    void feedResampler(const int32_t* in, size_t frames);
    void writeSilence(uint64_t frames);
    void updateLoop();
    static double calibrateLatency(double clampPpm, double* firstLatency);
    double audioTargetFor(int64_t tsNs) const;  // output sample index for a monotonic time
    void publish();
    uint64_t audioTargetTotal() const;
    void event(SyncEvent::Type t, uint64_t count, const std::string& detail = {});

    SyncConfig m_cfg;
    SyncSink* m_sink;
    double m_interval;  // ns per frame

    bool m_started = false;
    int64_t m_t0 = 0;
    int64_t m_nextSlot = 0;
    int64_t m_lastEventTs = 0;
    FrameClock m_clock;
    uint32_t m_lastSeq = 0;
    uint32_t m_seqStride = 1;
    int m_strideProbe = 0;
    uint64_t m_fillsSinceReal = 0;
    uint64_t m_fillRun = 0;
    int64_t m_fillRunStart = 0;
    VideoFrame m_lastReal;
    std::deque<AudioChunk> m_pendingAudio;

    // audio
    soxr* m_soxr = nullptr;
    bool m_audioStarted = false;
    double m_ioRatio = 1.0;
    double m_latencyCorr = 0.0;
    double m_firstLatency = 0.0;
    double m_eEma = 0.0;
    bool m_eEmaInit = false;
    double m_integ = 0.0;
    uint64_t m_sinceUpdate = 0;
    std::vector<float> m_fin, m_fout;
    std::vector<int32_t> m_iout;
    uint32_t m_dither = 0x12345678u;
    // audio-master mapping: output index and timestamp of the last chunk's first sample
    bool m_haveAudioMap = false;
    double m_aMapPos = 0.0;
    int64_t m_aMapTs = 0;

    SyncCounters m_c;
    mutable std::mutex m_pubMutex;
    SyncCounters m_pub;
    struct SlotTs {
        int64_t ts;
        int64_t slot;
    };
    std::deque<SlotTs> m_slotMap;  // guarded by m_pubMutex
};

}  // namespace pd
