// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncEngine.h"

#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pd {

const char* syncEventName(SyncEvent::Type t) {
    switch (t) {
    case SyncEvent::Insert: return "insert";
    case SyncEvent::Drop: return "drop";
    case SyncEvent::DriverDrop: return "driver_drop";
    case SyncEvent::RingOverflow: return "buffer_overflow";
    case SyncEvent::Xrun: return "audio_xrun";
    case SyncEvent::AudioResync: return "audio_resync";
    case SyncEvent::ErrorFrame: return "error_frame";
    case SyncEvent::Overfill: return "overfill";
    case SyncEvent::ClampHit: return "resample_clamp";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------

void FrameClock::anchor(int64_t tsNs, int64_t slot) {
    m_anchored = true;
    m_anchorTs = tsNs;
    m_anchorSlot = slot;
    m_window.clear();
}

FrameClock::Verdict FrameClock::judge(int64_t tsNs, int64_t nextSlot) {
    Verdict v;
    m_pending = true;
    m_lastTs = tsNs;
    m_lastSlot = nextSlot;
    if (!m_anchored) {
        anchor(tsNs, nextSlot);
        m_lastLate = 0.0;
        return v;
    }
    const double late = double(tsNs - m_anchorTs) / m_interval - double(nextSlot - m_anchorSlot);
    m_lastLate = late;
    if (m_window.empty()) return v;
    double lo = m_window.front(), hi = lo;
    for (double x : m_window) {
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    const double d = late - lo;
    if (d < -0.5) {
        v.early = true;
        return v;
    }
    if (m_seen < uint64_t(kWarmup)) {
        // Still learning the jitter: only a gap no jitter could explain counts.
        if (d >= 2.5) v.missing = uint64_t(std::floor(d + 0.5));
        return v;
    }
    const double j = std::min(std::max(m_jitter, hi - lo), kMaxJitter);
    const double m = std::floor(d + 0.5 - j / 2.0);
    v.missing = m > 0 ? uint64_t(m) : 0;
    return v;
}

void FrameClock::commit(uint64_t inserted) {
    if (!m_pending) return;
    m_pending = false;
    ++m_seen;
    const double corrected = m_lastLate - double(inserted);
    if (!m_window.empty()) {
        double lo = m_window.front();
        for (double x : m_window) lo = std::min(lo, x);
        // Ahead of the timeline: a fill or insert overshot and cannot be taken
        // back, so measure from here on. The learned jitter is kept.
        if (corrected - lo < -0.5) {
            anchor(m_lastTs, m_lastSlot + int64_t(inserted));
            m_window.push_back(0.0);
            return;
        }
    }
    m_window.push_back(corrected);
    while (m_window.size() > size_t(kWindow)) m_window.pop_front();
    double lo = m_window.front(), hi = lo;
    for (double x : m_window) {
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    // Rare jitter (one slow transfer a minute) is remembered for about 30 s.
    const double decay = std::exp(-1.0 / 900.0);
    m_jitter = std::max(hi - lo, m_jitter * decay);
}

// ---------------------------------------------------------------------------

std::vector<int64_t> StallWatchdog::poll(int64_t nowNs) {
    std::vector<int64_t> out;
    if (!m_have) return out;
    double last = double(m_last), arrival = double(m_arrival);
    while (double(nowNs) - arrival >= double(m_trail + 1) * m_interval) {
        last += m_interval;
        arrival += m_interval;
        out.push_back(int64_t(std::llround(last)));
        if (out.size() > 100000) break;  // clock jump guard
    }
    if (!out.empty()) {
        m_last = out.back();
        m_arrival = int64_t(std::llround(arrival));
    }
    return out;
}

uint32_t AudioGapDetector::check(int64_t chunkTsNs, uint32_t frames, bool xrun) {
    const double frameNs = 1e9 / double(m_rate);
    uint32_t fill = 0;
    if (m_have) {
        const int64_t gap = chunkTsNs - m_expect;
        const int64_t threshold = xrun ? 2000000 : 40000000;  // 2 ms after an xrun, 40 ms otherwise
        if (gap > threshold) {
            const double f = std::llround(double(gap) / frameNs);
            fill = f > 0 ? uint32_t(std::min<double>(f, 3600.0 * m_rate)) : 0;
        }
    }
    m_have = true;
    m_expect = chunkTsNs + int64_t(std::llround(frames * frameNs));
    return fill;
}

// ---------------------------------------------------------------------------

SyncEngine::SyncEngine(const SyncConfig& cfg, SyncSink* sink) : m_cfg(cfg), m_sink(sink) {
    m_interval = 1e9 * double(cfg.fps.den) / double(cfg.fps.num);
    m_clock.reset(m_interval);
    if (m_cfg.haveAudio && m_cfg.mode == SyncMode::VideoMaster) {
        soxr_error_t err = nullptr;
        soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
        const double maxRatio = 1.0 + m_cfg.clampPpm * 1e-6 * 2.0;
        m_soxr = soxr_create(maxRatio, 1.0, unsigned(m_cfg.channels), &err, &io, &q, nullptr);
        if (err || !m_soxr) {
            m_soxr = nullptr;
        } else {
            soxr_set_io_ratio(m_soxr, 1.0, 0);
            m_latencyCorr = calibrateLatency(m_cfg.clampPpm, &m_firstLatency);
        }
    }
}

// soxr_delay() reports buffered output samples, which is not quite the
// group delay of the filter. Measure the real figure once with an impulse so
// the offset the PI loop drives to zero is the true one.
double SyncEngine::calibrateLatency(double clampPpm, double* firstLatency) {
    // Two measurements on throwaway resamplers set up like the real one:
    // where a sample fed into an empty resampler comes out, and how far the
    // steady-state output position differs from soxr_delay()'s estimate.
    auto make = [&]() -> soxr_t {
        soxr_error_t err = nullptr;
        soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
        soxr_t s = soxr_create(1.0 + clampPpm * 1e-6 * 2.0, 1.0, 1, &err, &io, &q, nullptr);
        if (err) return nullptr;
        soxr_set_io_ratio(s, 1.0, 0);
        return s;
    };
    const size_t chunk = 480, total = 9600;
    auto run = [&](size_t impulseAt, double& predicted) -> double {
        soxr_t s = make();
        if (!s) return -1.0;
        std::vector<float> in(total, 0.0f), out(chunk * 4);
        in[impulseAt] = 1.0f;
        predicted = -1.0;
        size_t emitted = 0, peakIdx = 0;
        float peak = 0.0f;
        for (size_t pos = 0; pos < total; pos += chunk) {
            if (pos <= impulseAt && impulseAt < pos + chunk)
                predicted = double(emitted) + soxr_delay(s) + double(impulseAt - pos);
            size_t done = 0, idone = 0;
            soxr_process(s, in.data() + pos, chunk, &idone, out.data(), out.size(), &done);
            for (size_t i = 0; i < done; ++i) {
                if (std::fabs(out[i]) > peak) {
                    peak = std::fabs(out[i]);
                    peakIdx = emitted + i;
                }
            }
            emitted += done;
        }
        soxr_delete(s);
        return peak < 0.1f ? -1.0 : double(peakIdx);
    };
    double pred0 = 0, predSteady = 0;
    const double first = run(0, pred0);
    const double steady = run(4800 + 17, predSteady);
    if (firstLatency) *firstLatency = first < 0 ? 0.0 : first;
    if (steady < 0 || predSteady < 0) return 0.0;
    return steady - predSteady;
}

SyncEngine::~SyncEngine() {
    if (m_soxr) soxr_delete(m_soxr);
}

void SyncEngine::event(SyncEvent::Type t, uint64_t count, const std::string& detail) {
    if (!m_sink) return;
    SyncEvent ev;
    ev.type = t;
    ev.slot = m_nextSlot;
    ev.count = count;
    ev.detail = detail;
    m_sink->syncEvent(ev);
}

void SyncEngine::publish() {
    m_c.tsJitterMs = m_clock.jitterFrames() * m_interval / 1e6;
    std::lock_guard<std::mutex> lk(m_pubMutex);
    m_pub = m_c;
}

SyncCounters SyncEngine::counters() const {
    std::lock_guard<std::mutex> lk(m_pubMutex);
    return m_pub;
}

int64_t SyncEngine::slotForTs(int64_t tsNs) const {
    std::lock_guard<std::mutex> lk(m_pubMutex);
    if (m_slotMap.empty()) return -1;
    auto it = std::lower_bound(m_slotMap.begin(), m_slotMap.end(), tsNs,
                               [](const SlotTs& a, int64_t t) { return a.ts < t; });
    const SlotTs* best = nullptr;
    if (it != m_slotMap.end()) best = &*it;
    if (it != m_slotMap.begin()) {
        const SlotTs& prev = *(it - 1);
        if (!best || std::llabs(prev.ts - tsNs) < std::llabs(best->ts - tsNs)) best = &prev;
    }
    if (!best || double(std::llabs(best->ts - tsNs)) > 2.0 * m_interval) return -1;
    return best->slot;
}

// ---------------------------------------------------------------------------
// video

void SyncEngine::emitVideo(const VideoFrame& f, bool inserted) {
    if (m_cfg.maxFrames && uint64_t(m_nextSlot) >= m_cfg.maxFrames) return;  // timed capture is full
    if (m_sink) m_sink->syncVideo(f, m_nextSlot, inserted);
    if (!inserted && !(f.flags & VF_Fill)) {
        std::lock_guard<std::mutex> lk(m_pubMutex);
        m_slotMap.push_back({f.tsNs, m_nextSlot});
        while (m_slotMap.size() > 900) m_slotMap.pop_front();
    }
    ++m_nextSlot;
    ++m_c.framesOut;
}

void SyncEngine::flushFillRun() {
    if (!m_fillRun) return;
    if (m_sink) {
        SyncEvent ev;
        ev.type = SyncEvent::Insert;
        ev.slot = m_fillRunStart;
        ev.count = m_fillRun;
        ev.detail = "no frames from device";
        m_sink->syncEvent(ev);
    }
    m_fillRun = 0;
}

void SyncEngine::emitInserted(uint64_t n, const char* why, bool fillRun) {
    if (m_cfg.maxFrames) n = std::min<uint64_t>(n, m_cfg.maxFrames > uint64_t(m_nextSlot) ? m_cfg.maxFrames - uint64_t(m_nextSlot) : 0);
    if (!n) return;
    VideoFrame content;
    if (m_cfg.gapFill == GapFill::LastFrame && m_lastReal.data) {
        content = m_lastReal;
        content.flags &= ~VF_Fill;
    }
    if (fillRun) {
        // A signal loss can run for hours: log it once as a run, not per frame.
        if (!m_fillRun) m_fillRunStart = m_nextSlot;
        m_fillRun += n;
    } else {
        event(SyncEvent::Insert, n, why);
    }
    for (uint64_t i = 0; i < n; ++i) emitVideo(content, true);
    m_c.inserted += n;
}

void SyncEngine::startTimeline(const VideoFrame& f) {
    m_started = true;
    m_t0 = f.tsNs;
    m_lastEventTs = f.tsNs;
    m_lastSeq = f.seq;
    m_lastReal = f;
    m_c.started = true;
    m_c.t0Ns = m_t0;
    ++m_c.realFrames;
    emitVideo(f, false);
    std::deque<AudioChunk> pending;
    pending.swap(m_pendingAudio);
    for (const AudioChunk& c : pending) processAudio(c);
}

void SyncEngine::pushVideo(const VideoFrame& f) {
    if (full()) return;
    if (!m_started) {
        if (!(f.flags & VF_Fill)) startTimeline(f);
        publish();
        return;
    }

    if (f.flags & VF_Fill) {
        if (m_cfg.mode == SyncMode::NoCorrection) return;
        ++m_fillsSinceReal;
        if (m_cfg.mode == SyncMode::AudioMaster && m_haveAudioMap) {
            placeAudioMaster(f, true);
        } else {
            emitInserted(1, "no frames from device", true);
            m_lastEventTs = f.tsNs;
        }
        publish();
        return;
    }
    flushFillRun();

    // Sequence numbers: some drivers count fields, so learn the stride.
    const uint32_t seqDelta = f.seq - m_lastSeq;
    if (m_strideProbe < 3) {
        m_strideProbe = (seqDelta == 2) ? m_strideProbe + 1 : 3;
        if (m_strideProbe == 3 && seqDelta == 2) m_seqStride = 2;
    }
    // While the stride is being learned, gaps come from timestamps alone.
    uint64_t seqGap = 0;
    if (m_strideProbe >= 3 && seqDelta > m_seqStride && seqDelta < 1000000u) seqGap = seqDelta / m_seqStride - 1;

    const uint64_t ringDrops = f.ringDropsBefore;
    if (ringDrops) {
        m_c.ringDrops += ringDrops;
        event(SyncEvent::RingOverflow, ringDrops, "RAM ring full");
    }
    const uint64_t driver = seqGap > ringDrops ? seqGap - ringDrops : 0;
    if (driver) {
        m_c.driverDrops += driver;
        event(SyncEvent::DriverDrop, driver, "sequence gap");
    }
    if (f.flags & VF_Error) {
        ++m_c.errorFrames;
        event(SyncEvent::ErrorFrame, 1, "driver flagged frame as damaged, kept");
    }

    const uint64_t missingSeq = seqGap > m_fillsSinceReal ? seqGap - m_fillsSinceReal : 0;
    const FrameClock::Verdict verdict = m_clock.judge(f.tsNs, m_nextSlot);
    if (m_fillsSinceReal && verdict.early)
        event(SyncEvent::Overfill, 1, "frame arrived inside a filled gap");
    const uint64_t missing = std::max(missingSeq, verdict.missing);

    m_lastSeq = f.seq;
    m_fillsSinceReal = 0;

    switch (m_cfg.mode) {
    case SyncMode::NoCorrection:
        m_lastReal = f;
        ++m_c.realFrames;
        emitVideo(f, false);
        m_lastEventTs = f.tsNs;
        break;
    case SyncMode::AudioMaster:
        if (m_haveAudioMap) {
            placeAudioMaster(f, false);
            break;
        }
        [[fallthrough]];
    case SyncMode::VideoMaster: {
        const int64_t before = m_nextSlot;
        emitInserted(missing, "gap in video");
        m_clock.commit(uint64_t(m_nextSlot - before));
        m_lastReal = f;
        ++m_c.realFrames;
        emitVideo(f, false);
        m_lastEventTs = f.tsNs;
        break;
    }
    }
    publish();
}

void SyncEngine::placeAudioMaster(const VideoFrame& f, bool isFill) {
    const double rate = m_cfg.audioRate;
    const double ta = (m_aMapPos + double(f.tsNs - m_aMapTs) * rate / 1e9) / rate;
    const double p = ta * m_cfg.fps.toDouble();
    const double d = p - double(m_nextSlot);
    if (d < -0.6) {
        ++m_c.dropped;
        event(SyncEvent::Drop, 1, "audio master: video ahead of audio");
        if (!isFill) {
            m_lastReal = f;
            ++m_c.realFrames;
        }
        return;
    }
    if (d > 0.6) emitInserted(uint64_t(std::llround(d)), "audio master: video behind audio");
    if (isFill) {
        emitInserted(1, "no frames from device", true);
    } else {
        m_lastReal = f;
        ++m_c.realFrames;
        emitVideo(f, false);
    }
    m_lastEventTs = f.tsNs;
}

// ---------------------------------------------------------------------------
// audio

double SyncEngine::audioTargetFor(int64_t tsNs) const {
    const double lastSlot = double(m_nextSlot - 1);
    const double tNs = lastSlot * m_interval + double(tsNs - m_lastEventTs);
    return tNs * double(m_cfg.audioRate) / 1e9;
}

void SyncEngine::pushAudio(const AudioChunk& c) {
    if (!m_cfg.haveAudio || !c.frames) return;
    if (!m_started) {
        m_pendingAudio.push_back(c);
        while (!m_pendingAudio.empty() && m_pendingAudio.front().tsNs < c.tsNs - 3000000000LL)
            m_pendingAudio.pop_front();
        return;
    }
    if (full() && m_c.audioOut >= audioTargetTotal()) return;
    processAudio(c);
    publish();
}

bool SyncEngine::full() const { return m_cfg.maxFrames && uint64_t(m_nextSlot) >= m_cfg.maxFrames; }

uint64_t SyncEngine::audioTargetTotal() const {
    return uint64_t(std::llround(double(m_nextSlot) * m_cfg.audioRate * m_cfg.fps.den / double(m_cfg.fps.num)));
}

void SyncEngine::writeSilence(uint64_t frames) {
    static const std::vector<int32_t> zeros(4096 * 8, 0);
    const size_t ch = size_t(m_cfg.channels);
    const size_t block = zeros.size() / ch;
    while (frames) {
        const size_t n = size_t(std::min<uint64_t>(frames, block));
        if (m_sink) m_sink->syncAudio(zeros.data(), n);
        m_c.audioOut += n;
        frames -= n;
    }
}

void SyncEngine::feedResampler(const int32_t* in, size_t frames) {
    const size_t ch = size_t(m_cfg.channels);
    if (m_fin.size() < frames * ch) m_fin.resize(frames * ch);
    constexpr float kIn = 1.0f / 2147483648.0f;
    for (size_t i = 0; i < frames * ch; ++i) m_fin[i] = float(in[i]) * kIn;

    const size_t olen = frames + frames / 50 + 512;
    if (m_fout.size() < olen * ch) m_fout.resize(olen * ch);
    if (m_iout.size() < olen * ch) m_iout.resize(olen * ch);

    size_t consumed = 0;
    int guard = 0;
    while (consumed < frames && guard++ < 64) {
        size_t idone = 0, odone = 0;
        soxr_process(m_soxr, m_fin.data() + consumed * ch, frames - consumed, &idone, m_fout.data(), olen,
                     &odone);
        consumed += idone;
        if (odone) {
            // Back to int32, left-justified, at the device's own depth.
            const int bits = m_cfg.sampleBits;
            const double scale = std::ldexp(1.0, bits - 1);
            const double maxv = scale - 1.0;
            const int shift = 32 - bits;
            for (size_t i = 0; i < odone * ch; ++i) {
                double v = double(m_fout[i]) * scale;
                if (bits <= 16) {
                    // TPDF dither, one LSB, for the requantization the resampler forces.
                    m_dither = m_dither * 1664525u + 1013904223u;
                    const double r1 = double(m_dither >> 8) / 16777216.0;
                    m_dither = m_dither * 1664525u + 1013904223u;
                    const double r2 = double(m_dither >> 8) / 16777216.0;
                    v += r1 - r2;
                }
                v = std::nearbyint(v);
                if (v > maxv) v = maxv;
                if (v < -scale) v = -scale;
                m_iout[i] = int32_t(int64_t(v) * (int64_t(1) << shift));
            }
            size_t keep = odone;
            if (full()) {
                const uint64_t target = audioTargetTotal();
                keep = m_c.audioOut >= target ? 0 : size_t(std::min<uint64_t>(odone, target - m_c.audioOut));
            }
            if (m_sink && keep) m_sink->syncAudio(m_iout.data(), keep);
            m_c.audioOut += keep;
        }
        if (!idone && !odone) break;
    }
}

void SyncEngine::updateLoop() {
    const double rate = m_cfg.audioRate;
    const double eSec = m_eEma / rate;
    const double wn = 1.0 / std::max(1.0, m_cfg.loopSeconds);
    const double kp = 2.0 * wn;  // critically damped
    const double ki = wn * wn;
    const double newInteg = m_integ + eSec * 1.0;
    double u = 1.0 + kp * eSec + ki * newInteg;
    const double clamp = m_cfg.clampPpm * 1e-6;
    if (u > 1.0 + clamp || u < 1.0 - clamp) {
        u = std::clamp(u, 1.0 - clamp, 1.0 + clamp);
        if (!m_c.clampHit) event(SyncEvent::ClampHit, 1, "resample ratio hit the +-0.5% limit; check the audio device");
        m_c.clampHit = true;
    } else {
        m_integ = newInteg;
    }
    m_ioRatio = u;
    if (m_soxr) soxr_set_io_ratio(m_soxr, u, size_t(rate));
    m_c.ratio = u;
    // The correction actually applied; it settles on the clock error.
    m_c.driftPpm = (u - 1.0) * 1e6;
}

void SyncEngine::processAudio(const AudioChunk& c) {
    const double rate = m_cfg.audioRate;
    const size_t ch = size_t(m_cfg.channels);
    const int64_t chunkEnd = c.tsNs + int64_t(double(c.frames) * 1e9 / rate);
    if (chunkEnd <= m_t0 && !m_audioStarted) return;  // entirely before capture start

    if (c.flags & AF_XrunFill) {
        ++m_c.xruns;
        m_c.xrunSilence += c.frames;
        event(SyncEvent::Xrun, c.frames, "silence inserted to cover the gap");
        if (m_cfg.mode == SyncMode::NoCorrection) return;
    }

    const int32_t* in = c.samples ? c.samples->data() : nullptr;
    if (!in) return;
    size_t n = c.frames;
    int64_t firstTs = c.tsNs;
    m_c.audioIn += n;

    const double target = audioTargetFor(c.tsNs);
    const double hard = m_cfg.hardResyncFrames * rate / m_cfg.fps.toDouble();

    if (m_cfg.mode == SyncMode::VideoMaster && m_soxr) {
        // Before the first sample goes in, the resampler is empty and its
        // latency is the measured first-sample figure.
        const double actual = m_audioStarted ? double(m_c.audioOut) + soxr_delay(m_soxr) + m_latencyCorr
                                             : double(m_c.audioOut) + m_firstLatency;
        double e = actual - target;
        if (!m_audioStarted || std::fabs(e) > hard) {
            if (m_audioStarted) {
                ++m_c.audioResyncs;
                event(SyncEvent::AudioResync, uint64_t(std::llround(std::fabs(e))),
                      e < 0 ? "audio behind, padded with silence" : "audio ahead, input trimmed");
            }
            if (e < 0) {
                writeSilence(uint64_t(std::llround(-e)));
            } else {
                const size_t skip = size_t(std::llround(e));
                if (skip >= n) return;
                in += skip * ch;
                n -= skip;
            }
            m_audioStarted = true;
            m_eEma = 0.0;
            m_eEmaInit = true;
        } else {
            const double alpha = std::min(1.0, double(n) / (rate * 2.0));  // ~2 s smoothing
            m_eEma += alpha * (e - m_eEma);
        }
        feedResampler(in, n);
        m_sinceUpdate += n;
        while (m_sinceUpdate >= uint64_t(rate)) {
            m_sinceUpdate -= uint64_t(rate);
            updateLoop();
        }
        m_c.avOffsetMs = m_eEma / rate * 1000.0;
        return;
    }

    // Audio master, no correction, or video master without a resampler:
    // samples pass through untouched after the start is aligned.
    double e = double(m_c.audioOut) - target;
    if (!m_audioStarted) {
        if (e < 0) {
            writeSilence(uint64_t(std::llround(-e)));
        } else {
            const size_t skip = size_t(std::llround(e));
            if (skip >= n) return;
            in += skip * ch;
            n -= skip;
            firstTs += int64_t(double(skip) * 1e9 / rate);
        }
        m_audioStarted = true;
        e = 0;
    }
    m_aMapPos = double(m_c.audioOut);
    m_aMapTs = firstTs;
    m_haveAudioMap = true;
    if (full()) {
        const uint64_t target = audioTargetTotal();
        n = m_c.audioOut >= target ? 0 : size_t(std::min<uint64_t>(n, target - m_c.audioOut));
    }
    if (m_sink && n) m_sink->syncAudio(in, n);
    m_c.audioOut += n;
    const double alpha = std::min(1.0, double(n) / (rate * 2.0));
    m_eEma += alpha * (e - m_eEma);
    m_c.avOffsetMs = m_eEma / rate * 1000.0;
}

void SyncEngine::finish() {
    flushFillRun();
    if (m_soxr && m_audioStarted) {
        const size_t ch = size_t(m_cfg.channels);
        const size_t olen = 8192;
        if (m_fout.size() < olen * ch) m_fout.resize(olen * ch);
        if (m_iout.size() < olen * ch) m_iout.resize(olen * ch);
        const uint64_t target = uint64_t(std::llround(double(m_c.framesOut) * m_cfg.audioRate / m_cfg.fps.toDouble()));
        for (int guard = 0; guard < 256; ++guard) {
            size_t odone = 0;
            soxr_process(m_soxr, nullptr, 0, nullptr, m_fout.data(), olen, &odone);
            if (!odone) break;
            // Never write past the end of the video.
            if (m_c.audioOut >= target) break;
            odone = size_t(std::min<uint64_t>(odone, target - m_c.audioOut));
            const double scale = std::ldexp(1.0, m_cfg.sampleBits - 1);
            const int shift = 32 - m_cfg.sampleBits;
            for (size_t i = 0; i < odone * ch; ++i) {
                double v = std::nearbyint(double(m_fout[i]) * scale);
                v = std::clamp(v, -scale, scale - 1.0);
                m_iout[i] = int32_t(int64_t(v) * (int64_t(1) << shift));
            }
            if (m_sink) m_sink->syncAudio(m_iout.data(), odone);
            m_c.audioOut += odone;
        }
    }
    if (m_cfg.haveAudio && m_cfg.mode == SyncMode::VideoMaster && m_audioStarted) {
        const uint64_t target = uint64_t(std::llround(double(m_c.framesOut) * m_cfg.audioRate / m_cfg.fps.toDouble()));
        if (m_c.audioOut < target) writeSilence(target - m_c.audioOut);
    }
    publish();
}

}  // namespace pd
