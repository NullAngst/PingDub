// SPDX-License-Identifier: GPL-3.0-or-later
// The analysis thread. It never holds up capture: frames and audio reach it
// through bounded queues that drop instead of blocking.
//
// While previewing it reads the newest preview frame for the scopes and for
// Level Measure. While capturing it reads the timeline as the sync engine
// lays it out, so everything it finds has an exact timeline slot: crush and
// clip episodes, recording boundaries for chapters, contact sheet
// thumbnails and audio faults.
#pragma once

#include "AudioCheck.h"
#include "Chapters.h"
#include "ContactSheet.h"
#include "EventLog.h"
#include "FrameAnalyzer.h"
#include "LevelWatch.h"
#include "Levels.h"
#include "RingBuffer.h"
#include "Types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pd {

struct AnalysisSettings {
    int chromaLimit = FrameAnalyzer::kDefaultChromaLimit;
    LevelWatchSettings watch;
    AudioCheckSettings audio;
};

struct CaptureAnalysisPlan {
    std::string basePath;          // folder/stem of the capture
    VideoFormat vfmt;
    bool haveAudio = false;
    AudioFormat afmt;
    double contactSheetSec = 30;   // 0: no contact sheet
    EventLog* log = nullptr;
    ChapterBook* chapters = nullptr;
};

struct CaptureAnalysisResult {
    uint64_t framesAnalyzed = 0;
    uint64_t framesNotAnalyzed = 0;   // queue full or format not readable
    uint64_t audioSkipped = 0;        // sample frames the queue had to drop
    uint64_t levelEpisodes[3] = {0, 0, 0};
    uint64_t audioEvents[4] = {0, 0, 0, 0};  // by AudioFault
    std::vector<int64_t> boundaries;
    ContactSheetInfo sheet;
    std::string sheetError;
};

class AnalysisWorker {
public:
    using PreviewSource = std::function<bool(VideoFrame&, uint64_t&)>;

    ~AnalysisWorker();
    void start(const VideoFormat& fmt, const AnalysisSettings& s, PreviewSource preview);
    void stop();
    void setSettings(const AnalysisSettings& s);

    // Capture side. The push calls come from the sync thread and never block.
    void beginCapture(const CaptureAnalysisPlan& p, const AnalysisSettings& s);
    void pushTimeline(const VideoFrame& f, int64_t slot, bool inserted);
    void pushAudio(const int32_t* samples, size_t frames, uint64_t firstSample);
    // The sync engine covered an xrun with `frames` of silence near `atSample`.
    void noteXrun(uint64_t atSample, uint64_t frames);
    // Waits for the queues to drain (the sync thread has stopped), closes open
    // episodes and writes the last contact sheet page.
    CaptureAnalysisResult endCapture();
    // Waits until every timeline slot before `slot` was looked at, up to timeoutMs.
    void waitProcessed(int64_t slot, int timeoutMs);
    bool capturing() const { return m_capturing.load(); }

    // Level Measure, preview only.
    void startMeasure();
    void stopMeasure();
    bool measuring() const { return m_measuring.load(); }
    LevelReading reading();
    // Discards what was measured and skips frames for `settleMs`, after a control changed.
    void restartMeasure(int settleMs = 300);

    // Scopes.
    bool latestScope(ScopeFrame& out, uint64_t& serial);
    LevelWatch::Live liveLevels();
    uint64_t audioEventCount() const { return m_audioCount.load(); }
    uint64_t levelEpisodeCount() const { return m_levelCount.load(); }

private:
    struct TItem {
        VideoFrame frame;
        int64_t slot = 0;
        bool inserted = false;
    };
    struct AItem {
        std::vector<int32_t> samples;
        size_t frames = 0;
        uint64_t first = 0;
        uint64_t xrunFrames = 0;  // marker: an xrun fill around `first`
    };
    void run();
    void processPreview(const VideoFrame& f);
    void processTimeline(const TItem& it);
    void processAudio(const AItem& it);
    void publishScope(const ScopeFrame& s);
    void logEpisodes();
    void logAudio();

    PreviewSource m_preview;
    VideoFormat m_fmt;
    std::thread m_thread;
    std::atomic<bool> m_run{false};
    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_measuring{false};
    std::atomic<int64_t> m_settleUntil{0};

    std::mutex m_procMutex;  // held while processing; guards everything below to the next block
    FrameAnalyzer m_an;
    AnalysisSettings m_set;
    LevelAccumulator m_acc;
    LevelWatch m_watch;
    BoundaryDetector m_bounds;
    ContactSheet m_sheet;
    AudioCheck m_audio;
    CaptureAnalysisPlan m_plan;
    CaptureAnalysisResult m_res;
    uint64_t m_previewSerial = 0;
    int m_scopeEvery = 1;
    uint64_t m_scopeN = 0;

    RingBuffer<TItem> m_tq;
    RingBuffer<AItem> m_aq;
    std::atomic<uint64_t> m_tDropped{0};
    std::atomic<uint64_t> m_aDropped{0};
    std::atomic<int64_t> m_processed{-1};
    std::atomic<bool> m_busy{false};
    std::mutex m_waitMutex;
    std::condition_variable m_waitCv;

    std::mutex m_scopeMutex;
    ScopeFrame m_scope;
    uint64_t m_scopeSerial = 0;
    LevelWatch::Live m_live;
    std::atomic<uint64_t> m_audioCount{0};
    std::atomic<uint64_t> m_levelCount{0};
};

}  // namespace pd
