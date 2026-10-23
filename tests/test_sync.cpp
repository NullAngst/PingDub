// SPDX-License-Identifier: GPL-3.0-or-later
// Sync engine against a simulated clock. The long cases run six hours of
// NTSC with the audio clock off by +-100 ppm, timestamp jitter on both
// streams, driver drops, a signal loss and audio xruns, then check that the
// file ends within one frame of sync and every injected fault was counted
// exactly once.
#include "engine/SyncEngine.h"
#include "testing.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <set>

using namespace pd;

namespace {

struct Sink : SyncSink {
    uint64_t videoSlots = 0;
    uint64_t insertedSlots = 0;
    uint64_t audioFrames = 0;
    int64_t markSlot = -1;
    uint32_t markSeq = 0xffffffffu;
    int64_t toneAt = -1;          // first output audio frame above threshold
    int64_t searchFrom = 0;       // ignore audio before this output index
    int channels = 1;
    std::set<int> events;
    uint64_t lastSlot = 0;
    bool slotsContiguous = true;

    void syncVideo(const VideoFrame& f, int64_t slot, bool inserted) override {
        if (uint64_t(slot) != videoSlots) slotsContiguous = false;
        ++videoSlots;
        if (inserted) ++insertedSlots;
        if (!inserted && f.seq == markSeq && markSlot < 0) markSlot = slot;
    }
    void syncAudio(const int32_t* s, size_t frames) override {
        if (toneAt < 0) {
            for (size_t i = 0; i < frames; ++i) {
                const int64_t idx = int64_t(audioFrames + i);
                if (idx >= searchFrom && std::abs(double(s[i * channels])) > 0.25 * 2147483648.0) {
                    toneAt = idx;
                    break;
                }
            }
        }
        audioFrames += frames;
    }
    void syncEvent(const SyncEvent& ev) override { events.insert(int(ev.type)); }
};

struct SimResult {
    SyncCounters c;
    double toneErrorMs = 1e9;
    double endDiffFrames = 1e9;
    bool contiguous = false;
};

// Simulates the video thread (with its stall watchdog), the audio thread
// (with its gap detector), and the sync thread merging both by timestamp.
SimResult simulate(double hours, double audioPpm, SyncMode mode, bool faults) {
    const Rational fps{30000, 1001};
    const double interval = 1e9 * fps.den / fps.num;
    const int rate = 48000;
    const double faTrue = rate * (1.0 + audioPpm * 1e-6);
    const uint32_t chunk = 480;
    const int64_t tStart = 1000000000000LL;  // arbitrary monotonic origin
    const int64_t tEnd = tStart + int64_t(hours * 3600e9);

    SyncConfig cfg;
    cfg.mode = mode;
    cfg.fps = fps;
    cfg.audioRate = rate;
    cfg.channels = 1;
    cfg.sampleBits = 16;
    Sink sink;
    sink.channels = 1;
    SyncEngine eng(cfg, &sink);

    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> vj(-0.3e6, 0.3e6);   // +-0.3 ms video timestamp jitter
    std::uniform_real_distribution<double> aj(-1.0e6, 1.0e6);   // +-1 ms audio timestamp jitter

    // Tone burst near the end (true time), and the video frame that sees it.
    const double toneTrue = double(tEnd) - 60e9;
    const double toneLen = 0.5e9;

    // Video faults: frames never delivered. Sequence numbers skip for drops;
    // during the signal loss the driver delivers nothing and does not count.
    std::set<uint64_t> dropFrames;
    uint64_t lossStart = 0, lossLen = 0;
    if (faults) {
        for (uint64_t k : {1000ull, 50000ull, 50001ull, 50002ull, 200000ull, 200001ull}) dropFrames.insert(k);
        lossStart = 300000;
        lossLen = 60;
    }
    std::set<uint64_t> xrunChunks;  // first chunk of each 10-chunk xrun
    if (faults) {
        xrunChunks.insert(100000);
        xrunChunks.insert(2000000);
    }

    StallWatchdog wd;
    wd.reset(interval, 3);
    AudioGapDetector gap;
    gap.reset(rate);

    uint64_t k = 0;       // video frame index (true clock)
    uint32_t seq = 0;
    uint64_t j = 0;       // audio chunk index
    uint64_t sampleIdx = 0;
    uint64_t inLoss = 0;
    const int64_t audioStart = tStart - 20000000;  // audio starts 20 ms early

    auto videoTrue = [&](uint64_t i) { return double(tStart) + double(i) * interval; };
    auto audioTrue = [&](uint64_t i) { return double(audioStart) + double(i) * chunk * 1e9 / faTrue; };

    // Mark the frame closest to the tone onset.
    const uint64_t markK = uint64_t(std::llround((toneTrue - tStart) / interval));
    sink.markSeq = 0xffffffffu;

    std::vector<int32_t> buf(chunk);
    uint64_t pendingXrunSkip = 0;
    bool pendingXrunFlag = false;

    while (true) {
        const double vt = videoTrue(k);
        const double at = audioTrue(j);
        if (vt > tEnd && at > tEnd) break;
        if (vt <= at) {
            // video event
            const bool lost = faults && k >= lossStart && k < lossStart + lossLen;
            const bool dropped = dropFrames.count(k) != 0;
            if (lost) {
                // Video thread wakes from poll() each interval and runs the watchdog.
                for (int64_t ts : wd.poll(int64_t(vt) + 2000000)) {
                    VideoFrame fill;
                    fill.flags = VF_Fill;
                    fill.tsNs = ts;
                    eng.pushVideo(fill);
                }
                ++inLoss;
            } else if (dropped) {
                ++seq;  // driver counted it, but it never reached us
            } else {
                VideoFrame f;
                f.data = std::make_shared<Bytes>(1, uint8_t(k & 0xff));
                f.size = 1;
                f.tsNs = int64_t(vt + vj(rng));
                f.seq = seq++;
                if (k == markK) sink.markSeq = f.seq;
                wd.onEvent(f.tsNs, f.tsNs);
                eng.pushVideo(f);
            }
            ++k;
        } else {
            // audio event
            if (faults && xrunChunks.count(j)) {
                pendingXrunSkip = 10;
                pendingXrunFlag = true;
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                const double t = double(audioStart) + double(sampleIdx + i) * 1e9 / faTrue;
                double v = 0.0;
                if (t >= toneTrue && t < toneTrue + toneLen) v = 0.5 * std::sin(2.0 * M_PI * 1000.0 * (t - toneTrue) / 1e9 + M_PI / 2);
                buf[i] = int32_t(std::lrint(v * 32767.0)) * 65536;
            }
            sampleIdx += chunk;
            if (pendingXrunSkip) {
                --pendingXrunSkip;  // lost in the overrun
            } else {
                AudioChunk c;
                c.samples = std::make_shared<std::vector<int32_t>>(buf);
                c.frames = chunk;
                c.tsNs = int64_t(at + aj(rng));
                const uint32_t silence = gap.check(c.tsNs, c.frames, pendingXrunFlag);
                if (silence) {
                    AudioChunk s;
                    s.samples = std::make_shared<std::vector<int32_t>>(size_t(silence), 0);
                    s.frames = silence;
                    s.tsNs = c.tsNs - int64_t(double(silence) * 1e9 / rate);
                    s.flags = AF_XrunFill;
                    eng.pushAudio(s);
                }
                pendingXrunFlag = false;
                eng.pushAudio(c);
            }
            ++j;
        }
    }
    (void)inLoss;
    eng.finish();

    SimResult r;
    r.c = eng.counters();
    r.contiguous = sink.slotsContiguous;
    // Where the tone should land in the output: on the timeline, the marked
    // frame sits at its slot; the tone starts (toneTrue - frameTrue) later.
    if (sink.markSlot >= 0 && sink.toneAt >= 0) {
        const double expected = (double(sink.markSlot) * interval + (toneTrue - videoTrue(markK))) * rate / 1e9;
        r.toneErrorMs = (double(sink.toneAt) - expected) / rate * 1000.0;
    }
    r.endDiffFrames = (double(sink.audioFrames) / rate - double(sink.videoSlots) * interval / 1e9) * fps.toDouble();
    if (std::getenv("PD_SYNC_DEBUG")) std::printf("  dbg slots=%llu audio=%llu (%.3f frames) audioIn=%llu\n", (unsigned long long)sink.videoSlots, (unsigned long long)sink.audioFrames, double(sink.audioFrames)/rate*fps.toDouble(), (unsigned long long)r.c.audioIn);
    return r;
}

}  // namespace

TEST_CASE("watchdog trails real time and resumes") {
    StallWatchdog wd;
    const double iv = 1e9 * 1001.0 / 30000.0;
    wd.reset(iv, 3);
    CHECK(wd.poll(5000000000LL).empty());  // nothing before the first frame
    wd.onEvent(1000000000LL, 1000000000LL);
    CHECK(wd.poll(1000000000LL + int64_t(3.5 * iv)).empty());
    auto fills = wd.poll(1000000000LL + int64_t(10.2 * iv));
    CHECK_EQ(fills.size(), size_t(7));
    CHECK(std::llabs(fills.front() - (1000000000LL + int64_t(iv))) < 2);
}

TEST_CASE("audio gap detector measures xrun gaps") {
    AudioGapDetector g;
    g.reset(48000);
    CHECK_EQ(g.check(0, 480, false), 0u);
    CHECK_EQ(g.check(10000000, 480, false), 0u);
    // 100 ms gap after an xrun
    CHECK_EQ(g.check(20000000 + 100000000, 480, true), 4800u);
    // small jitter without an xrun is ignored
    CHECK_EQ(g.check(130000000 + 1000000, 480, false), 0u);
}

TEST_CASE("video master: gap fill with last frame and black") {
    for (GapFill gf : {GapFill::LastFrame, GapFill::Black}) {
        SyncConfig cfg;
        cfg.haveAudio = false;
        cfg.gapFill = gf;
        struct S : SyncSink {
            std::vector<std::pair<bool, bool>> slots;  // inserted, hasData
            void syncVideo(const VideoFrame& f, int64_t, bool ins) override { slots.push_back({ins, bool(f.data)}); }
            void syncAudio(const int32_t*, size_t) override {}
        } sink;
        SyncEngine e(cfg, &sink);
        const double iv = e.intervalNs();
        auto frame = [&](uint32_t seq, double t) {
            VideoFrame f;
            f.data = std::make_shared<Bytes>(4);
            f.size = 4;
            f.seq = seq;
            f.tsNs = int64_t(t);
            e.pushVideo(f);
        };
        frame(10, 0);
        frame(11, iv);
        frame(14, 4 * iv);  // two missing
        const auto c = e.counters();
        CHECK_EQ(c.inserted, 2u);
        CHECK_EQ(c.driverDrops, 2u);
        CHECK_EQ(c.framesOut, 5u);
        CHECK_EQ(sink.slots.size(), size_t(5));
        CHECK(sink.slots[2].first && sink.slots[3].first);
        CHECK_EQ(sink.slots[2].second, gf == GapFill::LastFrame);
    }
}

TEST_CASE("ring drops are counted apart from driver drops") {
    SyncConfig cfg;
    cfg.haveAudio = false;
    struct S : SyncSink {
        void syncVideo(const VideoFrame&, int64_t, bool) override {}
        void syncAudio(const int32_t*, size_t) override {}
    } sink;
    SyncEngine e(cfg, &sink);
    const double iv = e.intervalNs();
    VideoFrame f;
    f.data = std::make_shared<Bytes>(1);
    f.seq = 0;
    f.tsNs = 0;
    e.pushVideo(f);
    f.seq = 4;
    f.tsNs = int64_t(4 * iv);
    f.ringDropsBefore = 3;
    e.pushVideo(f);
    const auto c = e.counters();
    CHECK_EQ(c.ringDrops, 3u);
    CHECK_EQ(c.driverDrops, 0u);
    CHECK_EQ(c.inserted, 3u);
}

TEST_CASE("field-counting drivers: sequence stride of 2") {
    SyncConfig cfg;
    cfg.haveAudio = false;
    struct S : SyncSink {
        void syncVideo(const VideoFrame&, int64_t, bool) override {}
        void syncAudio(const int32_t*, size_t) override {}
    } sink;
    SyncEngine e(cfg, &sink);
    const double iv = e.intervalNs();
    for (uint32_t i = 0; i < 20; ++i) {
        VideoFrame f;
        f.data = std::make_shared<Bytes>(1);
        f.seq = i * 2;
        f.tsNs = int64_t(i * iv);
        e.pushVideo(f);
    }
    const auto c = e.counters();
    CHECK_EQ(c.inserted, 0u);
    CHECK_EQ(c.driverDrops, 0u);
}

TEST_CASE("6 hours, audio +100 ppm, video master") {
    const SimResult r = simulate(6.0, +100.0, SyncMode::VideoMaster, true);
    std::printf("  frames=%llu inserted=%llu driverDrops=%llu xruns=%llu drift=%.1fppm offset=%.2fms tone=%.2fms end=%.3f frames\n",
                (unsigned long long)r.c.framesOut, (unsigned long long)r.c.inserted,
                (unsigned long long)r.c.driverDrops, (unsigned long long)r.c.xruns, r.c.driftPpm, r.c.avOffsetMs,
                r.toneErrorMs, r.endDiffFrames);
    CHECK_EQ(r.c.inserted, 66u);
    CHECK_EQ(r.c.driverDrops, 6u);
    CHECK_EQ(r.c.dropped, 0u);
    CHECK_EQ(r.c.xruns, 2u);
    CHECK_EQ(r.c.audioResyncs, 0u);
    CHECK(!r.c.clampHit);
    CHECK(r.contiguous);
    CHECK_MSG(std::fabs(r.toneErrorMs) < 1001.0 / 30.0, "tone off by %.2f ms", r.toneErrorMs);
    CHECK_MSG(std::fabs(r.endDiffFrames) < 1.0, "end off by %.3f frames", r.endDiffFrames);
    CHECK_MSG(std::fabs(r.toneErrorMs) < 5.0, "residual offset %.2f ms", r.toneErrorMs);
    CHECK_MSG(std::fabs(r.c.driftPpm - 100.0) < 10.0, "drift estimate %.1f ppm", r.c.driftPpm);
}

TEST_CASE("6 hours, audio -100 ppm, video master") {
    const SimResult r = simulate(6.0, -100.0, SyncMode::VideoMaster, true);
    std::printf("  frames=%llu inserted=%llu driverDrops=%llu xruns=%llu drift=%.1fppm offset=%.2fms tone=%.2fms end=%.3f frames\n",
                (unsigned long long)r.c.framesOut, (unsigned long long)r.c.inserted,
                (unsigned long long)r.c.driverDrops, (unsigned long long)r.c.xruns, r.c.driftPpm, r.c.avOffsetMs,
                r.toneErrorMs, r.endDiffFrames);
    CHECK_EQ(r.c.inserted, 66u);
    CHECK_EQ(r.c.driverDrops, 6u);
    CHECK_EQ(r.c.dropped, 0u);
    CHECK_EQ(r.c.xruns, 2u);
    CHECK_EQ(r.c.audioResyncs, 0u);
    CHECK(!r.c.clampHit);
    CHECK(r.contiguous);
    CHECK_MSG(std::fabs(r.toneErrorMs) < 1001.0 / 30.0, "tone off by %.2f ms", r.toneErrorMs);
    CHECK_MSG(std::fabs(r.endDiffFrames) < 1.0, "end off by %.3f frames", r.endDiffFrames);
    CHECK_MSG(std::fabs(r.toneErrorMs) < 5.0, "residual offset %.2f ms", r.toneErrorMs);
    CHECK_MSG(std::fabs(r.c.driftPpm + 100.0) < 10.0, "drift estimate %.1f ppm", r.c.driftPpm);
}

TEST_CASE("audio master: audio untouched, video follows within half a frame") {
    const SimResult r = simulate(0.5, +100.0, SyncMode::AudioMaster, false);
    std::printf("  frames=%llu inserted=%llu dropped=%llu audioIn=%llu audioOut=%llu tone=%.2fms end=%.3f frames\n",
                (unsigned long long)r.c.framesOut, (unsigned long long)r.c.inserted,
                (unsigned long long)r.c.dropped, (unsigned long long)r.c.audioIn,
                (unsigned long long)r.c.audioOut, r.toneErrorMs, r.endDiffFrames);
    // A fast audio clock makes the audio longer: 30 minutes at +100 ppm is
    // about 5 frames, all made up by repeating video.
    CHECK(r.c.inserted >= 4 && r.c.inserted <= 7);
    CHECK_EQ(r.c.dropped, 0u);
    CHECK_MSG(std::fabs(r.toneErrorMs) < 1001.0 / 30.0, "tone off by %.2f ms", r.toneErrorMs);
    CHECK_MSG(std::fabs(r.endDiffFrames) < 1.5, "end off by %.3f frames", r.endDiffFrames);
}

TEST_CASE("audio master, audio -100 ppm: frames dropped") {
    const SimResult r = simulate(0.5, -100.0, SyncMode::AudioMaster, false);
    std::printf("  frames=%llu inserted=%llu dropped=%llu tone=%.2fms end=%.3f frames\n",
                (unsigned long long)r.c.framesOut, (unsigned long long)r.c.inserted,
                (unsigned long long)r.c.dropped, r.toneErrorMs, r.endDiffFrames);
    CHECK(r.c.dropped >= 4 && r.c.dropped <= 7);
    CHECK_EQ(r.c.inserted, 0u);
    CHECK_MSG(std::fabs(r.toneErrorMs) < 1001.0 / 30.0, "tone off by %.2f ms", r.toneErrorMs);
    // The last frame runs past the last audio chunk, and corrections only
    // fire beyond half a frame, so the lengths can differ by up to 1.5 frames.
    CHECK_MSG(std::fabs(r.endDiffFrames) < 1.5, "end off by %.3f frames", r.endDiffFrames);
}

TEST_CASE("no correction drifts as expected") {
    const SimResult r = simulate(0.5, +100.0, SyncMode::NoCorrection, false);
    CHECK_EQ(r.c.inserted, 0u);
    CHECK_EQ(r.c.dropped, 0u);
    // 30 min * 100 ppm = 180 ms of extra audio, about 5.4 frames
    CHECK_MSG(r.endDiffFrames > 4.0 && r.endDiffFrames < 7.0, "end diff %.3f frames", r.endDiffFrames);
}

TEST_MAIN()
