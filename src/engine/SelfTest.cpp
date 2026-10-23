// SPDX-License-Identifier: GPL-3.0-or-later
// --self-test: a real 10-second capture through the whole pipeline (device
// threads, rings, sync engine, encoder, muxer, sidecars, tag patching) using
// the fake backends, then a check of the file that came out.
#include "CaptureSession.h"
#include "Clock.h"
#include "backends/FakeBackends.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

namespace pd {

int runSelfTest(bool verbose) {
    namespace fs = std::filesystem;
    std::string base = fs::temp_directory_path().string() + "/pingdub-selftest-XXXXXX";
    std::vector<char> tmpl(base.begin(), base.end());
    tmpl.push_back(0);
    if (!mkdtemp(tmpl.data())) {
        std::fprintf(stderr, "self-test: cannot create a temporary folder\n");
        return 2;
    }
    const std::string dir = tmpl.data();
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
        if (!ok) ++failures;
    };
    std::printf("PingDub %s self-test\n", PINGDUB_VERSION);

    FakeVideoOptions vo;
    vo.ntsc = true;
    vo.dropEverySec = 3.0;   // driver-side drops
    vo.lossAtSec = 6.0;      // half a second with no frames
    vo.lossLenSec = 0.5;
    FakeAudioOptions ao;
    ao.ppm = 80;             // audio clock off by 80 ppm
    ao.xrunEverySec = 4.0;   // overruns
    auto video = std::make_unique<FakeVideoBackend>(vo);
    FakeVideoBackend* fv = video.get();
    auto audio = std::make_unique<FakeAudioBackend>(ao);

    SessionConfig cfg;
    cfg.output.folder = dir;
    cfg.output.container = Container::MKV;
    cfg.output.vcodec = VideoCodec::FFV1;
    cfg.output.acodec = AudioCodec::PCM;
    cfg.timing.sync = SyncMode::VideoMaster;
    cfg.timing.ringBytes = 256u << 20;
    cfg.stop.durationSec = 10.0;
    cfg.stop.minFreeBytes = 0;
    cfg.realtime = false;
    cfg.captureVbi = false;

    CaptureSession session;
    std::string err;
    if (!session.open(std::move(video), std::move(audio), cfg, err)) {
        std::printf("  [FAIL] open fake devices: %s\n", err.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    VideoFrame pv;
    uint64_t serial = 0;
    check(session.latestFrame(pv, serial), "preview delivers frames before capture");
    check(session.levels().chunks > 0, "audio meters run before capture");

    std::atomic<bool> done{false};
    CaptureSummary summary;
    session.onAutoStop = [&](const CaptureSummary& s) {
        summary = s;
        done = true;
    };
    if (!session.startCapture("self test", err)) {
        std::printf("  [FAIL] start capture: %s\n", err.c_str());
        return 1;
    }
    const int64_t t0 = monotonicNs();
    while (!done && monotonicNs() - t0 < 40000000000LL) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (verbose) {
            const SessionStats st = session.stats();
            std::printf("    %.1fs frames=%llu inserted=%llu drops=%llu xruns=%llu offset=%.2fms drift=%.1fppm ring=%.1f%%\n",
                        st.elapsedSec, (unsigned long long)st.sync.framesOut, (unsigned long long)st.sync.inserted,
                        (unsigned long long)st.sync.driverDrops, (unsigned long long)st.sync.xruns, st.sync.avOffsetMs,
                        st.sync.driftPpm, st.ringFill * 100);
        }
    }
    if (!done) summary = session.stopCapture("self-test timeout");
    session.close();

    check(summary.ok, "capture finished cleanly" + (summary.error.empty() ? std::string() : ": " + summary.error));
    check(summary.stopReason == "duration reached", "stopped by the duration condition (" + summary.stopReason + ")");
    const SyncCounters& c = summary.sync;
    char line[256];
    std::snprintf(line, sizeof line, "timeline %llu frames, %.2f s", (unsigned long long)c.framesOut, summary.seconds);
    check(c.framesOut >= 299 && c.framesOut <= 302, line);
    std::snprintf(line, sizeof line, "driver drops counted (%llu, device skipped %llu)", (unsigned long long)c.driverDrops,
                  (unsigned long long)fv->framesSkipped());
    check(c.driverDrops >= 2 && c.driverDrops == fv->framesSkipped(), line);
    std::snprintf(line, sizeof line, "signal loss and drops filled (%llu inserted)", (unsigned long long)c.inserted);
    check(c.inserted >= c.driverDrops + 12, line);
    std::snprintf(line, sizeof line, "xruns detected and filled (%llu, %llu silence frames)", (unsigned long long)c.xruns,
                  (unsigned long long)c.xrunSilence);
    check(c.xruns >= 2, line);
    const double expectAudio = double(c.framesOut) * 48000.0 * 1001.0 / 30000.0;
    std::snprintf(line, sizeof line, "audio length matches video (%llu vs %.0f samples)", (unsigned long long)c.audioOut, expectAudio);
    check(std::fabs(double(c.audioOut) - expectAudio) < 1601.6, line);
    check(summary.files.size() == 1, "one file written");

    if (!summary.files.empty()) {
        const std::string f = summary.files[0];
        AVFormatContext* fc = nullptr;
        const bool opened = avformat_open_input(&fc, f.c_str(), nullptr, nullptr) == 0;
        check(opened, "file opens: " + f);
        if (opened) {
            avformat_find_stream_info(fc, nullptr);
            const int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            const int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            check(vi >= 0 && ai >= 0, "video and audio streams present");
            if (vi >= 0) {
                const AVCodecParameters* p = fc->streams[vi]->codecpar;
                check(p->codec_id == AV_CODEC_ID_FFV1, "video is FFV1");
                check(p->width == 720 && p->height == 480, "720x480, nothing cropped or scaled");
                check(p->format == AV_PIX_FMT_YUV422P, "4:2:2 kept");
                check(p->field_order == AV_FIELD_BB, "field order BFF");
                const AVRational sar = fc->streams[vi]->sample_aspect_ratio.num ? fc->streams[vi]->sample_aspect_ratio
                                                                                 : p->sample_aspect_ratio;
                check(sar.num == 10 && sar.den == 11, "SAR 10:11");
                check(p->color_primaries == AVCOL_PRI_SMPTE170M && p->color_range == AVCOL_RANGE_MPEG, "BT.601 color tags");
            }
            int64_t vpk = 0;
            AVPacket* pkt = av_packet_alloc();
            while (av_read_frame(fc, pkt) >= 0) {
                if (pkt->stream_index == vi) ++vpk;
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
            std::snprintf(line, sizeof line, "every timeline frame is in the file (%lld packets)", (long long)vpk);
            check(uint64_t(vpk) == c.framesOut, line);
            const AVDictionaryEntry* e = av_dict_get(fc->metadata, "PINGDUB_FRAMES", nullptr, 0);
            check(e && std::strtoull(e->value, nullptr, 10) == c.framesOut, "totals written into the tags");
            const AVDictionaryEntry* d = av_dict_get(fc->metadata, "PINGDUB_DEVICE", nullptr, 0);
            check(d && std::string(d->value) == "PingDub test pattern", "device recorded in the tags");
            avformat_close_input(&fc);
        }
        bool csv = false, json = false;
        for (const auto& s : summary.sidecars) {
            csv |= s.size() > 8 && s.compare(s.size() - 8, 8, ".log.csv") == 0 && fs::exists(s);
            json |= s.size() > 5 && s.compare(s.size() - 5, 5, ".json") == 0 && fs::exists(s);
        }
        check(csv, "event log sidecar written");
        check(json, "JSON sidecar written");
    }

    if (failures == 0) {
        std::printf("self-test passed\n");
        if (!verbose) fs::remove_all(dir);
        else std::printf("files kept in %s\n", dir.c_str());
        return 0;
    }
    std::printf("self-test FAILED (%d checks), files kept in %s\n", failures, dir.c_str());
    return 1;
}

}  // namespace pd
