// SPDX-License-Identifier: GPL-3.0-or-later
// The capture report: a capture made before this update (old sidecars, no
// thumbnails, no level or audio events) opens without errors and shows what
// its sidecars hold; a new capture through the real session gets its
// contact sheet, chapters and checksums; events cluster into spots; a frame
// decodes from the file. Runs the real window on Qt's offscreen platform.
#include "backends/FakeBackends.h"
#include "capture_util.h"
#include "engine/CaptureRecord.h"
#include "engine/CaptureSession.h"
#include "engine/Clock.h"
#include "testing.h"
#include "ui/ReportWindow.h"

#include <QApplication>

#include <filesystem>
#include <fstream>
#include <thread>

using namespace pd;
using namespace pdtest;
namespace fs = std::filesystem;

namespace {

// A sidecar pair the way PingDub wrote them before this update.
std::string oldCapture() {
    static std::string made;
    if (!made.empty()) return made;
    WriteOptions o;
    o.frames = 90;
    const auto w = writeTestCapture("2026-10-20_120000_old_tape", o);
    const std::string mkv = w.files.empty() ? std::string() : w.files[0];
    const std::string base = tmpDir() + "/2026-10-20_120000_old_tape";
    std::ofstream(base + ".json") << R"({
  "pingdub_version": "0.9.0",
  "capture_start": "2026-10-20T12:00:00-0400",
  "capture_end": "2026-10-20T12:00:03-0400",
  "stop_reason": "stopped by user",
  "tape_label": "old tape",
  "files": [")" << mkv << R"("],
  "sidecars": [],
  "device": {"name": "gvusb2", "driver": "gvusb2", "backend": "V4L2", "path": "/dev/video0", "input": "S-Video", "crop": ""},
  "video": {"standard": "NTSC-M", "pixel_format": "YUYV", "width": 160, "height": 120, "frame_rate": "30000/1001",
            "field_order": "BFF", "sar": "10:11", "color": "smpte170m, tv range", "codec": "FFV1", "container": "Matroska"},
  "audio": {"present": true},
  "settings": {"sync_mode": "video-master"},
  "totals": {"seconds": 3.003, "frames": 90, "inserted": 3, "dropped": 0, "driver_drops": 1, "buffer_overflow": 0, "xruns": 1},
  "warnings": ["Realtime priority not granted"]
}
)";
    std::ofstream(base + ".log.csv") << "wallclock,timeline_frame,timeline_time,event,count,detail\n"
                                        "2026-10-20T12:00:00-0400,,,capture_start,0,\"gvusb2, NTSC-M\"\n"
                                        "2026-10-20T12:00:01-0400,20,00:00:00.667,driver_drop,1,sequence gap\n"
                                        "2026-10-20T12:00:01-0400,20,00:00:00.667,insert,1,gap in video\n"
                                        "2026-10-20T12:00:02-0400,60,00:00:02.002,audio_xrun,2400,silence inserted to cover the gap\n"
                                        "2026-10-20T12:00:02-0400,70,00:00:02.336,insert,2,\"no frames, from device\"\n"
                                        "2026-10-20T12:00:03-0400,90,00:00:03.003,capture_stop,90,stopped by user\n";
    made = mkv;
    return mkv;
}

}  // namespace

TEST_CASE("csv lines with quotes and commas") {
    std::vector<std::string> f;
    CHECK(parseCsvLine("a,\"b, c\",\"say \"\"hi\"\"\",", f));
    CHECK_EQ(f.size(), size_t(4));
    if (f.size() == 4) {
        CHECK(f[1] == "b, c");
        CHECK(f[2] == "say \"hi\"");
        CHECK(f[3].empty());
    }
}

TEST_CASE("events close together are one spot") {
    std::vector<RecordEvent> ev(4);
    ev[0].slot = ev[0].endSlot = 100;
    ev[0].lane = ReportLane::Video;
    ev[1].slot = 120;  // under 5 s later: same spot
    ev[1].endSlot = 130;
    ev[1].lane = ReportLane::Audio;
    ev[2].slot = ev[2].endSlot = 2000;
    ev[2].lane = ReportLane::Levels;
    ev[3].slot = ev[3].endSlot = 5000;
    ev[3].lane = ReportLane::Info;  // not reviewable
    CHECK_EQ(countSpots(ev, {30000, 1001}), 2);
}

TEST_CASE("an old capture loads and shows what its sidecars hold") {
    const std::string mkv = oldCapture();
    CaptureRecord r;
    std::string err;
    CHECK_MSG(loadCaptureRecord(mkv, r, err), "%s", err.c_str());
    CHECK(r.haveJson && r.haveCsv);
    CHECK_EQ(r.frames, int64_t(90));
    CHECK(r.label == "old tape");
    CHECK(r.sheet.empty());
    CHECK(r.chapters.empty());
    CHECK_EQ(r.events.size(), size_t(6));
    // driver_drop + insert at 20, xrun at 60, insert at 70: within 5 s of each other.
    CHECK_EQ(r.spots, 1);
    CHECK(r.verdict() == "Review 1 spot");
    CHECK_EQ(r.segments.size(), size_t(1));
    // Loading from either sidecar finds the same capture.
    CaptureRecord r2;
    CHECK(loadCaptureRecord(captureBaseFor(mkv) + ".log.csv", r2, err));
    CHECK(r2.mediaPath == mkv);
    RgbImage img;
    CHECK_MSG(decodeFrameRgb(mkv, 45, img, err), "%s", err.c_str());
    CHECK_EQ(img.width, 160);
    CHECK_EQ(img.height, 120);
    CHECK_EQ(img.rgb.size(), size_t(160 * 120 * 3));
    CHECK(img.sar.num == 10 && img.sar.den == 11);
}

TEST_CASE("the report window opens an old capture and a new one") {
    int argc = 1;
    char name[] = "test_report";
    char* argv[] = {name, nullptr};
    QApplication app(argc, argv);

    const std::string oldMkv = oldCapture();
    {
        auto* w = new ReportWindow();
        QString err;
        CHECK_MSG(w->load(QString::fromStdString(oldMkv), err), "%s", err.toLocal8Bit().constData());
        w->show();
        app.processEvents();
        CHECK(w->record().sheet.empty());
        w->close();
        app.processEvents();
    }

    // A new capture through the whole session: fake devices, 6 seconds.
    FakeVideoOptions vo;
    vo.lossAtSec = 3.0;
    vo.lossLenSec = 0.8;  // a recording boundary
    auto video = std::make_unique<FakeVideoBackend>(vo);
    auto audio = std::make_unique<FakeAudioBackend>();
    SessionConfig cfg;
    cfg.output.folder = tmpDir() + "/{title}";
    cfg.output.contactSheetSec = 1;
    cfg.output.chapterBoundaries = true;
    cfg.output.chapterIntervalMin = 5;
    cfg.stop.durationSec = 6;
    cfg.stop.minFreeBytes = 0;
    cfg.realtime = false;
    cfg.captureVbi = false;
    cfg.timing.ringBytes = 256u << 20;
    CaptureSession s;
    std::string err;
    CHECK(s.open(std::move(video), std::move(audio), cfg, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::atomic<bool> done{false};
    CaptureSummary sum;
    s.onAutoStop = [&](const CaptureSummary& x) {
        sum = x;
        done = true;
    };
    CHECK(s.startCapture("New Tape", err));
    const int64_t t0 = monotonicNs();
    while (!done && monotonicNs() - t0 < 30000000000LL) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    s.close();
    CHECK(done.load());
    CHECK_MSG(sum.folder == tmpDir() + "/New Tape", "folder %s", sum.folder.c_str());
    CHECK(!sum.analysis.sheet.pages.empty());
    CHECK(!sum.sha256.empty());
    CHECK_EQ(sum.analysis.boundaries.size(), size_t(1));
    {
        auto* w = new ReportWindow();
        QString e2;
        CHECK_MSG(w->load(QString::fromStdString(sum.jsonPath()), e2), "%s", e2.toLocal8Bit().constData());
        w->show();
        app.processEvents();
        const CaptureRecord& r = w->record();
        CHECK(r.sheet.thumbs.size() >= 5);
        CHECK(fs::exists(r.sheet.pages[0]));
        CHECK(r.segments.size() == 1);
        // The 6 s capture is too short for a second chapter: the boundary merges back.
        CHECK_EQ(r.chapters.size(), size_t(1));
        CHECK(r.verdict().rfind("Review", 0) == 0);  // the signal loss is a spot
        w->close();
        app.processEvents();
    }
}

TEST_MAIN()
