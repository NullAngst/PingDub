// SPDX-License-Identifier: GPL-3.0-or-later
#include "Analysis.h"

#include "Clock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace pd {

AnalysisWorker::~AnalysisWorker() { stop(); }

void AnalysisWorker::start(const VideoFormat& fmt, const AnalysisSettings& s, PreviewSource preview) {
    stop();
    m_fmt = fmt;
    m_preview = std::move(preview);
    {
        std::lock_guard<std::mutex> lk(m_procMutex);
        m_set = s;
        m_an.setFormat(fmt);
        m_an.setChromaLimit(s.chromaLimit);
        m_acc.reset();
    }
    {
        std::lock_guard<std::mutex> lk(m_scopeMutex);
        m_scope = ScopeFrame{};
        m_live = LevelWatch::Live{};
    }
    m_previewSerial = 0;
    m_tq.setCapacity(48);
    m_aq.setCapacity(8u << 20);
    m_tq.reopen();
    m_aq.reopen();
    m_measuring = false;
    m_capturing = false;
    m_run = true;
    m_thread = std::thread([this] { run(); });
}

void AnalysisWorker::stop() {
    m_run = false;
    m_tq.close();
    m_aq.close();
    if (m_thread.joinable()) m_thread.join();
    m_tq.clear();
    m_aq.clear();
    m_capturing = false;
    m_measuring = false;
}

void AnalysisWorker::setSettings(const AnalysisSettings& s) {
    std::lock_guard<std::mutex> lk(m_procMutex);
    if (m_capturing) return;
    m_set = s;
    m_an.setChromaLimit(s.chromaLimit);
}

// ---------------------------------------------------------------------------

void AnalysisWorker::run() {
    while (m_run) {
        if (m_capturing) {
            bool did = false;
            m_busy = true;
            TItem t;
            int n = 0;
            while (n < 8 && m_tq.tryPop(t)) {
                {
                    std::lock_guard<std::mutex> lk(m_procMutex);
                    processTimeline(t);
                }
                t = TItem{};
                ++n;
                did = true;
            }
            AItem a;
            while (m_aq.tryPop(a)) {
                std::lock_guard<std::mutex> lk(m_procMutex);
                processAudio(a);
                did = true;
            }
            m_busy = false;
            m_waitCv.notify_all();
            if (!did) m_tq.waitNonEmpty(std::chrono::milliseconds(10));
            continue;
        }
        VideoFrame f;
        uint64_t serial = m_previewSerial;
        if (m_preview && m_preview(f, serial)) {
            m_previewSerial = serial;
            std::lock_guard<std::mutex> lk(m_procMutex);
            if (!m_capturing) processPreview(f);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }
}

void AnalysisWorker::publishScope(const ScopeFrame& s) {
    std::lock_guard<std::mutex> lk(m_scopeMutex);
    m_scope = s;
    ++m_scopeSerial;
}

void AnalysisWorker::processPreview(const VideoFrame& f) {
    if (!m_an.supported() || !f.data) return;
    FrameStats st;
    ScopeFrame sc;
    if (!m_an.analyze(f.data->data(), f.size, st, &sc)) return;
    publishScope(sc);
    if (!m_measuring) return;
    const double frameSec = m_fmt.fps.valid() ? double(m_fmt.fps.den) / m_fmt.fps.num : 1.0 / 30.0;
    if (monotonicNs() < m_settleUntil.load()) return;
    // Search and fast-forward frames usually have no lock and drop out here.
    if ((f.flags & (VF_NoLock | VF_Error)) || st.blank) m_acc.skip(frameSec);
    else m_acc.add(st, frameSec);
}

void AnalysisWorker::logEpisodes() {
    for (const LevelEpisode& e : m_watch.take()) {
        ++m_res.levelEpisodes[int(e.kind)];
        ++m_levelCount;
        if (!m_plan.log) continue;
        const double endSec = double(e.endSlot + 1) * m_fmt.fps.den / m_fmt.fps.num;
        char buf[160];
        const char* what = e.kind == LevelKind::Crush ? "below 16" : e.kind == LevelKind::Clip ? "above 235" : "past the chroma limit";
        std::snprintf(buf, sizeof buf, "%s until %02d:%02d:%06.3f, worst %.2f%% %s", levelKindLabel(e.kind), int(endSec / 3600),
                      int(std::fmod(endSec, 3600) / 60), std::fmod(endSec, 60), e.worstPct, what);
        m_plan.log->log(levelKindEvent(e.kind), e.startSlot, uint64_t(e.endSlot - e.startSlot + 1), buf);
    }
}

void AnalysisWorker::logAudio() {
    const double perSample = m_plan.afmt.rate > 0 ? m_fmt.fps.toDouble() / double(m_plan.afmt.rate) : 0.0;
    for (const AudioEvent& e : m_audio.take()) {
        ++m_res.audioEvents[int(e.kind)];
        ++m_audioCount;
        if (!m_plan.log) continue;
        const int64_t s0 = int64_t(std::floor(double(e.startSample) * perSample));
        const int64_t s1 = std::max(s0, int64_t(std::floor(double(e.endSample) * perSample)));
        m_plan.log->log(audioFaultEvent(e.kind), s0, uint64_t(s1 - s0 + 1), e.detail);
    }
}

void AnalysisWorker::processTimeline(const TItem& it) {
    const VideoFrame& f = it.frame;
    const bool real = !it.inserted && f.data && !(f.flags & VF_Fill);
    BoundaryDetector::Slot kind = BoundaryDetector::Slot::Gap;
    bool analyzed = false;
    if (real && !(f.flags & VF_NoLock)) {
        if (m_an.supported()) {
            FrameStats st;
            ScopeFrame sc;
            const bool wantScope = (m_scopeN++ % uint64_t(std::max(1, m_scopeEvery))) == 0;
            analyzed = m_an.analyze(f.data->data(), f.size, st, wantScope ? &sc : nullptr);
            if (analyzed) {
                ++m_res.framesAnalyzed;
                if (wantScope) publishScope(sc);
                if (!st.blank) m_watch.add(it.slot, st);
                kind = st.blank ? BoundaryDetector::Slot::Gap : BoundaryDetector::Slot::Picture;
            } else {
                ++m_res.framesNotAnalyzed;
                kind = BoundaryDetector::Slot::Unknown;
            }
        } else {
            kind = BoundaryDetector::Slot::Picture;  // MJPEG: lock is all there is to go on
        }
    }
    if (m_bounds.add(it.slot, kind) && m_plan.chapters) m_plan.chapters->addBoundary(it.slot);
    if (m_sheet.due(it.slot) && m_an.supported()) {
        if (!analyzed) {
            if (!(f.data && m_an.load(f.data->data(), f.size))) m_an.loadBlack();
        }
        m_sheet.add(m_an.planes(), it.slot);
    }
    logEpisodes();
    {
        std::lock_guard<std::mutex> lk(m_scopeMutex);
        m_live = m_watch.live();
    }
    m_processed = it.slot;
}

void AnalysisWorker::processAudio(const AItem& a) {
    if (a.xrunFrames) {
        // The fill passes through the resampler, so allow for its delay on both sides.
        const uint64_t margin = uint64_t(std::max(1, m_plan.afmt.rate / 5));
        m_audio.ignore(a.first > margin ? a.first - margin : 0, a.first + a.xrunFrames + margin);
        return;
    }
    const uint64_t pos = m_audio.position();
    if (a.first > pos) {
        m_audio.discontinuity(a.first - pos);
        m_res.audioSkipped += a.first - pos;
    }
    m_audio.push(a.samples.data(), a.frames);
    logAudio();
}

// ---------------------------------------------------------------------------

void AnalysisWorker::beginCapture(const CaptureAnalysisPlan& p, const AnalysisSettings& s) {
    std::lock_guard<std::mutex> lk(m_procMutex);
    m_measuring = false;
    m_plan = p;
    m_set = s;
    m_an.setChromaLimit(s.chromaLimit);
    m_watch.reset(s.watch, p.vfmt.fps);
    m_bounds.reset(p.vfmt.fps);
    m_res = CaptureAnalysisResult{};
    std::string err;
    if (!m_sheet.begin(p.basePath, p.vfmt, m_an.supported() ? p.contactSheetSec : 0.0, err)) m_res.sheetError = err;
    m_audio.reset(p.afmt.rate, p.afmt.channels, s.audio);
    m_tq.clear();
    m_aq.clear();
    m_tDropped = 0;
    m_aDropped = 0;
    m_processed = -1;
    m_audioCount = 0;
    m_levelCount = 0;
    // Scopes during capture follow the timeline at about 15 updates a second.
    m_scopeEvery = std::max(1, int(std::lround(p.vfmt.fps.toDouble() / 15.0)));
    m_scopeN = 0;
    {
        std::lock_guard<std::mutex> sl(m_scopeMutex);
        m_live = LevelWatch::Live{};
    }
    m_capturing = true;
}

void AnalysisWorker::pushTimeline(const VideoFrame& f, int64_t slot, bool inserted) {
    if (!m_capturing) return;
    TItem t;
    t.frame = f;
    t.slot = slot;
    t.inserted = inserted;
    if (!m_tq.push(std::move(t), 1)) ++m_tDropped;
}

void AnalysisWorker::pushAudio(const int32_t* s, size_t frames, uint64_t first) {
    if (!m_capturing || !m_plan.haveAudio || !frames) return;
    AItem a;
    const size_t ch = size_t(std::max(1, m_plan.afmt.channels));
    a.samples.assign(s, s + frames * ch);
    a.frames = frames;
    a.first = first;
    if (!m_aq.push(std::move(a), frames * ch * sizeof(int32_t))) ++m_aDropped;
}

void AnalysisWorker::noteXrun(uint64_t atSample, uint64_t frames) {
    if (!m_capturing || !m_plan.haveAudio) return;
    AItem a;
    a.first = atSample;
    a.xrunFrames = std::max<uint64_t>(1, frames);
    m_aq.push(std::move(a), 64);
}

void AnalysisWorker::waitProcessed(int64_t slot, int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_waitMutex);
    m_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
        return m_processed.load() >= slot - 1 || (!m_busy.load() && m_tq.count() == 0);
    });
}

CaptureAnalysisResult AnalysisWorker::endCapture() {
    if (!m_capturing) return {};
    {
        // The sync thread has stopped; everything still queued is the tail of the tape.
        std::unique_lock<std::mutex> lk(m_waitMutex);
        m_waitCv.wait_for(lk, std::chrono::seconds(60),
                          [&] { return !m_busy.load() && m_tq.count() == 0 && m_aq.count() == 0; });
    }
    std::lock_guard<std::mutex> lk(m_procMutex);
    m_capturing = false;
    m_watch.finish();
    logEpisodes();
    m_audio.finish();
    logAudio();
    m_sheet.finish();
    m_res.sheet = m_sheet.info();
    if (m_res.sheetError.empty()) m_res.sheetError = m_sheet.error();
    m_res.boundaries = m_bounds.marks();
    m_res.framesNotAnalyzed += m_tDropped.load();
    m_tq.clear();
    m_aq.clear();
    m_plan.log = nullptr;
    m_plan.chapters = nullptr;
    return m_res;
}

// ---------------------------------------------------------------------------

void AnalysisWorker::startMeasure() {
    restartMeasure(0);
    m_measuring = true;
}

void AnalysisWorker::stopMeasure() { m_measuring = false; }

void AnalysisWorker::restartMeasure(int settleMs) {
    std::lock_guard<std::mutex> lk(m_procMutex);
    m_acc.reset();
    m_settleUntil = monotonicNs() + int64_t(settleMs) * 1000000LL;
}

LevelReading AnalysisWorker::reading() {
    std::lock_guard<std::mutex> lk(m_procMutex);
    return m_acc.reading();
}

bool AnalysisWorker::latestScope(ScopeFrame& out, uint64_t& serial) {
    std::lock_guard<std::mutex> lk(m_scopeMutex);
    if (m_scopeSerial == serial || m_scope.y.empty()) return false;
    out = m_scope;
    serial = m_scopeSerial;
    return true;
}

LevelWatch::Live AnalysisWorker::liveLevels() {
    std::lock_guard<std::mutex> lk(m_scopeMutex);
    return m_live;
}

}  // namespace pd
