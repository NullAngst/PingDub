// SPDX-License-Identifier: GPL-3.0-or-later
#include "CaptureSession.h"

#include "Checksums.h"
#include "Clock.h"
#include "PixelConvert.h"
#include "SysInfo.h"
#include "backends/V4l2Logic.h"
#include "platform/RtKit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <linux/videodev2.h>

extern "C" {
#include <libavformat/avformat.h>
}

namespace pd {

CaptureSession::CaptureSession() : m_pool(BufferPool::create(256)) {}

CaptureSession::~CaptureSession() { close(); }

void CaptureSession::warn(const std::string& w) {
    std::lock_guard<std::mutex> lk(m_warnMutex);
    for (const auto& x : m_warnings)
        if (x == w) return;
    m_warnings.push_back(w);
    if (m_log) m_log->log("warning", -1, 0, w);
}

std::vector<std::string> CaptureSession::warnings() {
    std::lock_guard<std::mutex> lk(m_warnMutex);
    return m_warnings;
}

std::string CaptureSession::sanitizeLabel(const std::string& label) {
    std::string s;
    for (char c : label) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') s += c;
        else if (c == ' ' && !s.empty() && s.back() != '_') s += '_';
    }
    while (!s.empty() && (s.back() == '_' || s.back() == '.')) s.pop_back();
    if (s.size() > 80) s.resize(80);
    return s;
}

double CaptureSession::estimatedBytesPerSecond(const VideoFormat& v, const AudioFormat& a, bool haveAudio,
                                               VideoCodec codec) {
    double raw = double(v.frameBytes ? v.frameBytes : size_t(v.width) * v.height * 2) * v.fps.toDouble();
    if (pixFmtIs420(v.pixfmt)) raw = double(v.width) * v.height * 1.5 * v.fps.toDouble();
    double factor = 1.0;
    switch (codec) {
    case VideoCodec::FFV1: factor = 0.55; break;     // noisy VHS compresses worse than this
    case VideoCodec::UTVideo: factor = 0.70; break;
    case VideoCodec::Huff: factor = 0.65; break;
    case VideoCodec::Raw: factor = 1.0; break;
    }
    if (v.pixfmt == PixFmt::MJPEG) factor = 0.15;
    const double audio = haveAudio ? double(a.rate) * a.channels * (a.fmt == SampleFmt::S16 ? 2 : (a.fmt == SampleFmt::S24 ? 3 : 4)) : 0;
    return raw * factor + audio;
}

bool CaptureSession::open(std::unique_ptr<VideoBackend> video, std::unique_ptr<AudioBackend> audio,
                          const SessionConfig& cfg, std::string& err) {
    close();
    m_cfg = cfg;
    m_video = std::move(video);
    {
        std::lock_guard<std::mutex> lk(m_warnMutex);
        m_warnings.clear();
    }
    if (cfg.input >= 0 && !m_video->setInput(cfg.input, err)) {
        m_video.reset();
        return false;
    }
    if (cfg.standard && !m_video->setStandard(cfg.standard, err)) {
        m_video.reset();
        return false;
    }
    PixFmt pf = cfg.pixfmt;
    const auto offered = m_video->formats();
    std::vector<PixFmt> list;
    for (const auto& f : offered) list.push_back(f.pixfmt);
    if (pf == PixFmt::Unknown || std::find(list.begin(), list.end(), pf) == list.end()) pf = choosePixFmt(list);
    if (pf == PixFmt::Unknown) {
        err = "the device offers no pixel format PingDub can store losslessly";
        m_video.reset();
        return false;
    }
    if (!m_video->configure(pf, m_vfmt, err)) {
        m_video.reset();
        return false;
    }
    if (cfg.fieldOverride != FieldOrder::Unknown) m_vfmt.field = cfg.fieldOverride;
    if (cfg.sarOverride.valid()) m_vfmt.sar = cfg.sarOverride;
    if (pf == PixFmt::MJPEG)
        warn("Only MJPEG is offered: the device compresses inside, so this capture is not archival.");
    for (const auto& [id, value] : cfg.controls) {
        std::string e;
        m_video->setControl(id, value, e);
    }
    const int speed = m_video->usbSpeedMbps();
    if (speed && speed < 480)
        warn("Device is on a USB 1.1 link (" + std::to_string(speed) + " Mbit/s). Uncompressed SD video does not fit; move it to a USB 2.0 or faster port.");

    // Audio: the given device, or audio embedded in the video stream.
    m_audioOwned = std::move(audio);
    m_audioPtr = nullptr;
    if (cfg.audioEnabled) {
        AudioBackend* a = m_audioOwned ? m_audioOwned.get() : m_video->embeddedAudio();
        if (a) {
            std::string aerr;
            if (a->open(cfg.audioRate, m_afmt, aerr)) {
                m_audioPtr = a;
            } else {
                warn("Audio not available: " + aerr);
            }
        }
    }

    if (!m_video->start(cfg.timing.v4l2Buffers, err)) {
        if (m_audioPtr) m_audioPtr->close();
        m_video.reset();
        m_audioOwned.reset();
        m_audioPtr = nullptr;
        return false;
    }
    if (m_audioPtr) {
        std::string aerr;
        if (!m_audioPtr->start(aerr)) {
            warn("Audio would not start: " + aerr);
            m_audioPtr->close();
            m_audioPtr = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_levelMutex);
        m_levels = AudioLevels{};
        m_levels.channels = m_audioPtr ? m_afmt.channels : 0;
    }
    m_vring.setCapacity(cfg.timing.ringBytes);
    m_aring.setCapacity(64u << 20);
    m_deviceFailed = false;
    m_run = true;
    m_vthread = std::thread([this] { videoLoop(); });
    if (m_audioPtr) m_athread = std::thread([this] { audioLoop(); });
    m_mthread = std::thread([this] { monitorLoop(); });
    m_analysis.start(m_vfmt, cfg.analysis, [this](VideoFrame& f, uint64_t& serial) { return latestFrame(f, serial); });
    return true;
}

void CaptureSession::close() {
    if (m_capturing) stopCapture("session closed");
    m_analysis.stop();
    m_run = false;
    if (m_vthread.joinable()) m_vthread.join();
    if (m_athread.joinable()) m_athread.join();
    if (m_mthread.joinable()) m_mthread.join();
    if (m_video) m_video->stop();
    if (m_audioPtr) {
        m_audioPtr->stop();
        m_audioPtr->close();
    }
    m_audioPtr = nullptr;
    m_audioOwned.reset();
    m_video.reset();
    std::lock_guard<std::mutex> lk(m_previewMutex);
    m_preview = VideoFrame{};
}

bool CaptureSession::setRuntimeSettings(const OutputSettings& out, const TimingSettings& timing,
                                        const StopConditions& stop, bool captureVbi) {
    std::lock_guard<std::mutex> lk(m_captureMutex);
    if (m_capturing) return false;
    m_cfg.output = out;
    m_cfg.timing.sync = timing.sync;
    m_cfg.timing.gapFill = timing.gapFill;
    m_cfg.timing.ringBytes = timing.ringBytes;
    m_cfg.stop = stop;
    m_cfg.captureVbi = captureVbi;
    m_vring.setCapacity(timing.ringBytes);
    return true;
}

bool CaptureSession::setControl(uint32_t id, int64_t value, std::string& err) {
    if (!m_video) return false;
    m_cfg.controls[id] = value;
    return m_video->setControl(id, value, err);
}

// ---------------------------------------------------------------------------
// device threads

void CaptureSession::videoLoop() {
    if (m_cfg.realtime) {
        std::string e;
        if (!makeCurrentThreadRealtime(10, e) && !m_rtWarned.exchange(true))
            warn("Realtime priority not granted (" + e + "); capture continues at normal priority.");
    }
    const double interval = 1e9 * m_vfmt.fps.den / m_vfmt.fps.num;
    const int timeoutMs = std::max(10, int(interval / 1e6));
    LumaLayout layout = LumaLayout::YUYV;
    switch (m_vfmt.pixfmt) {
    case PixFmt::UYVY: layout = LumaLayout::UYVY; break;
    case PixFmt::Y210: layout = LumaLayout::Y210; break;
    case PixFmt::V210: layout = LumaLayout::V210; break;
    case PixFmt::NV12:
    case PixFmt::GREY: layout = LumaLayout::Planar8; break;
    default: break;
    }
    const bool lumaOk = m_vfmt.pixfmt != PixFmt::HM12 && m_vfmt.pixfmt != PixFmt::MJPEG;
    uint64_t n = 0;
    while (m_run) {
        VideoFrame f;
        std::string err;
        const int r = m_video->dequeue(f, *m_pool, timeoutMs, err);
        const int64_t now = monotonicNs();
        if (r < 0) {
            m_deviceError = err.empty() ? "video device error" : err;
            m_deviceFailed = true;
            break;
        }
        if (r == 0) {
            if (m_capturing) {
                for (int64_t ts : m_watchdog.poll(now)) {
                    VideoFrame fill;
                    fill.flags = VF_Fill;
                    fill.tsNs = ts;
                    if (m_vring.push(fill, 64)) m_lastVideoArrival = now;
                }
            }
            continue;
        }
        m_lastVideoArrival = now;
        m_lastRealArrival = now;
        if (!m_lockedNow.load()) f.flags |= VF_NoLock;
        if (lumaOk && (n++ % 3) == 0 && f.data && f.size >= size_t(m_vfmt.bytesPerLine) * m_vfmt.height) {
            const LumaStats st = lumaStats(f.data->data(), m_vfmt.bytesPerLine, m_vfmt.width, m_vfmt.height, layout, 16);
            m_lumaStddev = std::sqrt(std::max(0.0, st.variance));
        }
        {
            std::lock_guard<std::mutex> lk(m_previewMutex);
            m_preview = f;
            ++m_previewSerial;
        }
        if (m_capturing) {
            f.ringDropsBefore = m_pendingRingDrops.load();
            if (m_vring.push(f, f.size + 64)) {
                m_pendingRingDrops = 0;
            } else {
                ++m_pendingRingDrops;  // counted by the sync engine on the next frame that fits
            }
            m_watchdog.onEvent(f.tsNs, now);
        }
    }
}

void CaptureSession::audioLoop() {
    if (m_cfg.realtime) {
        std::string e;
        if (!makeCurrentThreadRealtime(10, e) && !m_rtWarned.exchange(true))
            warn("Realtime priority not granted (" + e + "); capture continues at normal priority.");
    }
    const int ch = m_afmt.channels;
    float peakHold[8] = {};
    while (m_run) {
        AudioChunk c;
        bool xrun = false;
        std::string err;
        const int r = m_audioPtr->read(c, 50, xrun, err);
        if (r < 0) {
            warn("Audio stopped: " + err);
            break;
        }
        if (r == 0) continue;
        const int64_t now = monotonicNs();
        m_lastAudioArrival = now;
        // Meters: peak and RMS per channel in dBFS.
        float peak[8] = {}, sum[8] = {};
        const int32_t* s = c.samples->data();
        for (uint32_t i = 0; i < c.frames; ++i)
            for (int k = 0; k < std::min(ch, 8); ++k) {
                const float v = float(s[i * uint32_t(ch) + uint32_t(k)]) / 2147483648.0f;
                const float a = std::fabs(v);
                if (a > peak[k]) peak[k] = a;
                sum[k] += v * v;
            }
        {
            std::lock_guard<std::mutex> lk(m_levelMutex);
            m_levels.channels = std::min(ch, 8);
            ++m_levels.chunks;
            for (int k = 0; k < m_levels.channels; ++k) {
                peakHold[k] = std::max(peak[k], peakHold[k] * 0.9f);
                m_levels.peakDb[k] = peakHold[k] > 1e-6f ? 20.0f * std::log10(peakHold[k]) : -120.0f;
                const float rms = std::sqrt(sum[k] / float(std::max<uint32_t>(1, c.frames)));
                m_levels.rmsDb[k] = rms > 1e-6f ? 20.0f * std::log10(rms) : -120.0f;
                if (peak[k] >= 0.9999f) m_levels.clipped[k] = true;
            }
        }
        if (!m_capturing) continue;
        const uint32_t silence = m_gap.check(c.tsNs, c.frames, xrun);
        if (silence || xrun) {
            AudioChunk z;
            const uint32_t nfill = std::max<uint32_t>(silence, 0);
            z.samples = std::make_shared<std::vector<int32_t>>(size_t(nfill) * size_t(ch), 0);
            z.frames = nfill;
            z.tsNs = c.tsNs - int64_t(double(nfill) * 1e9 / m_afmt.rate);
            z.flags = AF_XrunFill;
            if (nfill) m_aring.push(z, z.samples->size() * 4 + 64);
            else if (m_log) m_log->log("audio_xrun", m_sync ? m_sync->counters().framesOut : -1, 0, "overrun with no measurable gap");
        }
        m_aring.push(c, c.samples->size() * 4 + 64);
    }
}

void CaptureSession::syncLoop() {
    const bool haveAudio = m_audioPtr != nullptr;
    const int64_t stallNs = 300000000;  // a stream silent this long no longer holds the other back
    while (true) {
        const bool stopping = m_stopSync.load();
        VideoFrame v;
        AudioChunk a;
        const bool hv = m_vring.peek(v);
        const bool ha = haveAudio && m_aring.peek(a);
        const int64_t now = monotonicNs();
        if (hv && ha) {
            if (v.tsNs <= a.tsNs) {
                m_vring.tryPop(v);
                m_sync->pushVideo(v);
            } else {
                m_aring.tryPop(a);
                m_sync->pushAudio(a);
            }
            continue;
        }
        if (hv && (!haveAudio || stopping || now - m_lastAudioArrival.load() > stallNs)) {
            m_vring.tryPop(v);
            m_sync->pushVideo(v);
            continue;
        }
        if (ha && (stopping || now - m_lastVideoArrival.load() > stallNs)) {
            m_aring.tryPop(a);
            m_sync->pushAudio(a);
            continue;
        }
        if (stopping && !hv && !ha) break;
        if (!hv) m_vring.waitNonEmpty(std::chrono::milliseconds(5));
        else m_aring.waitNonEmpty(std::chrono::milliseconds(5));
    }
}

// ---------------------------------------------------------------------------
// capture

std::vector<PreflightItem> CaptureSession::preflight(double plannedSeconds, const std::string& label) {
    std::vector<PreflightItem> out;
    const std::string tmpl = folderTemplate();
    std::string folder, terr;
    TemplateValues tv = templateValues(label);
    tv.startRealNs = realtimeNs();
    if (!resolveFolderTemplate(tmpl, tv, folder, terr)) {
        out.push_back({PreflightItem::Bad, "Output folder: " + terr + "."});
        folder = templateBase(tmpl);
    } else if (templateUsesToken(tmpl, "title") && cleanTemplateValue(label).empty()) {
        out.push_back({PreflightItem::Warn, "The tape label is empty, so {title} in the output folder becomes \"Untitled\"."});
    }
    // The folder is created when the capture starts; until then its nearest existing parent holds the space.
    const double rate = estimatedBytesPerSecond(m_vfmt, m_afmt, m_audioPtr != nullptr, m_cfg.output.vcodec);
    uint64_t freeB = freeBytes(nearestExistingDir(folder));
    if (!m_cfg.output.overflowFolder.empty()) freeB += freeBytes(nearestExistingDir(templateBase(m_cfg.output.overflowFolder)));
    char buf[256];
    if (plannedSeconds > 0) {
        const double need = rate * plannedSeconds;
        std::snprintf(buf, sizeof buf, "Estimated %.1f MB/s, %.0f GB for %.0f minutes; %.0f GB free.", rate / 1e6, need / 1e9,
                      plannedSeconds / 60.0, double(freeB) / 1e9);
        out.push_back({need > double(freeB) ? PreflightItem::Bad : PreflightItem::Ok, buf});
    } else {
        std::snprintf(buf, sizeof buf, "Estimated %.1f MB/s; %.0f GB free is about %.1f hours.", rate / 1e6,
                      double(freeB) / 1e9, rate > 0 ? double(freeB) / rate / 3600.0 : 0.0);
        out.push_back({double(freeB) < rate * 1800 ? PreflightItem::Warn : PreflightItem::Ok, buf});
    }
    const int64_t now = monotonicNs();
    const bool frames = now - m_lastRealArrival.load() < 1000000000LL;
    SignalStatus sig;
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        sig = m_signal;
    }
    if (!frames) out.push_back({PreflightItem::Bad, "No video frames in the last second."});
    else if (sig.known && sig.noSignal) out.push_back({PreflightItem::Warn, "The driver reports no signal on this input."});
    else out.push_back({PreflightItem::Ok, "Video signal present."});
    if (!sig.detected.empty() && sig.detected != m_vfmt.standardName &&
        sig.detected.find(m_vfmt.standardName) == std::string::npos && m_vfmt.standardName.find(sig.detected) == std::string::npos)
        out.push_back({PreflightItem::Warn, "Selected standard " + m_vfmt.standardName + " but the driver detects " + sig.detected + "."});
    if (m_audioPtr) {
        if (now - m_lastAudioArrival.load() > 1000000000LL)
            out.push_back({PreflightItem::Bad, "The audio device is not delivering samples."});
        else
            out.push_back({PreflightItem::Ok, "Audio delivering samples."});
    } else if (m_cfg.audioEnabled) {
        out.push_back({PreflightItem::Warn, "No audio device: the capture will have video only."});
    }
    std::string why;
    if (!videoCodecSupports(m_cfg.output.vcodec, m_cfg.output.container, m_vfmt.pixfmt, why))
        out.push_back({PreflightItem::Bad, why});
    if (m_cfg.output.container == Container::AVI) {
        out.push_back({PreflightItem::Warn, "AVI cannot carry field order reliably; it goes in the sidecar instead."});
        if (m_cfg.output.acodec == AudioCodec::FLAC) out.push_back({PreflightItem::Warn, "FLAC does not go in AVI; PCM is used."});
    }
    for (const auto& w : warnings()) out.push_back({PreflightItem::Warn, w});
    return out;
}

bool CaptureSession::startCapture(const std::string& label, std::string& err) {
    std::lock_guard<std::mutex> lk(m_captureMutex);
    if (!m_video || m_capturing) {
        err = m_capturing ? "already capturing" : "no device open";
        return false;
    }
    m_label = label;
    m_captureStartReal = realtimeNs();
    m_captureStartMono = monotonicNs();
    // The folder template is resolved once, here, so a capture never splits across folders.
    TemplateValues tv = templateValues(label);
    tv.startRealNs = m_captureStartReal;
    std::string folder, terr;
    if (!resolveFolderTemplate(folderTemplate(), tv, folder, terr)) {
        err = "output folder: " + terr;
        return false;
    }
    if (!makeDirs(folder)) {
        err = "cannot create " + folder;
        return false;
    }
    {
        std::lock_guard<std::mutex> fl(m_folderMutex);
        m_folder = folder;
    }
    const std::string clean = sanitizeLabel(label);
    const std::string stem = fileStamp(m_captureStartReal) + (clean.empty() ? "" : "_" + clean);
    m_basePath = folder + "/" + stem;

    OutputSettings outSet = m_cfg.output;
    if (outSet.container == Container::AVI && outSet.acodec == AudioCodec::FLAC) outSet.acodec = AudioCodec::PCM;
    if (!outSet.overflowFolder.empty()) {
        std::string of;
        if (!resolveFolderTemplate(outSet.overflowFolder, tv, of, terr)) {
            err = "overflow folder: " + terr;
            return false;
        }
        outSet.overflowFolder = of;
    }

    m_log = std::make_unique<EventLog>();
    if (!m_log->open(m_basePath + ".log.csv", m_vfmt.fps)) {
        err = "cannot write " + m_basePath + ".log.csv";
        m_log.reset();
        return false;
    }

    OutputPlan plan;
    plan.out = outSet;
    plan.vfmt = m_vfmt;
    plan.haveAudio = m_audioPtr != nullptr;
    plan.afmt = m_afmt;
    plan.folder = folder;
    plan.stem = stem;
    const std::string ver = PINGDUB_VERSION;
    plan.tags["TITLE"] = label.empty() ? stem : label;
    plan.tags["ENCODER"] = "PingDub " + ver;
    plan.tags["encoding_tool"] = "PingDub " + ver;  // Matroska WritingApp
    plan.tags["PINGDUB_VERSION"] = ver;
    plan.tags["PINGDUB_DEVICE"] = m_video->deviceName();
    plan.tags["PINGDUB_DRIVER"] = m_video->driverName();
    plan.tags["PINGDUB_BACKEND"] = m_video->backendName();
    {
        int cur = m_video->currentInput();
        for (const auto& in : m_video->inputs())
            if (in.index == cur) plan.tags["PINGDUB_INPUT"] = in.name;
    }
    plan.tags["PINGDUB_STANDARD"] = m_vfmt.standardName;
    plan.tags["PINGDUB_FIELD_ORDER"] = fieldOrderName(m_vfmt.field);
    plan.tags["PINGDUB_SOURCE_PIXEL_FORMAT"] = pixFmtName(m_vfmt.pixfmt);
    char sar[32];
    std::snprintf(sar, sizeof sar, "%d:%d", m_vfmt.sar.num, m_vfmt.sar.den);
    plan.tags["PINGDUB_SAR"] = sar;
    plan.tags["PINGDUB_AUDIO_DEVICE"] = m_audioPtr ? (m_cfg.audioDeviceName.empty() ? m_audioPtr->deviceName() : m_cfg.audioDeviceName) : "none";
    plan.tags["PINGDUB_SYNC_MODE"] = syncModeName(m_cfg.timing.sync);
    plan.tags["PINGDUB_CAPTURE_START"] = isoLocalTime(m_captureStartReal);
    if (!label.empty()) plan.tags["PINGDUB_TAPE_LABEL"] = label;
    plan.tags["DATE_RECORDED"] = isoLocalTime(m_captureStartReal).substr(0, 10);
    plan.deferredTags = {"PINGDUB_CAPTURE_END",   "PINGDUB_FRAMES",       "PINGDUB_INSERTED",     "PINGDUB_DROPPED",
                         "PINGDUB_DRIVER_DROPS",  "PINGDUB_BUFFER_OVERFLOW", "PINGDUB_XRUNS",      "PINGDUB_DRIFT_PPM",
                         "PINGDUB_AV_OFFSET_MS",  "PINGDUB_STOP_REASON",  "PINGDUB_XDS_TITLE",    "PINGDUB_XDS_RATING"};
    plan.frameHashes = m_cfg.checksums;
    ChapterSettings chs;
    chs.intervalMinutes = m_cfg.output.chapterIntervalMin;
    chs.boundaries = m_cfg.output.chapterBoundaries;
    m_chapters.reset(chs, m_vfmt.fps);
    if (chs.enabled()) {
        plan.chapters = [this](int64_t start, int64_t end, bool final) {
            // A boundary right before a split may still be in the analysis queue.
            if (!final) m_analysis.waitProcessed(end, 3000);
            return m_chapters.forSegment(start, end, final);
        };
    }

    m_out = std::make_unique<OutputWriter>(m_log.get());
    if (!m_out->open(plan, err)) {
        m_out.reset();
        m_log->close();
        m_log.reset();
        return false;
    }

    SyncConfig sc;
    sc.mode = m_cfg.timing.sync;
    sc.gapFill = m_cfg.timing.gapFill;
    sc.fps = m_vfmt.fps;
    sc.haveAudio = m_audioPtr != nullptr;
    sc.audioRate = m_afmt.rate;
    sc.channels = m_afmt.channels;
    sc.sampleBits = sampleFmtBits(m_afmt.fmt);
    if (m_cfg.stop.durationSec > 0)
        sc.maxFrames = uint64_t(std::llround(m_cfg.stop.durationSec * m_vfmt.fps.num / double(m_vfmt.fps.den)));
    m_tee.out = m_out.get();
    m_tee.analysis = &m_analysis;
    m_tee.audioPos = 0;
    m_sync = std::make_unique<SyncEngine>(sc, &m_tee);

    m_vbi.reset();
    if (m_cfg.captureVbi) {
        const std::string node = m_video->vbiNode();
        if (!node.empty()) {
            auto vbi = std::make_unique<VbiCapture>();
            std::string verr;
            if (vbi->open(node, m_vfmt.lines525, verr)) {
                SyncEngine* eng = m_sync.get();
                vbi->start([eng](int64_t ts) { return eng->slotForTs(ts); }, m_basePath, m_vfmt.fps);
                m_log->log("vbi", -1, 0, node + ": " + vbi->description());
                m_vbi = std::move(vbi);
            } else {
                warn("VBI node found but not usable: " + verr);
            }
        }
    }

    m_log->log("capture_start", -1, 0,
               m_video->deviceName() + ", " + m_vfmt.standardName + ", " + pixFmtName(m_vfmt.pixfmt) + " " +
                   std::to_string(m_vfmt.width) + "x" + std::to_string(m_vfmt.height) + ", " + m_out->videoDescription() +
                   (m_audioPtr ? ", " + m_out->audioDescription() : ", no audio"));
    for (const auto& w : warnings()) m_log->log("warning", -1, 0, w);

    m_vring.clear();
    m_aring.clear();
    m_pendingRingDrops = 0;
    m_watchdog.reset(1e9 * m_vfmt.fps.den / m_vfmt.fps.num, 3);
    m_gap.reset(m_afmt.rate);
    m_staticSince = 0;
    m_lostSince = 0;
    m_dropHistory.clear();
    m_dataRate = 0;
    m_lastBytes = 0;
    m_lastRateNs = monotonicNs();
    m_stopReason.clear();
    m_error.clear();
    m_stopSync = false;
    {
        CaptureAnalysisPlan ap;
        ap.basePath = m_basePath;
        ap.vfmt = m_vfmt;
        ap.haveAudio = m_audioPtr != nullptr;
        ap.afmt = m_afmt;
        ap.contactSheetSec = m_cfg.output.contactSheetSec;
        ap.log = m_log.get();
        ap.chapters = &m_chapters;
        m_analysis.beginCapture(ap, m_cfg.analysis);
    }
    {
        std::lock_guard<std::mutex> fl(m_finishMutex);
        m_finishStage.clear();
    }
    m_sthread = std::thread([this] { syncLoop(); });
    m_capturing = true;
    return true;
}

std::map<std::string, std::string> CaptureSession::finalTags(const CaptureSummary& s) {
    std::map<std::string, std::string> t;
    char b[64];
    t["PINGDUB_CAPTURE_END"] = isoLocalTime(realtimeNs());
    t["PINGDUB_FRAMES"] = std::to_string(s.sync.framesOut);
    t["PINGDUB_INSERTED"] = std::to_string(s.sync.inserted);
    t["PINGDUB_DROPPED"] = std::to_string(s.sync.dropped);
    t["PINGDUB_DRIVER_DROPS"] = std::to_string(s.sync.driverDrops);
    t["PINGDUB_BUFFER_OVERFLOW"] = std::to_string(s.sync.ringDrops);
    t["PINGDUB_XRUNS"] = std::to_string(s.sync.xruns);
    std::snprintf(b, sizeof b, "%.2f", s.sync.driftPpm);
    t["PINGDUB_DRIFT_PPM"] = b;
    std::snprintf(b, sizeof b, "%.2f", s.sync.avOffsetMs);
    t["PINGDUB_AV_OFFSET_MS"] = b;
    t["PINGDUB_STOP_REASON"] = s.stopReason;
    if (m_vbi) {
        const XdsInfo x = m_vbi->xds();
        t["PINGDUB_XDS_TITLE"] = x.title;
        t["PINGDUB_XDS_RATING"] = x.rating;
    } else {
        t["PINGDUB_XDS_TITLE"] = "";
        t["PINGDUB_XDS_RATING"] = "";
    }
    return t;
}

void CaptureSession::writeJsonSidecar(const CaptureSummary& s) {
    JsonWriter j;
    j.begin();
    j.str("pingdub_version", PINGDUB_VERSION);
    j.str("capture_start", isoLocalTime(m_captureStartReal));
    j.str("capture_end", isoLocalTime(realtimeNs()));
    j.str("stop_reason", s.stopReason);
    if (!s.error.empty()) j.str("error", s.error);
    j.str("tape_label", m_label);
    j.str("profile", m_cfg.profileName);
    j.str("output_folder", s.folder);
    j.strList("files", s.files);
    j.strList("sidecars", s.sidecars);
    j.begin("device");
    j.str("name", m_video->deviceName()).str("driver", m_video->driverName()).str("backend", m_video->backendName());
    j.str("path", m_video->devicePath());
    {
        const int cur = m_video->currentInput();
        for (const auto& in : m_video->inputs())
            if (in.index == cur) j.str("input", in.name);
    }
    j.str("crop", m_video->cropSummary());
    j.end();
    j.begin("video");
    j.str("standard", m_vfmt.standardName).str("pixel_format", pixFmtName(m_vfmt.pixfmt));
    j.integer("width", m_vfmt.width).integer("height", m_vfmt.height);
    j.str("frame_rate", std::to_string(m_vfmt.fps.num) + "/" + std::to_string(m_vfmt.fps.den));
    j.str("field_order", fieldOrderName(m_vfmt.field));
    j.str("sar", std::to_string(m_vfmt.sar.num) + ":" + std::to_string(m_vfmt.sar.den));
    j.str("color", m_vfmt.lines525 ? "smpte170m, tv range" : "bt470bg, tv range");
    j.str("codec", m_out ? m_out->videoDescription() : "");
    j.str("container", containerName(m_cfg.output.container));
    j.end();
    j.begin("audio");
    j.boolean("present", m_audioPtr != nullptr);
    if (m_audioPtr) {
        j.str("device", m_cfg.audioDeviceName.empty() ? m_audioPtr->deviceName() : m_cfg.audioDeviceName);
        j.integer("rate", m_afmt.rate).integer("channels", m_afmt.channels).str("format", sampleFmtName(m_afmt.fmt));
        j.str("codec", m_out ? m_out->audioDescription() : "");
    }
    j.end();
    j.begin("settings");
    j.str("sync_mode", syncModeName(m_cfg.timing.sync));
    j.str("gap_fill", m_cfg.timing.gapFill == GapFill::Black ? "black" : "last-frame");
    j.integer("v4l2_buffers", m_cfg.timing.v4l2Buffers);
    j.integer("ring_bytes", (long long)m_cfg.timing.ringBytes);
    j.num("segment_seconds", m_cfg.output.segmentSeconds);
    j.integer("segment_bytes", (long long)m_cfg.output.segmentBytes);
    j.str("overflow_folder", m_cfg.output.overflowFolder);
    j.str("folder_template", folderTemplate());
    j.integer("chapter_interval_min", m_cfg.output.chapterIntervalMin);
    j.boolean("chapter_boundaries", m_cfg.output.chapterBoundaries);
    j.num("contact_sheet_sec", m_cfg.output.contactSheetSec);
    j.num("level_threshold_pct", m_cfg.analysis.watch.thresholdPct);
    j.num("level_min_seconds", m_cfg.analysis.watch.minSeconds);
    j.integer("chroma_limit", m_cfg.analysis.chromaLimit);
    j.begin("controls");
    if (m_video)
        for (const auto& c : m_video->controls()) j.integer(c.name, c.value);
    j.end();
    j.end();
    j.begin("totals");
    j.num("seconds", s.seconds);
    j.integer("frames", (long long)s.sync.framesOut);
    j.integer("frames_from_device", (long long)s.sync.realFrames);
    j.integer("inserted", (long long)s.sync.inserted);
    j.integer("dropped", (long long)s.sync.dropped);
    j.integer("driver_drops", (long long)s.sync.driverDrops);
    j.num("video_timestamp_jitter_ms", s.sync.tsJitterMs);
    j.integer("buffer_overflow", (long long)s.sync.ringDrops);
    j.integer("error_frames", (long long)s.sync.errorFrames);
    j.integer("audio_frames_in", (long long)s.sync.audioIn);
    j.integer("audio_frames_out", (long long)s.sync.audioOut);
    j.integer("xruns", (long long)s.sync.xruns);
    j.integer("xrun_silence_frames", (long long)s.sync.xrunSilence);
    j.integer("audio_resyncs", (long long)s.sync.audioResyncs);
    j.num("drift_ppm", s.sync.driftPpm);
    j.num("av_offset_ms", s.sync.avOffsetMs);
    j.boolean("resample_clamp_hit", s.sync.clampHit);
    j.integer("bytes_written", m_out ? (long long)m_out->bytesWritten() : 0);
    const CaptureAnalysisResult& an = s.analysis;
    j.integer("level_warnings", (long long)(an.levelEpisodes[0] + an.levelEpisodes[1] + an.levelEpisodes[2]));
    j.integer("audio_faults", (long long)(an.audioEvents[0] + an.audioEvents[1] + an.audioEvents[2] + an.audioEvents[3]));
    j.integer("chapters", (long long)s.chapters.size());
    j.end();
    j.begin("level_events");
    j.integer("crush", (long long)an.levelEpisodes[int(LevelKind::Crush)]);
    j.integer("clip", (long long)an.levelEpisodes[int(LevelKind::Clip)]);
    j.integer("chroma", (long long)an.levelEpisodes[int(LevelKind::Chroma)]);
    j.end();
    j.begin("audio_events");
    j.integer("dropouts", (long long)an.audioEvents[int(AudioFault::Dropout)]);
    j.integer("bursts", (long long)an.audioEvents[int(AudioFault::Burst)]);
    j.integer("dead_channel", (long long)an.audioEvents[int(AudioFault::DeadChannel)]);
    j.integer("phase", (long long)an.audioEvents[int(AudioFault::Phase)]);
    j.end();
    j.begin("analysis");
    j.integer("frames_analyzed", (long long)an.framesAnalyzed);
    j.integer("frames_not_analyzed", (long long)an.framesNotAnalyzed);
    j.integer("audio_frames_not_checked", (long long)an.audioSkipped);
    j.end();
    j.beginArray("segments");
    for (const SegmentRecord& seg : s.segments) {
        j.begin().str("file", seg.path).integer("start_frame", seg.startSlot).integer("frames", seg.frames);
        if (!seg.frameMd5.empty()) j.str("framemd5", seg.frameMd5);
        if (!seg.chaptersTxt.empty()) j.str("chapters_txt", seg.chaptersTxt);
        auto it = s.sha256.find(seg.path);
        if (it != s.sha256.end()) j.str("sha256", it->second);
        j.end();
    }
    j.endArray();
    j.beginArray("chapters");
    for (const Chapter& ch : s.chapters) j.begin().integer("start", ch.start).integer("end", ch.end).str("title", ch.title).end();
    j.endArray();
    const ContactSheetInfo& cs = an.sheet;
    if (!cs.empty()) {
        j.begin("contact_sheet");
        j.num("interval_sec", cs.intervalSec).integer("tile_width", cs.tileWidth).integer("tile_height", cs.tileHeight);
        j.integer("columns", cs.columns).integer("rows", cs.rows).integer("gap", cs.gap).integer("label_height", cs.labelHeight);
        j.strList("pages", cs.pages);
        j.beginArray("thumbs");
        for (const ContactThumb& t : cs.thumbs) j.begin().integer("frame", t.slot).integer("page", t.page).integer("index", t.index).end();
        j.endArray();
        j.end();
    }
    if (m_vbi) {
        const XdsInfo x = m_vbi->xds();
        j.begin("xds").str("title", x.title).str("rating", x.rating).end();
    }
    j.strList("warnings", warnings());
    j.end();
    writeTextFile(m_basePath + ".json", j.text());
}

CaptureSummary CaptureSession::stopCapture(const std::string& reason) {
    std::lock_guard<std::mutex> lk(m_captureMutex);
    CaptureSummary s;
    if (!m_capturing || !m_sync) return s;
    m_capturing = false;  // device threads stop feeding the rings
    m_stopSync = true;
    if (m_sthread.joinable()) m_sthread.join();
    m_sync->finish();
    s.sync = m_sync->counters();
    s.seconds = double(s.sync.framesOut) * m_vfmt.fps.den / m_vfmt.fps.num;
    s.stopReason = m_stopReason.empty() ? reason : m_stopReason;
    {
        std::lock_guard<std::mutex> fl(m_folderMutex);
        s.folder = m_folder;
    }
    s.basePath = m_basePath;
    setFinishing("analysis");
    // Drains the analysis queue: level and audio events, the last contact sheet
    // page and every recording boundary are known after this.
    s.analysis = m_analysis.endCapture();
    if (!s.analysis.sheetError.empty() && m_cfg.output.contactSheetSec > 0) warn("Contact sheet: " + s.analysis.sheetError);
    if (m_vbi) {
        for (const auto& f : m_vbi->stop(int64_t(s.sync.framesOut))) s.sidecars.push_back(f);
    }
    if (m_out->failed()) s.error = m_out->error();
    m_log->log("capture_stop", int64_t(s.sync.framesOut), s.sync.framesOut, s.stopReason);
    setFinishing("finalizing the file");
    m_out->close(finalTags(s));
    if (s.error.empty() && m_out->failed()) s.error = m_out->error();
    s.files = m_out->files();
    s.segments = m_out->segments();
    s.chapters = m_chapters.all(int64_t(s.sync.framesOut));
    s.sidecars.insert(s.sidecars.begin(), {m_basePath + ".log.csv", m_basePath + ".json"});
    for (SegmentRecord& seg : s.segments) {
        if (!seg.frameMd5.empty()) s.sidecars.push_back(seg.frameMd5);
        if (!seg.chaptersTxt.empty()) s.sidecars.push_back(seg.chaptersTxt);
        if (seg.chapters.empty() || m_cfg.output.container != Container::MKV) continue;
        // Check the chapters made it into the file; if this FFmpeg dropped
        // them, the same list goes in a sidecar instead.
        unsigned got = 0;
        AVFormatContext* fc = nullptr;
        if (avformat_open_input(&fc, seg.path.c_str(), nullptr, nullptr) == 0) {
            got = fc->nb_chapters;
            avformat_close_input(&fc);
        }
        if (got != seg.chapters.size()) {
            seg.chaptersTxt = sidecarFor(seg.path, ".chapters.txt");
            writeTextFile(seg.chaptersTxt, ffmetadataChapters(seg.chapters, m_vfmt.fps));
            s.sidecars.push_back(seg.chaptersTxt);
            warn("Matroska chapters did not stick in " + seg.path + "; they are in " + seg.chaptersTxt + " instead.");
        }
    }
    for (const std::string& p : s.analysis.sheet.pages) s.sidecars.push_back(p);
    if (m_cfg.checksums) {
        uint64_t total = 0, done = 0;
        for (const std::string& f : s.files) {
            std::error_code ec;
            total += uint64_t(std::filesystem::file_size(f, ec));
        }
        for (const std::string& f : s.files) {
            std::string hex, herr;
            uint64_t before = done;
            const bool ok = sha256File(f, hex, herr, [&](uint64_t d, uint64_t) {
                done = before + d;
                char b[64];
                std::snprintf(b, sizeof b, "checksum %.0f%%", total ? 100.0 * double(done) / double(total) : 0.0);
                setFinishing(b);
                return true;
            });
            if (!ok) {
                warn("SHA-256 of " + f + " failed: " + herr);
                continue;
            }
            s.sha256[f] = hex;
            const std::string side = writeSha256Sidecar(f, hex);
            if (side.empty()) warn("cannot write the .sha256 sidecar for " + f);
            else s.sidecars.push_back(side);
        }
    }
    s.ok = s.error.empty();
    setFinishing("sidecars");
    writeJsonSidecar(s);
    m_log->close();
    setFinishing("");
    m_vbi.reset();
    m_sync.reset();
    m_out.reset();
    m_log.reset();
    m_vring.clear();
    m_aring.clear();
    return s;
}

// ---------------------------------------------------------------------------
// monitor

void CaptureSession::monitorLoop() {
    int64_t lastSignalPoll = 0;
    while (m_run) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int64_t now = monotonicNs();
        if (now - lastSignalPoll >= 240000000LL) {
            lastSignalPoll = now;
            SignalStatus st = m_video->signalStatus();
            m_lockedNow = !(st.known && (st.noSignal || st.noHLock));
            std::lock_guard<std::mutex> lk(m_statsMutex);
            if (m_capturing && m_log && st.noSignal != m_signal.noSignal)
                m_log->log(st.noSignal ? "signal_lost" : "signal_restored", m_sync ? int64_t(m_sync->counters().framesOut) : -1, 0, "");
            m_signal = st;
        }
        if (m_deviceFailed) {
            if (m_capturing) {
                m_stopReason = "device lost: " + m_deviceError;
                CaptureSummary s = stopCapture(m_stopReason);
                if (onAutoStop) onAutoStop(s);
            } else if (onDeviceError) {
                onDeviceError(m_deviceError);
            }
            m_run = false;  // device threads are done; close() joins them
            break;
        }
        if (!m_capturing || !m_sync || !m_out) continue;

        const SyncCounters c = m_sync->counters();
        const StopConditions& sc = m_cfg.stop;
        std::string reason;
        const double secs = double(c.framesOut) * m_vfmt.fps.den / m_vfmt.fps.num;
        if (m_out->failed()) reason = "write error: " + m_out->error();
        if (reason.empty() && sc.durationSec > 0 && (m_sync->full() || secs >= sc.durationSec)) reason = "duration reached";
        if (reason.empty() && sc.fileBytes > 0 && m_out->bytesWritten() >= sc.fileBytes) reason = "file size reached";
        if (reason.empty() && sc.minFreeBytes > 0) {
            const uint64_t freeB = freeBytes(m_out->currentFolder());
            const bool canSpill = !m_cfg.output.overflowFolder.empty() && !m_out->usingOverflow();
            if (freeB && freeB < sc.minFreeBytes && !canSpill) reason = "disk almost full";
        }
        if (reason.empty() && sc.maxDropPerMin > 0) {
            const uint64_t bad = c.inserted + c.dropped + c.ringDrops;
            m_dropHistory.push_back({now, bad});
            while (!m_dropHistory.empty() && now - m_dropHistory.front().first > 60000000000LL) m_dropHistory.pop_front();
            if (now - m_captureStartMono > 60000000000LL && m_dropHistory.size() > 1 &&
                double(bad - m_dropHistory.front().second) > sc.maxDropPerMin)
                reason = "dropped frame rate above threshold";
        }
        bool lost;
        {
            std::lock_guard<std::mutex> lk(m_statsMutex);
            lost = (m_signal.known && m_signal.noSignal) || now - m_lastRealArrival.load() > 1000000000LL;
        }
        if (lost) {
            if (!m_lostSince) m_lostSince = now;
        } else {
            m_lostSince = 0;
        }
        if (reason.empty() && sc.signalLostSec > 0 && m_lostSince && double(now - m_lostSince) / 1e9 >= sc.signalLostSec)
            reason = "signal lost";
        const double sd = m_lumaStddev.load();
        if (sd >= 0 && sd < sc.staticStddev && !lost) {
            if (!m_staticSince) m_staticSince = now;
        } else {
            m_staticSince = 0;
        }
        if (reason.empty() && sc.staticSec > 0 && m_staticSince && double(now - m_staticSince) / 1e9 >= sc.staticSec)
            reason = "static picture (end of tape)";

        if (!reason.empty()) {
            m_stopReason = reason;
            CaptureSummary s = stopCapture(reason);
            if (onAutoStop) onAutoStop(s);
        }
    }
}

SessionStats CaptureSession::stats() {
    SessionStats st;
    st.state = !m_video ? SessionState::Closed : (m_capturing ? SessionState::Capturing : SessionState::Previewing);
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        st.signal = m_signal;
    }
    const int64_t now = monotonicNs();
    st.signalLocked = !(st.signal.known && (st.signal.noSignal || st.signal.noHLock)) && now - m_lastRealArrival.load() < 1000000000LL;
    st.lumaStddev = m_lumaStddev.load();
    st.ringBytes = m_vring.capacity();
    st.ringFill = m_vring.fill();
    st.ringFillPeak = st.ringBytes ? double(m_vring.peakBytes()) / double(st.ringBytes) : 0.0;
    st.stopReason = m_stopReason;
    // While a stop is finalizing files, report that instead of waiting on it.
    std::unique_lock<std::mutex> lk(m_captureMutex, std::try_to_lock);
    if (!lk.owns_lock()) {
        st.state = SessionState::Stopping;
        std::lock_guard<std::mutex> fl(m_finishMutex);
        st.finishing = m_finishStage;
        return st;
    }
    if (m_capturing && m_sync && m_out) {
        st.sync = m_sync->counters();
        st.elapsedSec = double(st.sync.framesOut) * m_vfmt.fps.den / m_vfmt.fps.num;
        st.wallSec = double(now - m_captureStartMono) / 1e9;
        st.bytesWritten = m_out->bytesWritten();
        st.currentFile = m_out->currentFile();
        st.segments = m_out->segmentCount();
        st.diskFree = freeBytes(m_out->currentFolder());
        if (now - m_lastRateNs > 1000000000LL) {
            const double inst = double(st.bytesWritten - m_lastBytes) / (double(now - m_lastRateNs) / 1e9);
            m_dataRate = m_dataRate <= 0 ? inst : m_dataRate * 0.8 + inst * 0.2;
            m_lastBytes = st.bytesWritten;
            m_lastRateNs = now;
        }
        st.dataRate = m_dataRate;
        uint64_t freeAll = st.diskFree;
        if (!m_cfg.output.overflowFolder.empty() && !m_out->usingOverflow())
            freeAll += freeBytes(expandHome(m_cfg.output.overflowFolder));
        st.timeLeftSec = m_dataRate > 0 ? double(freeAll) / m_dataRate : -1;
        st.levels = m_analysis.liveLevels();
        st.levelEpisodes = m_analysis.levelEpisodeCount();
        st.audioEvents = m_analysis.audioEventCount();
        {
            std::lock_guard<std::mutex> fl(m_folderMutex);
            st.captureFolder = m_folder;
        }
        if (m_vbi) {
            st.vbiActive = true;
            st.vbiDescription = m_vbi->description();
            st.ccLive = m_vbi->liveText();
        }
        if (m_out->failed()) st.error = m_out->error();
    } else {
        st.diskFree = freeBytes(nearestExistingDir(templateBase(folderTemplate())));
        const double rate = m_video ? estimatedBytesPerSecond(m_vfmt, m_afmt, m_audioPtr != nullptr, m_cfg.output.vcodec) : 0;
        st.timeLeftSec = rate > 0 ? double(st.diskFree) / rate : -1;
    }
    return st;
}

AudioLevels CaptureSession::levels() {
    std::lock_guard<std::mutex> lk(m_levelMutex);
    AudioLevels l = m_levels;
    return l;
}

void CaptureSession::resetClip() {
    std::lock_guard<std::mutex> lk(m_levelMutex);
    for (bool& c : m_levels.clipped) c = false;
}

void CaptureSession::setFinishing(const std::string& what) {
    std::lock_guard<std::mutex> lk(m_finishMutex);
    m_finishStage = what;
}

std::string CaptureSession::folderTemplate() const {
    return m_cfg.output.folder.empty() ? "~/Videos/PingDub" : m_cfg.output.folder;
}

TemplateValues CaptureSession::templateValues(const std::string& label) const {
    TemplateValues v;
    v.title = label;
    v.startRealNs = realtimeNs();
    v.profile = m_cfg.profileName;
    if (m_video) {
        v.device = m_video->deviceName();
        const int cur = m_video->currentInput();
        for (const auto& in : m_video->inputs())
            if (in.index == cur) v.input = in.name;
    }
    return v;
}

std::string CaptureSession::lastCaptureFolder() const {
    std::lock_guard<std::mutex> lk(m_folderMutex);
    return m_folder;
}

void CaptureSession::setAnalysisSettings(const AnalysisSettings& a) {
    std::lock_guard<std::mutex> lk(m_captureMutex);
    if (m_capturing) return;
    m_cfg.analysis = a;
    m_analysis.setSettings(a);
}

bool CaptureSession::startLevelMeasure(std::string& err) {
    if (!m_video) {
        err = "no device open";
        return false;
    }
    if (m_capturing) {
        err = "levels are set in preview only, never during a capture";
        return false;
    }
    if (m_vfmt.pixfmt == PixFmt::MJPEG) {
        err = "MJPEG frames are compressed by the device; Level Measure needs an uncompressed format";
        return false;
    }
    m_analysis.startMeasure();
    return true;
}

void CaptureSession::stopLevelMeasure() { m_analysis.stopMeasure(); }

ProcAmp CaptureSession::procAmp() {
    ProcAmp a;
    if (!m_video) return a;
    for (const VideoControl& c : m_video->controls()) {
        if (c.readOnly || c.inactive || (c.type != VideoControl::Type::Integer && c.type != VideoControl::Type::Integer64)) continue;
        ProcControl* t = nullptr;
        if (c.id == V4L2_CID_BRIGHTNESS) t = &a.brightness;
        else if (c.id == V4L2_CID_CONTRAST) t = &a.contrast;
        else if (c.id == V4L2_CID_SATURATION) t = &a.saturation;
        if (!t) continue;
        t->present = c.maximum > c.minimum;
        t->id = c.id;
        t->name = c.name;
        t->minimum = c.minimum;
        t->maximum = c.maximum;
        t->step = std::max<int64_t>(1, c.step);
        t->defaultValue = c.defaultValue;
        t->value = c.value;
    }
    return a;
}

bool CaptureSession::latestFrame(VideoFrame& out, uint64_t& serial) {
    std::lock_guard<std::mutex> lk(m_previewMutex);
    if (!m_preview.data || m_previewSerial == serial) return false;
    out = m_preview;
    serial = m_previewSerial;
    return true;
}

}  // namespace pd
