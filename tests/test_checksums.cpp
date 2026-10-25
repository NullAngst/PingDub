// SPDX-License-Identifier: GPL-3.0-or-later
// The .framemd5 sidecar against FFmpeg's own framemd5 of the same file, for
// every codec, and Verify against a single flipped byte: the quick check
// catches it, the full check names the frame.
#include "capture_util.h"
#include "engine/Checksums.h"
#include "testing.h"

#include <cstring>
#include <fstream>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

using namespace pd;
using namespace pdtest;

namespace {

std::vector<std::string> dataLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l))
        if (!l.empty() && l.rfind("#software", 0) != 0) out.push_back(l);
    return out;
}

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// What `ffmpeg -i file -map 0:v:0 -f framemd5 -` does, through the library:
// decode, lay each frame out as rawvideo, feed the framemd5 muxer.
bool libraryFramemd5(const std::string& file, const std::string& outPath, std::string& why) {
    if (!av_guess_format("framemd5", nullptr, nullptr)) {
        why = "this FFmpeg has no framemd5 muxer";
        return false;
    }
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, file.c_str(), nullptr, nullptr) < 0) return false;
    avformat_find_stream_info(in, nullptr);
    const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVStream* st = in->streams[vi];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dc, st->codecpar);
    avcodec_open2(dc, dec, nullptr);
    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    AVFormatContext* out = nullptr;
    avformat_alloc_output_context2(&out, nullptr, "framemd5", outPath.c_str());
    AVStream* os = avformat_new_stream(out, nullptr);
    os->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    os->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    os->codecpar->width = st->codecpar->width;
    os->codecpar->height = st->codecpar->height;
    os->codecpar->format = dc->pix_fmt;
    os->time_base = av_inv_q(fr);
    os->sample_aspect_ratio = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
    avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE);
    AVDictionary* opts = nullptr;
    if (avformat_write_header(out, &opts) < 0) return false;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    auto drain = [&] {
        while (avcodec_receive_frame(dc, f) == 0) {
            const int size = av_image_get_buffer_size(AVPixelFormat(f->format), f->width, f->height, 1);
            AVPacket* op = av_packet_alloc();
            av_new_packet(op, size);
            av_image_copy_to_buffer(op->data, size, f->data, f->linesize, AVPixelFormat(f->format), f->width, f->height, 1);
            const int64_t idx = std::llround(double(f->best_effort_timestamp) * av_q2d(st->time_base) * av_q2d(fr));
            op->pts = op->dts = idx;
            op->duration = 1;
            op->stream_index = 0;
            av_write_frame(out, op);
            av_packet_free(&op);
            av_frame_unref(f);
        }
    };
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            avcodec_send_packet(dc, pkt);
            drain();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dc, nullptr);
    drain();
    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&dc);
    avformat_close_input(&in);
    return true;
}

std::string ffmpegCli() {
    if (const char* p = std::getenv("PD_FFMPEG")) return p;
    return shell("command -v ffmpeg 2>/dev/null").empty() ? std::string() : std::string("ffmpeg");
}

// Offset and size of video packet `n`.
bool packetAt(const std::string& file, int n, int64_t& pos, int& size) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, file.c_str(), nullptr, nullptr) < 0) return false;
    avformat_find_stream_info(in, nullptr);
    const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket* pkt = av_packet_alloc();
    int k = 0;
    bool ok = false;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vi && k++ == n) {
            pos = pkt->pos;
            size = pkt->size;
            ok = pos >= 0;
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&in);
    return ok;
}

struct CodecCase {
    const char* name;
    Container cont;
    VideoCodec codec;
    PixFmt pf;
};

}  // namespace

TEST_CASE("sha-256 matches the reference at every length and split") {
    std::printf("  CPU SHA instructions: %s\n", Sha256::hardware() ? "yes" : "no, FFmpeg's C version");
    auto hexOf = [](const std::vector<uint8_t>& d, size_t split) {
        Sha256 s;
        s.update(d.data(), std::min(split, d.size()));
        if (split < d.size()) s.update(d.data() + split, d.size() - split);
        uint8_t out[32];
        s.final(out);
        return toHex(out, 32);
    };
    const std::string abc = "abc";
    CHECK(hexOf(std::vector<uint8_t>(abc.begin(), abc.end()), 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hexOf({}, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    uint32_t s = 1;
    for (size_t n : {1u, 55u, 56u, 63u, 64u, 65u, 119u, 120u, 127u, 128u, 1000u, 65537u}) {
        std::vector<uint8_t> d(n);
        for (auto& b : d) b = uint8_t(s = s * 1103515245u + 12345u);
        AVSHA* ref = av_sha_alloc();
        av_sha_init(ref, 256);
        av_sha_update(ref, d.data(), d.size());
        uint8_t r[32];
        av_sha_final(ref, r);
        av_free(ref);
        for (size_t split : {size_t(0), size_t(1), n / 2, size_t(63), n})
            CHECK_MSG(hexOf(d, split) == toHex(r, 32), "length %zu split %zu", n, split);
    }
}

TEST_CASE("framemd5 sidecar equals FFmpeg's framemd5, every codec") {
    const CodecCase cases[] = {
        {"ffv1_yuyv", Container::MKV, VideoCodec::FFV1, PixFmt::YUYV},
        {"ffv1_nv12", Container::MKV, VideoCodec::FFV1, PixFmt::NV12},
        {"ffv1_grey", Container::MKV, VideoCodec::FFV1, PixFmt::GREY},
        {"ffv1_v210", Container::MKV, VideoCodec::FFV1, PixFmt::V210},
        {"ffv1_y210", Container::MKV, VideoCodec::FFV1, PixFmt::Y210},
        {"utvideo_yuyv", Container::MKV, VideoCodec::UTVideo, PixFmt::YUYV},
        {"ffvhuff_yuyv", Container::MKV, VideoCodec::Huff, PixFmt::YUYV},
        {"huffyuv_avi", Container::AVI, VideoCodec::Huff, PixFmt::YUYV},
        {"raw_yuyv", Container::MKV, VideoCodec::Raw, PixFmt::YUYV},
        {"raw_avi", Container::AVI, VideoCodec::Raw, PixFmt::YUYV},
    };
    const std::string cli = ffmpegCli();
    if (cli.empty()) std::printf("  no ffmpeg on PATH: comparing with the library framemd5 muxer only\n");
    for (const CodecCase& c : cases) {
        std::string why;
        if (!videoCodecSupports(c.codec, c.cont, c.pf, why)) {
            std::printf("  %s skipped: %s\n", c.name, why.c_str());
            continue;
        }
        WriteOptions o;
        o.container = c.cont;
        o.codec = c.codec;
        o.pixfmt = c.pf;
        o.width = c.pf == PixFmt::V210 ? 192 : 160;
        o.frames = 40;
        o.repeats = {7, 8, 20};  // inserted repeats share the last frame's buffer and hash
        const auto w = writeTestCapture(std::string("md5_") + c.name, o);
        CHECK_MSG(w.segments.size() == 1 && !w.segments[0].frameMd5.empty(), "%s: no sidecar", c.name);
        if (w.segments.empty() || w.segments[0].frameMd5.empty()) continue;
        const std::string file = w.segments[0].path;
        const auto mine = dataLines(slurp(w.segments[0].frameMd5));
        CHECK_MSG(mine.size() == size_t(o.frames) + 9, "%s: %zu lines", c.name, mine.size());
        const std::string ref = tmpDir() + "/" + c.name + ".ref.framemd5";
        if (libraryFramemd5(file, ref, why)) {
            const auto theirs = dataLines(slurp(ref));
            CHECK_MSG(mine == theirs, "%s: sidecar differs from the library framemd5", c.name);
        } else {
            std::printf("  %s: %s\n", c.name, why.c_str());
        }
        if (!cli.empty()) {
            const auto theirs = dataLines(shell(cli + " -v error -i '" + file + "' -map 0:v:0 -f framemd5 - 2>/dev/null"));
            CHECK_MSG(mine == theirs, "%s: sidecar differs from ffmpeg -f framemd5", c.name);
        }
    }
}

TEST_CASE("verify: clean file passes, a flipped byte is caught and named") {
    for (const CodecCase& c : {CodecCase{"ffv1", Container::MKV, VideoCodec::FFV1, PixFmt::YUYV},
                          CodecCase{"raw", Container::MKV, VideoCodec::Raw, PixFmt::YUYV},
                          CodecCase{"huffyuv", Container::AVI, VideoCodec::Huff, PixFmt::YUYV}}) {
        WriteOptions o;
        o.container = c.cont;
        o.codec = c.codec;
        o.pixfmt = c.pf;
        o.width = 320;
        o.height = 240;
        o.frames = 30;
        const auto w = writeTestCapture(std::string("verify_") + c.name, o);
        if (w.files.empty()) continue;
        const std::string file = w.files[0];
        std::string hex, err;
        CHECK(sha256File(file, hex, err));
        const std::string side = writeSha256Sidecar(file, hex);
        CHECK(!side.empty());
        if (!shell("command -v sha256sum 2>/dev/null").empty()) {
            const size_t slash = side.find_last_of('/');
            const std::string out = shell("cd '" + side.substr(0, slash) + "' && sha256sum -c '" + side.substr(slash + 1) + "' 2>&1");
            CHECK_MSG(out.find(": OK") != std::string::npos, "%s: sha256sum -c said %s", c.name, out.c_str());
        }
        VerifyResult clean = verifyCapture(file, true);
        CHECK_MSG(clean.ok, "%s clean: %s", c.name, clean.summary().c_str());
        CHECK_EQ(clean.framesChecked, uint64_t(o.frames));

        const int victim = 17;
        int64_t pos = 0;
        int size = 0;
        CHECK(packetAt(file, victim, pos, size));
        {
            std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
            f.seekg(pos + size / 2);
            char b = 0;
            f.read(&b, 1);
            b = char(b ^ 0x10);
            f.seekp(pos + size / 2);
            f.write(&b, 1);
        }
        const VerifyResult quick = verifyCapture(file, false);
        CHECK(!quick.ok);
        CHECK(quick.haveSha && !quick.shaMatch);
        const VerifyResult full = verifyCapture(file, true);
        CHECK(!full.ok);
        auto has = [&](const std::vector<int64_t>& v) { return std::find(v.begin(), v.end(), int64_t(victim)) != v.end(); };
        const bool named = has(full.badFrames) || has(full.decodeErrors) || has(full.crcErrors);
        CHECK_MSG(named, "%s: frame %d not named: %s", c.name, victim, full.summary().c_str());
        // Nothing else is blamed.
        CHECK_MSG(full.badFrames.size() <= 1 && full.missingFrames.empty(), "%s: %s", c.name, full.summary().c_str());
        if (c.codec == VideoCodec::FFV1) CHECK_MSG(has(full.crcErrors), "ffv1: slice CRC not reported: %s", full.summary().c_str());
    }
}

TEST_MAIN()
