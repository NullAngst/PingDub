// SPDX-License-Identifier: GPL-3.0-or-later
// Writes short captures with every codec and container, then checks them with
// ffprobe (pixel format, size, frame rate, field order, SAR, color tags,
// patched totals) and decodes the video back to confirm it is bit-exact.
#include "engine/Output.h"
#include "engine/PixelConvert.h"
#include "engine/Recover.h"
#include "engine/SyncEngine.h"
#include "testing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace pd;
namespace fs = std::filesystem;

namespace {

std::string tmpDir() {
    static std::string dir;
    if (dir.empty()) {
        char tmpl[] = "/tmp/pingdub-test-XXXXXX";
        const char* base = std::getenv("PD_TEST_TMP");
        std::string t = base ? std::string(base) + "/pingdub-test-XXXXXX" : tmpl;
        std::vector<char> buf(t.begin(), t.end());
        buf.push_back(0);
        dir = mkdtemp(buf.data());
    }
    return dir;
}

std::string ffprobePath() {
    if (const char* p = std::getenv("PD_FFPROBE")) return p;
    return "ffprobe";
}

std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

// Finds "key": "value" or "key": number inside the first stream block of the given type.
std::string field(const std::string& json, const std::string& codecType, const std::string& key) {
    size_t pos = 0;
    while (true) {
        pos = json.find("\"codec_type\": \"" + codecType + "\"", pos);
        if (pos == std::string::npos) return {};
        size_t start = json.rfind('{', pos);
        size_t end = json.find("\"disposition\"", pos);
        if (end == std::string::npos) end = json.size();
        const std::string block = json.substr(start, end - start);
        const size_t k = block.find("\"" + key + "\": ");
        if (k == std::string::npos) return {};
        size_t v = k + key.size() + 4;
        if (block[v] == '"') {
            const size_t e = block.find('"', v + 1);
            return block.substr(v + 1, e - v - 1);
        }
        const size_t e = block.find_first_of(",\n}", v);
        return block.substr(v, e - v);
    }
}

std::string formatTag(const std::string& json, const std::string& key) {
    const size_t f = json.find("\"format\"");
    if (f == std::string::npos) return {};
    // Matroska tags come back as written; look either case.
    for (const std::string& k : {key, std::string()}) {
        (void)k;
    }
    size_t t = json.find("\"" + key + "\": \"", f);
    if (t == std::string::npos) return {};
    t += key.size() + 5;
    return json.substr(t, json.find('"', t) - t);
}

VideoFormat makeFormat(PixFmt pf, bool ntsc) {
    VideoFormat vf;
    vf.pixfmt = pf;
    vf.width = 720;
    vf.height = ntsc ? 480 : 576;
    vf.lines525 = ntsc;
    vf.fps = ntsc ? Rational{30000, 1001} : Rational{25, 1};
    vf.field = ntsc ? FieldOrder::BFF : FieldOrder::TFF;
    vf.sar = ntsc ? Rational{10, 11} : Rational{12, 11};
    switch (pf) {
    case PixFmt::YUYV:
    case PixFmt::UYVY: vf.bytesPerLine = vf.width * 2; vf.frameBytes = size_t(vf.bytesPerLine) * vf.height; break;
    case PixFmt::Y210: vf.bytesPerLine = vf.width * 4; vf.frameBytes = size_t(vf.bytesPerLine) * vf.height; break;
    case PixFmt::V210: vf.bytesPerLine = v210Stride(vf.width); vf.frameBytes = size_t(vf.bytesPerLine) * vf.height; break;
    case PixFmt::NV12: vf.bytesPerLine = vf.width; vf.frameBytes = size_t(vf.width) * vf.height * 3 / 2; break;
    case PixFmt::HM12: vf.bytesPerLine = vf.width; vf.frameBytes = hm12Size(vf.width, vf.height); break;
    case PixFmt::GREY: vf.bytesPerLine = vf.width; vf.frameBytes = size_t(vf.width) * vf.height; break;
    default: break;
    }
    return vf;
}

// Pseudo-random but deterministic content that exercises every value.
BytesPtr makeFrame(const VideoFormat& vf, int idx) {
    auto b = std::make_shared<Bytes>(vf.frameBytes);
    uint32_t s = 0x9e3779b9u * uint32_t(idx + 1);
    for (size_t i = 0; i < b->size(); ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        (*b)[i] = uint8_t(s);
    }
    if (vf.pixfmt == PixFmt::Y210) {
        // 10 bits in the top of each word, low 6 bits zero.
        for (size_t i = 0; i + 1 < b->size(); i += 2) (*b)[i] &= 0xC0;
    }
    if (vf.pixfmt == PixFmt::V210) {
        for (size_t i = 3; i < b->size(); i += 4) (*b)[i] &= 0x3F;
    }
    return b;
}

struct Written {
    std::vector<std::string> files;
    std::vector<BytesPtr> frames;
    uint64_t audioFrames = 0;
};

Written writeCapture(const std::string& name, Container cont, VideoCodec vc, PixFmt pf, bool ntsc,
                     AudioCodec ac, SampleFmt sf, int nframes, double segmentSeconds = 0,
                     bool withAudio = true) {
    OutputPlan plan;
    plan.out.container = cont;
    plan.out.vcodec = vc;
    plan.out.acodec = ac;
    plan.out.segmentSeconds = segmentSeconds;
    plan.out.threads = 4;
    plan.vfmt = makeFormat(pf, ntsc);
    plan.haveAudio = withAudio;
    plan.afmt.rate = 48000;
    plan.afmt.channels = 2;
    plan.afmt.fmt = sf;
    plan.folder = tmpDir();
    plan.stem = name;
    plan.tags["PINGDUB_DEVICE"] = "test";
    plan.tags["TITLE"] = name;
    plan.deferredTags = {"PINGDUB_INSERTED", "PINGDUB_DRIFT_PPM"};
    EventLog log;
    OutputWriter w(&log);
    std::string err;
    Written out;
    if (!w.open(plan, err)) {
        std::printf("  open failed: %s\n", err.c_str());
        return out;
    }
    const double spf = 48000.0 * plan.vfmt.fps.den / plan.vfmt.fps.num;
    std::vector<int32_t> audio;
    uint64_t emitted = 0;
    for (int i = 0; i < nframes; ++i) {
        VideoFrame f;
        f.data = makeFrame(plan.vfmt, i);
        f.size = f.data->size();
        out.frames.push_back(f.data);
        w.syncVideo(f, i, false);
        if (withAudio) {
            const uint64_t target = uint64_t(std::llround((i + 1) * spf));
            const size_t n = size_t(target - emitted);
            audio.assign(n * 2, 0);
            for (size_t k = 0; k < n; ++k) {
                const int32_t v = int32_t((emitted + k) * 7919u) << 8;
                audio[2 * k] = sf == SampleFmt::S16 ? (v & int32_t(0xFFFF0000)) : (sf == SampleFmt::S24 ? (v & int32_t(0xFFFFFF00)) : v);
                audio[2 * k + 1] = -audio[2 * k];
            }
            w.syncAudio(audio.data(), n);
            emitted = target;
        }
    }
    out.audioFrames = emitted;
    w.close({{"PINGDUB_INSERTED", "3"}, {"PINGDUB_DRIFT_PPM", "12.5"}});
    if (w.failed()) std::printf("  writer error: %s\n", w.error().c_str());
    out.files = w.files();
    return out;
}

// Decodes the first video frame and compares it with the source buffer.
bool firstFrameMatches(const std::string& path, const VideoFormat& vf, const BytesPtr& src) {
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return false;
    avformat_find_stream_info(fc, nullptr);
    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        avformat_close_input(&fc);
        return false;
    }
    const AVCodec* dec = avcodec_find_decoder(fc->streams[vi]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fc->streams[vi]->codecpar);
    avcodec_open2(ctx, dec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    bool got = false;
    while (!got && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            avcodec_send_packet(ctx, pkt);
            if (avcodec_receive_frame(ctx, fr) == 0) got = true;
        }
        av_packet_unref(pkt);
    }
    if (!got) {
        avcodec_send_packet(ctx, nullptr);
        got = avcodec_receive_frame(ctx, fr) == 0;
    }
    bool match = false;
    if (got) {
        const int w = vf.width, h = vf.height;
        std::vector<uint8_t> y(size_t(w) * h), u(size_t(w) * h), v(size_t(w) * h);
        std::vector<uint16_t> y16(size_t(w) * h), u16(size_t(w) * h), v16(size_t(w) * h);
        match = true;
        auto cmp8 = [&](int plane, const uint8_t* ref, int refStride, int pw, int ph) {
            for (int r = 0; r < ph && match; ++r)
                if (std::memcmp(fr->data[plane] + size_t(r) * fr->linesize[plane], ref + size_t(r) * refStride, size_t(pw)))
                    match = false;
        };
        auto cmp16 = [&](int plane, const uint16_t* ref, int refStride, int pw, int ph) {
            for (int r = 0; r < ph && match; ++r)
                if (std::memcmp(fr->data[plane] + size_t(r) * fr->linesize[plane], ref + size_t(r) * refStride, size_t(pw) * 2))
                    match = false;
        };
        switch (vf.pixfmt) {
        case PixFmt::YUYV:
        case PixFmt::UYVY:
            if (fr->format == AV_PIX_FMT_YUYV422 || fr->format == AV_PIX_FMT_UYVY422) {
                cmp8(0, src->data(), vf.bytesPerLine, w * 2, h);
            } else {
                Planes8 p{y.data(), u.data(), v.data(), w, w / 2, w / 2};
                if (vf.pixfmt == PixFmt::YUYV) yuyvToPlanar(src->data(), vf.bytesPerLine, w, h, p);
                else uyvyToPlanar(src->data(), vf.bytesPerLine, w, h, p);
                cmp8(0, y.data(), w, w, h);
                cmp8(1, u.data(), w / 2, w / 2, h);
                cmp8(2, v.data(), w / 2, w / 2, h);
            }
            break;
        case PixFmt::Y210:
        case PixFmt::V210: {
            Planes16 p{y16.data(), u16.data(), v16.data(), w, w / 2, w / 2};
            if (vf.pixfmt == PixFmt::Y210) y210ToPlanar10(src->data(), vf.bytesPerLine, w, h, p);
            else v210ToPlanar10(src->data(), vf.bytesPerLine, w, h, p);
            cmp16(0, y16.data(), w, w, h);
            cmp16(1, u16.data(), w / 2, w / 2, h);
            cmp16(2, v16.data(), w / 2, w / 2, h);
            break;
        }
        case PixFmt::NV12: {
            Planes8 p{y.data(), u.data(), v.data(), w, w / 2, w / 2};
            nv12ToPlanar(src->data(), vf.bytesPerLine, w, h, p);
            cmp8(0, y.data(), w, w, h);
            cmp8(1, u.data(), w / 2, w / 2, h / 2);
            cmp8(2, v.data(), w / 2, w / 2, h / 2);
            break;
        }
        case PixFmt::GREY: cmp8(0, src->data(), w, w, h); break;
        default: match = false;
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fc);
    return match;
}

int64_t countPackets(const std::string& path, AVMediaType type, int64_t* samples = nullptr) {
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return -1;
    avformat_find_stream_info(fc, nullptr);
    AVPacket* pkt = av_packet_alloc();
    int64_t n = 0, s = 0;
    while (av_read_frame(fc, pkt) >= 0) {
        const AVCodecParameters* par = fc->streams[pkt->stream_index]->codecpar;
        if (par->codec_type == type) {
            ++n;
            if (type == AVMEDIA_TYPE_AUDIO && par->codec_id != AV_CODEC_ID_FLAC) {
                const int bps = av_get_bits_per_sample(par->codec_id) / 8;
                if (bps) s += pkt->size / (bps * par->ch_layout.nb_channels);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fc);
    if (samples) *samples = s;
    return n;
}

void checkProbe(const std::string& file, const char* pix, int w, int h, const char* rate, const char* fieldOrder,
                const char* sar, bool ntsc, bool mkv) {
    const std::string json = run(ffprobePath() + " -v error -show_streams -show_format -of json '" + file + "'");
    CHECK_MSG(!json.empty(), "ffprobe produced nothing for %s", file.c_str());
    CHECK_MSG(field(json, "video", "pix_fmt") == pix, "pix_fmt %s", field(json, "video", "pix_fmt").c_str());
    CHECK_EQ(std::atoi(field(json, "video", "width").c_str()), w);
    CHECK_EQ(std::atoi(field(json, "video", "height").c_str()), h);
    CHECK_MSG(field(json, "video", "r_frame_rate") == rate, "rate %s", field(json, "video", "r_frame_rate").c_str());
    if (mkv) {
        CHECK_MSG(field(json, "video", "field_order") == fieldOrder, "field_order %s",
                  field(json, "video", "field_order").c_str());
        CHECK_MSG(field(json, "video", "sample_aspect_ratio") == sar, "sar %s",
                  field(json, "video", "sample_aspect_ratio").c_str());
        CHECK_MSG(field(json, "video", "color_range") == "tv", "range %s", field(json, "video", "color_range").c_str());
        const char* spc = ntsc ? "smpte170m" : "bt470bg";
        // UT Video's decoder reports its own BT.601 flag as bt470bg; the
        // matrix is the same one, so accept either for that codec.
        const std::string cs = field(json, "video", "color_space");
        const bool utv = field(json, "video", "codec_name") == "utvideo";
        CHECK_MSG(cs == spc || (utv && cs == "bt470bg"), "color_space %s", cs.c_str());
        CHECK_MSG(field(json, "video", "color_primaries") == spc, "primaries %s",
                  field(json, "video", "color_primaries").c_str());
        CHECK_MSG(formatTag(json, "PINGDUB_INSERTED") == "3", "tag PINGDUB_INSERTED='%s'",
                  formatTag(json, "PINGDUB_INSERTED").c_str());
        CHECK_MSG(formatTag(json, "PINGDUB_DRIFT_PPM") == "12.5", "tag PINGDUB_DRIFT_PPM='%s'",
                  formatTag(json, "PINGDUB_DRIFT_PPM").c_str());
        CHECK_MSG(formatTag(json, "PINGDUB_DEVICE") == "test", "tag PINGDUB_DEVICE='%s'",
                  formatTag(json, "PINGDUB_DEVICE").c_str());
    }
}

}  // namespace

TEST_CASE("MKV FFV1 YUYV NTSC: probe and bit-exact") {
    auto w = writeCapture("ffv1_yuyv", Container::MKV, VideoCodec::FFV1, PixFmt::YUYV, true, AudioCodec::PCM, SampleFmt::S16, 30);
    CHECK_EQ(w.files.size(), size_t(1));
    if (w.files.empty()) return;
    checkProbe(w.files[0], "yuv422p", 720, 480, "30000/1001", "bb", "10:11", true, true);
    CHECK(firstFrameMatches(w.files[0], makeFormat(PixFmt::YUYV, true), w.frames[0]));
    CHECK_EQ(countPackets(w.files[0], AVMEDIA_TYPE_VIDEO), 30);
    const std::string json = run(ffprobePath() + " -v error -show_streams -of json '" + w.files[0] + "'");
    CHECK_MSG(field(json, "audio", "sample_fmt") == "s16", "audio fmt %s", field(json, "audio", "sample_fmt").c_str());
    CHECK_EQ(std::atoi(field(json, "audio", "sample_rate").c_str()), 48000);
    CHECK_EQ(std::atoi(field(json, "audio", "channels").c_str()), 2);
}

TEST_CASE("MKV FFV1 UYVY PAL: TFF, 12:11, bt470bg") {
    auto w = writeCapture("ffv1_uyvy_pal", Container::MKV, VideoCodec::FFV1, PixFmt::UYVY, false, AudioCodec::PCM, SampleFmt::S24, 25);
    if (w.files.empty()) { CHECK(false); return; }
    checkProbe(w.files[0], "yuv422p", 720, 576, "25/1", "tt", "12:11", false, true);
    CHECK(firstFrameMatches(w.files[0], makeFormat(PixFmt::UYVY, false), w.frames[0]));
    const std::string json = run(ffprobePath() + " -v error -show_streams -of json '" + w.files[0] + "'");
    CHECK_MSG(field(json, "audio", "codec_name") == "pcm_s24le", "audio %s", field(json, "audio", "codec_name").c_str());
}

TEST_CASE("MKV FFV1 10-bit Y210 and v210 stay 10-bit") {
    for (PixFmt pf : {PixFmt::Y210, PixFmt::V210}) {
        auto w = writeCapture(pf == PixFmt::Y210 ? "ffv1_y210" : "ffv1_v210", Container::MKV, VideoCodec::FFV1, pf, true,
                              AudioCodec::FLAC, SampleFmt::S24, 10);
        if (w.files.empty()) { CHECK(false); continue; }
        checkProbe(w.files[0], "yuv422p10le", 720, 480, "30000/1001", "bb", "10:11", true, true);
        CHECK(firstFrameMatches(w.files[0], makeFormat(pf, true), w.frames[0]));
    }
}

TEST_CASE("MKV FFV1 NV12 stays 4:2:0, GREY stays gray") {
    auto a = writeCapture("ffv1_nv12", Container::MKV, VideoCodec::FFV1, PixFmt::NV12, true, AudioCodec::PCM, SampleFmt::S16, 10);
    if (!a.files.empty()) {
        checkProbe(a.files[0], "yuv420p", 720, 480, "30000/1001", "bb", "10:11", true, true);
        CHECK(firstFrameMatches(a.files[0], makeFormat(PixFmt::NV12, true), a.frames[0]));
    } else CHECK(false);
    auto g = writeCapture("ffv1_grey", Container::MKV, VideoCodec::FFV1, PixFmt::GREY, true, AudioCodec::PCM, SampleFmt::S16, 10);
    if (!g.files.empty()) {
        checkProbe(g.files[0], "gray", 720, 480, "30000/1001", "bb", "10:11", true, true);
        CHECK(firstFrameMatches(g.files[0], makeFormat(PixFmt::GREY, true), g.frames[0]));
    } else CHECK(false);
}

TEST_CASE("MKV UT Video, FFVHuff, uncompressed") {
    struct C { VideoCodec vc; const char* pix; const char* name; };
    for (C c : {C{VideoCodec::UTVideo, "yuv422p", "utv"}, C{VideoCodec::Huff, "yuv422p", "ffvhuff"},
                C{VideoCodec::Raw, "yuyv422", "raw"}}) {
        auto w = writeCapture(std::string("mkv_") + c.name, Container::MKV, c.vc, PixFmt::YUYV, true, AudioCodec::FLAC,
                              SampleFmt::S16, 12);
        if (w.files.empty()) { CHECK(false); continue; }
        checkProbe(w.files[0], c.pix, 720, 480, "30000/1001", "bb", "10:11", true, true);
        CHECK_MSG(firstFrameMatches(w.files[0], makeFormat(PixFmt::YUYV, true), w.frames[0]), "%s not bit-exact", c.name);
        const std::string json = run(ffprobePath() + " -v error -show_streams -of json '" + w.files[0] + "'");
        CHECK_MSG(field(json, "audio", "codec_name") == "flac", "audio %s", field(json, "audio", "codec_name").c_str());
    }
}

TEST_CASE("AVI: FFV1, HuffYUV, uncompressed with PCM") {
    struct C { VideoCodec vc; const char* pix; const char* codec; };
    for (C c : {C{VideoCodec::FFV1, "yuv422p", "ffv1"}, C{VideoCodec::Huff, "yuv422p", "huffyuv"},
                C{VideoCodec::Raw, "yuyv422", "rawvideo"}}) {
        auto w = writeCapture(std::string("avi_") + c.codec, Container::AVI, c.vc, PixFmt::YUYV, true, AudioCodec::PCM,
                              SampleFmt::S16, 12);
        if (w.files.empty()) { CHECK(false); continue; }
        CHECK(w.files[0].size() > 4 && w.files[0].substr(w.files[0].size() - 4) == ".avi");
        checkProbe(w.files[0], c.pix, 720, 480, "30000/1001", "", "", true, false);
        const std::string json = run(ffprobePath() + " -v error -show_streams -of json '" + w.files[0] + "'");
        CHECK_MSG(field(json, "video", "codec_name") == c.codec, "codec %s", field(json, "video", "codec_name").c_str());
        CHECK_MSG(firstFrameMatches(w.files[0], makeFormat(PixFmt::YUYV, true), w.frames[0]), "%s not bit-exact", c.codec);
    }
}

TEST_CASE("codec/format pairs that cannot be lossless are refused") {
    std::string why;
    CHECK(!videoCodecSupports(VideoCodec::UTVideo, Container::MKV, PixFmt::Y210, why));
    CHECK(!why.empty());
    CHECK(videoCodecSupports(VideoCodec::FFV1, Container::MKV, PixFmt::Y210, why));
    AudioEncoder ae;
    AudioFormat af;
    std::string err;
    CHECK(!ae.open(af, AudioCodec::FLAC, Container::AVI, err));
}

TEST_CASE("segments by duration are frame- and sample-continuous") {
    auto w = writeCapture("segmented", Container::MKV, VideoCodec::FFV1, PixFmt::YUYV, true, AudioCodec::PCM,
                          SampleFmt::S16, 75, 1.0);
    CHECK_EQ(w.files.size(), size_t(3));
    int64_t frames = 0, samples = 0;
    for (const auto& f : w.files) {
        int64_t s = 0;
        frames += countPackets(f, AVMEDIA_TYPE_VIDEO);
        countPackets(f, AVMEDIA_TYPE_AUDIO, &s);
        samples += s;
    }
    CHECK_EQ(frames, 75);
    CHECK_EQ(samples, int64_t(w.audioFrames));
    if (w.files.size() == 3) {
        CHECK(w.files[0].find("_part001.mkv") != std::string::npos);
        CHECK(w.files[2].find("_part003.mkv") != std::string::npos);
        // The second segment starts with source frame 30 (1 s at 29.97 rounds up).
        CHECK(firstFrameMatches(w.files[1], makeFormat(PixFmt::YUYV, true), w.frames[30]));
    }
}

TEST_CASE("recover rebuilds a truncated file") {
    auto w = writeCapture("truncated", Container::MKV, VideoCodec::FFV1, PixFmt::YUYV, true, AudioCodec::PCM,
                          SampleFmt::S16, 90);
    if (w.files.empty()) { CHECK(false); return; }
    const auto size = fs::file_size(w.files[0]);
    fs::resize_file(w.files[0], size * 6 / 10);
    RecoverResult r = recoverCapture(w.files[0]);
    std::printf("  %s\n", r.message.c_str());
    CHECK(r.ok);
    CHECK(r.packets > 40);
    const int64_t v = countPackets(r.outPath, AVMEDIA_TYPE_VIDEO);
    CHECK(v > 40 && v < 90);
    CHECK(firstFrameMatches(r.outPath, makeFormat(PixFmt::YUYV, true), w.frames[0]));
}

TEST_CASE("video without audio") {
    auto w = writeCapture("noaudio", Container::MKV, VideoCodec::FFV1, PixFmt::YUYV, true, AudioCodec::PCM,
                          SampleFmt::S16, 10, 0, false);
    if (w.files.empty()) { CHECK(false); return; }
    CHECK_EQ(countPackets(w.files[0], AVMEDIA_TYPE_VIDEO), 10);
    CHECK_EQ(countPackets(w.files[0], AVMEDIA_TYPE_AUDIO), 0);
}

int main() {
    const int r = pdtest::runAll();
    if (r == 0 && !std::getenv("PD_KEEP_TMP")) fs::remove_all(tmpDir());
    else std::printf("test files kept in %s\n", tmpDir().c_str());
    return r;
}
