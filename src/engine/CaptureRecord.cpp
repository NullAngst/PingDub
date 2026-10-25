// SPDX-License-Identifier: GPL-3.0-or-later
#include "CaptureRecord.h"

#include "Json.h"
#include "SysInfo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace pd {

namespace fs = std::filesystem;

ReportLane laneOf(const std::string& e, std::string& label) {
    struct M {
        const char* name;
        ReportLane lane;
        const char* label;
    };
    static const M map[] = {
        {"insert", ReportLane::Video, "Inserted frames"},
        {"driver_drop", ReportLane::Video, "Driver drop"},
        {"buffer_overflow", ReportLane::Video, "Buffer overflow"},
        {"error_frame", ReportLane::Video, "Damaged frame"},
        {"signal_lost", ReportLane::Video, "Signal lost"},
        {"drop", ReportLane::Video, "Dropped frame (audio master)"},
        {"levels_crush", ReportLane::Levels, "Blacks crushed"},
        {"levels_clip", ReportLane::Levels, "Whites clipped"},
        {"levels_chroma", ReportLane::Levels, "Chroma past the limit"},
        {"audio_dropout", ReportLane::Audio, "Hi-fi dropout"},
        {"audio_dead_channel", ReportLane::Audio, "Dead channel"},
        {"audio_phase", ReportLane::Audio, "Phase problem"},
        {"audio_xrun", ReportLane::Audio, "Audio xrun"},
        {"audio_resync", ReportLane::Audio, "Audio resync"},
        {"resample_clamp", ReportLane::Audio, "Resampler at its limit"},
    };
    for (const M& m : map)
        if (e == m.name) {
            label = m.label;
            return m.lane;
        }
    label = e;
    return ReportLane::Info;
}

std::string captureBaseFor(const std::string& path) {
    std::string p = path;
    for (const char* ext : {".log.csv", ".json", ".mkv", ".avi", ".sha256", ".framemd5", ".chapters.txt"}) {
        const std::string e = ext;
        if (p.size() > e.size() && p.compare(p.size() - e.size(), e.size(), e) == 0) {
            p.resize(p.size() - e.size());
            break;
        }
    }
    // name_part002 -> name
    const size_t u = p.rfind("_part");
    if (u != std::string::npos && p.size() - u == 8) {
        bool digits = true;
        for (size_t i = u + 5; i < p.size(); ++i) digits &= std::isdigit(static_cast<unsigned char>(p[i])) != 0;
        if (digits) p.resize(u);
    }
    return p;
}

bool parseCsvLine(const std::string& line, std::vector<std::string>& f) {
    f.clear();
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            f.push_back(cur);
            cur.clear();
        } else if (c != '\r' && c != '\n') {
            cur += c;
        }
    }
    f.push_back(cur);
    return !quoted;
}

int countSpots(const std::vector<RecordEvent>& ev, const Rational& fps, double gapSec) {
    std::vector<std::pair<int64_t, int64_t>> spans;
    for (const RecordEvent& e : ev)
        if (e.lane != ReportLane::Info && e.slot >= 0) spans.push_back({e.slot, std::max(e.slot, e.endSlot)});
    std::sort(spans.begin(), spans.end());
    const int64_t gap = std::llround(gapSec * (fps.valid() ? fps.toDouble() : 29.97));
    int spots = 0;
    int64_t end = -1;
    for (const auto& s : spans) {
        if (spots == 0 || s.first - end > gap) ++spots;
        end = std::max(end, s.second);
    }
    return spots;
}

std::string CaptureRecord::verdict() const {
    if (spots == 0) return "Clean";
    return "Review " + std::to_string(spots) + (spots == 1 ? " spot" : " spots");
}

std::string CaptureRecord::clock(int64_t slot) const {
    if (slot < 0) return "-";
    const double sec = double(slot) * fps.den / fps.num;
    const int h = int(sec / 3600), m = int(std::fmod(sec, 3600) / 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%06.3f", h, m, std::fmod(sec, 60));
    return buf;
}

bool CaptureRecord::locate(int64_t slot, std::string& file, int64_t& frame) const {
    if (segments.empty()) {
        file = mediaPath;
        frame = slot;
        return !file.empty() && slot >= 0;
    }
    const RecordSegment* best = &segments.front();
    for (const RecordSegment& s : segments)
        if (s.startSlot <= slot) best = &s;
    file = best->file;
    frame = slot - best->startSlot;
    return frame >= 0;
}

static Rational parseRate(const std::string& s) {
    const size_t sl = s.find('/');
    if (sl == std::string::npos) return {0, 1};
    return {std::atoi(s.substr(0, sl).c_str()), std::atoi(s.substr(sl + 1).c_str())};
}

// A path from the JSON, or the file of the same name next to the JSON if the
// capture was moved since.
static std::string resolveNear(const std::string& p, const std::string& folder) {
    std::error_code ec;
    if (!p.empty() && fs::exists(p, ec)) return p;
    const std::string name = fs::path(p).filename().string();
    const std::string near = folder + "/" + name;
    if (!name.empty() && fs::exists(near, ec)) return near;
    return p;
}

static double segmentStartTag(const std::string& file) {
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, file.c_str(), nullptr, nullptr) < 0) return -1;
    double v = -1;
    if (const AVDictionaryEntry* e = av_dict_get(fc->metadata, "PINGDUB_SEGMENT_START", nullptr, 0)) v = std::atof(e->value);
    avformat_close_input(&fc);
    return v;
}

bool loadCaptureRecord(const std::string& path, CaptureRecord& r, std::string& err) {
    r = CaptureRecord{};
    r.basePath = captureBaseFor(path);
    r.jsonPath = r.basePath + ".json";
    r.csvPath = r.basePath + ".log.csv";
    const std::string folder = fs::path(r.basePath).parent_path().string();
    std::error_code ec;
    const bool isMedia = path.size() > 4 && (path.compare(path.size() - 4, 4, ".mkv") == 0 || path.compare(path.size() - 4, 4, ".avi") == 0);
    r.mediaPath = isMedia ? path : std::string();

    JsonValue j;
    std::string jerr;
    r.haveJson = fs::exists(r.jsonPath, ec) && readJsonFile(r.jsonPath, j, jerr) && j.isObject();
    if (r.haveJson) {
        const Rational fr = parseRate(j["video"]["frame_rate"].str());
        if (fr.valid()) r.fps = fr;
        r.label = j["tape_label"].str();
        r.startTime = j["capture_start"].str();
        r.stopReason = j["stop_reason"].str();
        r.device = j["device"]["name"].str();
        r.input = j["device"]["input"].str();
        r.standard = j["video"]["standard"].str();
        for (const auto& [k, v] : j["totals"].o)
            if (v.type == JsonValue::Type::Number || v.type == JsonValue::Type::Bool) r.totals[k] = v.num();
        r.frames = j["totals"]["frames"].integer(0);
        r.seconds = j["totals"]["seconds"].num(0);
        for (const JsonValue& w : j["warnings"].a) r.warnings.push_back(w.str());
        if (j["segments"].isArray()) {
            for (const JsonValue& s : j["segments"].a) {
                RecordSegment seg;
                seg.file = resolveNear(s["file"].str(), folder);
                seg.startSlot = s["start_frame"].integer(0);
                seg.frames = s["frames"].integer(-1);
                r.segments.push_back(seg);
            }
        } else {
            // Older captures: the files list, with segment starts from their tags.
            const auto& files = j["files"].a;
            for (size_t i = 0; i < files.size(); ++i) {
                RecordSegment seg;
                seg.file = resolveNear(files[i].str(), folder);
                if (files.size() > 1) {
                    const double t = segmentStartTag(seg.file);
                    seg.startSlot = t >= 0 ? std::llround(t * r.fps.toDouble()) : 0;
                }
                r.segments.push_back(seg);
            }
        }
        for (const JsonValue& c : j["chapters"].a) r.chapters.push_back({c["start"].integer(), c["end"].integer(), c["title"].str()});
        const JsonValue& cs = j["contact_sheet"];
        if (cs.isObject()) {
            r.sheet.intervalSec = cs["interval_sec"].num(0);
            r.sheet.tileWidth = int(cs["tile_width"].integer(320));
            r.sheet.tileHeight = int(cs["tile_height"].integer(240));
            r.sheet.columns = int(cs["columns"].integer(6));
            r.sheet.rows = int(cs["rows"].integer(10));
            r.sheet.gap = int(cs["gap"].integer(8));
            r.sheet.labelHeight = int(cs["label_height"].integer(22));
            for (const JsonValue& p : cs["pages"].a) r.sheet.pages.push_back(resolveNear(p.str(), folder));
            for (const JsonValue& t : cs["thumbs"].a)
                r.sheet.thumbs.push_back({t["frame"].integer(), int(t["page"].integer()), int(t["index"].integer())});
        }
    }
    if (r.mediaPath.empty() && !r.segments.empty()) r.mediaPath = r.segments.front().file;
    if (r.mediaPath.empty()) {
        for (const char* ext : {".mkv", ".avi"})
            if (fs::exists(r.basePath + ext, ec)) r.mediaPath = r.basePath + ext;
    }

    std::ifstream csv(r.csvPath);
    r.haveCsv = csv.good();
    if (r.haveCsv) {
        std::string line;
        std::vector<std::string> f;
        bool header = true;
        while (std::getline(csv, line)) {
            if (header) {
                header = false;
                if (line.rfind("wallclock,", 0) == 0) continue;
            }
            if (line.empty() || !parseCsvLine(line, f) || f.size() < 6) continue;
            RecordEvent e;
            e.wallclock = f[0];
            e.slot = f[1].empty() ? -1 : std::atoll(f[1].c_str());
            e.event = f[3];
            e.count = std::strtoull(f[4].c_str(), nullptr, 10);
            e.detail = f[5];
            e.lane = laneOf(e.event, e.label);
            const bool spans = e.event == "insert" || e.event.rfind("levels_", 0) == 0 ||
                               (e.event.rfind("audio_", 0) == 0 && e.event != "audio_xrun" && e.event != "audio_resync");
            e.endSlot = e.slot >= 0 && spans && e.count > 0 ? e.slot + int64_t(e.count) - 1 : e.slot;
            r.events.push_back(e);
        }
    }
    if (!r.haveJson && !r.haveCsv) {
        err = "No PingDub sidecars (.json, .log.csv) next to " + path;
        return false;
    }
    if (r.frames <= 0) {
        for (const RecordEvent& e : r.events)
            if (e.event == "capture_stop" && e.slot > 0) r.frames = e.slot;
    }
    if (r.seconds <= 0 && r.frames > 0) r.seconds = double(r.frames) * r.fps.den / r.fps.num;
    r.spots = countSpots(r.events, r.fps);
    return true;
}

bool decodeFrameRgb(const std::string& file, int64_t index, RgbImage& out, std::string& err) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, file.c_str(), nullptr, nullptr) < 0) {
        err = "cannot open " + file;
        return false;
    }
    avformat_find_stream_info(in, nullptr);
    const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        avformat_close_input(&in);
        err = "no video stream";
        return false;
    }
    AVStream* st = in->streams[vi];
    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (!fr.num) fr = AVRational{30000, 1001};
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dc = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!dc || avcodec_parameters_to_context(dc, st->codecpar) < 0 || avcodec_open2(dc, dec, nullptr) < 0) {
        if (dc) avcodec_free_context(&dc);
        avformat_close_input(&in);
        err = "no decoder for this file";
        return false;
    }
    const int64_t target = av_rescale_q(index, av_inv_q(fr), st->time_base);
    av_seek_frame(in, vi, target, AVSEEK_FLAG_BACKWARD);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    bool got = false;
    auto idxOf = [&](int64_t ts) { return int64_t(std::llround(double(ts) * av_q2d(st->time_base) * av_q2d(fr))); };
    auto pull = [&] {
        while (!got && avcodec_receive_frame(dc, f) == 0) {
            if (f->best_effort_timestamp != AV_NOPTS_VALUE && idxOf(f->best_effort_timestamp) >= index) got = true;
            else av_frame_unref(f);
        }
    };
    while (!got && av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            avcodec_send_packet(dc, pkt);
            pull();
        }
        av_packet_unref(pkt);
    }
    if (!got) {
        avcodec_send_packet(dc, nullptr);
        pull();
    }
    bool ok = false;
    if (got) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        if (d && !(d->flags & AV_PIX_FMT_FLAG_RGB) && !(d->flags & AV_PIX_FMT_FLAG_PAL)) {
            const int w = f->width, h = f->height;
            const bool hasChroma = d->nb_components >= 3;
            const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
                              f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;
            const int shift = std::max(0, d->comp[0].depth - 8);
            const int cw = AV_CEIL_RSHIFT(w, d->log2_chroma_w);
            std::vector<uint16_t> ly(static_cast<size_t>(w)), lu(static_cast<size_t>(cw)), lv(static_cast<size_t>(cw));
            out.width = w;
            out.height = h;
            const AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : st->sample_aspect_ratio;
            out.sar = sar.num ? Rational{sar.num, sar.den} : Rational{1, 1};
            out.rgb.assign(size_t(w) * size_t(h) * 3, 0);
            const uint8_t* data[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
            for (int y = 0; y < h; ++y) {
                av_read_image_line2(ly.data(), data, f->linesize, d, 0, y, 0, w, 0, 2);
                if (hasChroma) {
                    const int cy = y >> d->log2_chroma_h;
                    av_read_image_line2(lu.data(), data, f->linesize, d, 0, cy, 1, cw, 0, 2);
                    av_read_image_line2(lv.data(), data, f->linesize, d, 0, cy, 2, cw, 0, 2);
                }
                uint8_t* o = out.rgb.data() + size_t(y) * size_t(w) * 3;
                for (int x = 0; x < w; ++x) {
                    const double Y = double(ly[size_t(x)] >> shift);
                    const int cx = x >> d->log2_chroma_w;
                    const double U = hasChroma ? double(lu[size_t(cx)] >> shift) - 128.0 : 0.0;
                    const double V = hasChroma ? double(lv[size_t(cx)] >> shift) - 128.0 : 0.0;
                    double R, G, B;
                    if (full) {
                        R = Y + 1.402 * V;
                        G = Y - 0.344136 * U - 0.714136 * V;
                        B = Y + 1.772 * U;
                    } else {
                        const double yy = 1.164383 * (Y - 16.0);
                        R = yy + 1.596027 * V;
                        G = yy - 0.391762 * U - 0.812968 * V;
                        B = yy + 2.017232 * U;
                    }
                    o[3 * x + 0] = uint8_t(std::clamp(std::lround(R), 0L, 255L));
                    o[3 * x + 1] = uint8_t(std::clamp(std::lround(G), 0L, 255L));
                    o[3 * x + 2] = uint8_t(std::clamp(std::lround(B), 0L, 255L));
                }
            }
            ok = true;
        } else {
            err = "unsupported picture format";
        }
    } else {
        err = "frame " + std::to_string(index) + " is not in the file";
    }
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&dc);
    avformat_close_input(&in);
    return ok;
}

}  // namespace pd
