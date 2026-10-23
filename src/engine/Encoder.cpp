// SPDX-License-Identifier: GPL-3.0-or-later
#include "Encoder.h"

#include "PixelConvert.h"

#include <algorithm>
#include <cstring>
#include <thread>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace pd {

const char* containerName(Container c) { return c == Container::AVI ? "AVI" : "Matroska"; }

const char* videoCodecName(VideoCodec c) {
    switch (c) {
    case VideoCodec::FFV1: return "FFV1";
    case VideoCodec::UTVideo: return "UT Video";
    case VideoCodec::Huff: return "FFVHuff/HuffYUV";
    case VideoCodec::Raw: return "Uncompressed";
    }
    return "?";
}

const char* audioCodecName(AudioCodec c) { return c == AudioCodec::FLAC ? "FLAC" : "PCM"; }

std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

AVPixelFormat encoderPixFmt(PixFmt src, VideoCodec codec) {
    if (codec == VideoCodec::Raw) {
        switch (src) {
        case PixFmt::YUYV: return AV_PIX_FMT_YUYV422;
        case PixFmt::UYVY: return AV_PIX_FMT_UYVY422;
        case PixFmt::Y210: return AV_PIX_FMT_Y210LE;
        case PixFmt::V210: return AV_PIX_FMT_YUV422P10LE;
        case PixFmt::NV12: return AV_PIX_FMT_NV12;
        case PixFmt::HM12: return AV_PIX_FMT_YUV420P;
        case PixFmt::GREY: return AV_PIX_FMT_GRAY8;
        default: return AV_PIX_FMT_NONE;
        }
    }
    switch (src) {
    case PixFmt::YUYV:
    case PixFmt::UYVY: return AV_PIX_FMT_YUV422P;
    case PixFmt::Y210:
    case PixFmt::V210: return AV_PIX_FMT_YUV422P10LE;
    case PixFmt::NV12:
    case PixFmt::HM12: return AV_PIX_FMT_YUV420P;
    case PixFmt::GREY: return AV_PIX_FMT_GRAY8;
    default: return AV_PIX_FMT_NONE;
    }
}

const char* encoderName(VideoCodec codec, Container container, PixFmt src) {
    switch (codec) {
    case VideoCodec::FFV1: return "ffv1";
    case VideoCodec::UTVideo: return "utvideo";
    case VideoCodec::Raw: return "rawvideo";
    case VideoCodec::Huff:
        // Classic HuffYUV only does 8-bit 4:2:2 and RGB; it is the one old
        // AVI tools read, so prefer it there when it fits.
        if (container == Container::AVI && encoderPixFmt(src, codec) == AV_PIX_FMT_YUV422P) return "huffyuv";
        return "ffvhuff";
    }
    return "ffv1";
}

static bool codecHasPixFmt(const AVCodec* c, AVPixelFormat pf) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* cfg = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) < 0) return false;
    if (!cfg) return true;
    const AVPixelFormat* p = static_cast<const AVPixelFormat*>(cfg);
    for (int i = 0; i < n; ++i)
        if (p[i] == pf) return true;
    return false;
#else
    if (!c->pix_fmts) return true;
    for (const AVPixelFormat* p = c->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == pf) return true;
    return false;
#endif
}

bool videoCodecSupports(VideoCodec codec, Container container, PixFmt src, std::string& why) {
    if (src == PixFmt::MJPEG) return true;  // stored as delivered
    const AVPixelFormat pf = encoderPixFmt(src, codec);
    if (pf == AV_PIX_FMT_NONE) {
        why = std::string("no lossless mapping for ") + pixFmtName(src);
        return false;
    }
    const AVCodec* c = avcodec_find_encoder_by_name(encoderName(codec, container, src));
    if (!c) {
        why = std::string("this FFmpeg build has no ") + encoderName(codec, container, src) + " encoder";
        return false;
    }
    if (!codecHasPixFmt(c, pf)) {
        why = std::string(videoCodecName(codec)) + " cannot store " + av_get_pix_fmt_name(pf) +
              " without converting it; use FFV1";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

VideoEncoder::~VideoEncoder() {
    if (m_lastPkt) av_packet_free(&m_lastPkt);
    if (m_ctx) avcodec_free_context(&m_ctx);
    if (m_par) avcodec_parameters_free(&m_par);
}

static void applyColorTags(AVCodecContext* ctx, const VideoFormat& fmt) {
    ctx->color_range = AVCOL_RANGE_MPEG;
    if (fmt.lines525) {
        ctx->color_primaries = AVCOL_PRI_SMPTE170M;
        ctx->color_trc = AVCOL_TRC_SMPTE170M;
        ctx->colorspace = AVCOL_SPC_SMPTE170M;
    } else {
        ctx->color_primaries = AVCOL_PRI_BT470BG;
        ctx->color_trc = AVCOL_TRC_GAMMA28;
        ctx->colorspace = AVCOL_SPC_BT470BG;
    }
    ctx->chroma_sample_location = AVCHROMA_LOC_LEFT;
}

static AVFieldOrder toAvField(FieldOrder f) {
    switch (f) {
    case FieldOrder::TFF: return AV_FIELD_TT;
    case FieldOrder::BFF: return AV_FIELD_BB;
    case FieldOrder::Progressive: return AV_FIELD_PROGRESSIVE;
    default: return AV_FIELD_UNKNOWN;
    }
}

static int ffv1Slices(int threads) {
    // FFV1 v3 only accepts slice grids it can lay out evenly.
    static const int valid[] = {4, 6, 9, 12, 16, 24, 30};
    int best = 4;
    for (int v : valid)
        if (v <= std::max(4, threads)) best = v;
    return best;
}

bool VideoEncoder::open(const VideoFormat& fmt, VideoCodec codec, Container container, int threads,
                        std::string& err) {
    m_fmt = fmt;
    if (m_fmt.bytesPerLine == 0) {
        switch (fmt.pixfmt) {
        case PixFmt::YUYV:
        case PixFmt::UYVY: m_fmt.bytesPerLine = fmt.width * 2; break;
        case PixFmt::Y210: m_fmt.bytesPerLine = fmt.width * 4; break;
        case PixFmt::V210: m_fmt.bytesPerLine = v210Stride(fmt.width); break;
        default: m_fmt.bytesPerLine = fmt.width; break;
        }
    }
    m_tb = AVRational{fmt.fps.den, fmt.fps.num};
    if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));

    if (fmt.pixfmt == PixFmt::MJPEG) {
        m_passthrough = true;
        m_par = avcodec_parameters_alloc();
        m_par->codec_type = AVMEDIA_TYPE_VIDEO;
        m_par->codec_id = AV_CODEC_ID_MJPEG;
        m_par->width = fmt.width;
        m_par->height = fmt.height;
        m_par->field_order = toAvField(fmt.field);
        m_par->sample_aspect_ratio = AVRational{fmt.sar.num, fmt.sar.den};
        m_par->color_range = AVCOL_RANGE_JPEG;
        m_desc = "MJPEG as delivered by the device (not archival)";
        return true;
    }

    const char* name = encoderName(codec, container, fmt.pixfmt);
    const AVCodec* c = avcodec_find_encoder_by_name(name);
    if (!c) {
        err = std::string("encoder not available: ") + name;
        return false;
    }
    std::string why;
    if (!videoCodecSupports(codec, container, fmt.pixfmt, why)) {
        err = why;
        return false;
    }
    m_ctx = avcodec_alloc_context3(c);
    m_ctx->width = fmt.width;
    m_ctx->height = fmt.height;
    m_ctx->pix_fmt = encoderPixFmt(fmt.pixfmt, codec);
    m_ctx->time_base = m_tb;
    m_ctx->framerate = AVRational{fmt.fps.num, fmt.fps.den};
    m_ctx->sample_aspect_ratio = AVRational{fmt.sar.num, fmt.sar.den};
    m_ctx->field_order = toAvField(fmt.field);
    m_ctx->gop_size = 1;
    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    m_ctx->thread_count = threads;
    m_ctx->thread_type = FF_THREAD_SLICE;
    applyColorTags(m_ctx, fmt);

    if (codec == VideoCodec::FFV1) {
        m_ctx->level = 3;
        m_ctx->slices = ffv1Slices(threads);
        av_opt_set_int(m_ctx->priv_data, "slicecrc", 1, 0);
        av_opt_set_int(m_ctx->priv_data, "context", 1, 0);
        av_opt_set_int(m_ctx->priv_data, "coder", 1, 0);  // range coder, default table
    } else if (codec == VideoCodec::UTVideo) {
        av_opt_set(m_ctx->priv_data, "pred", "median", 0);
    }

    int r = avcodec_open2(m_ctx, c, nullptr);
    if (r < 0) {
        err = std::string("could not open ") + name + ": " + avErr(r);
        return false;
    }
    m_par = avcodec_parameters_alloc();
    avcodec_parameters_from_context(m_par, m_ctx);
    char d[160];
    if (codec == VideoCodec::FFV1)
        std::snprintf(d, sizeof d, "FFV1 v3, %s, %d slices, range coder, context 1, slice CRC",
                      av_get_pix_fmt_name(m_ctx->pix_fmt), m_ctx->slices);
    else
        std::snprintf(d, sizeof d, "%s, %s", name, av_get_pix_fmt_name(m_ctx->pix_fmt));
    m_desc = d;
    return true;
}

void VideoEncoder::fillBlack(AVFrame* fr) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(fr->format));
    const int depth = desc->comp[0].depth;
    const int ylevel = 16 << (depth - 8);
    const int clevel = 128 << (depth - 8);
    if (fr->format == AV_PIX_FMT_YUYV422 || fr->format == AV_PIX_FMT_UYVY422) {
        const bool yuyv = fr->format == AV_PIX_FMT_YUYV422;
        for (int y = 0; y < fr->height; ++y) {
            uint8_t* p = fr->data[0] + size_t(y) * fr->linesize[0];
            for (int x = 0; x < fr->width / 2; ++x) {
                p[4 * x + 0] = yuyv ? 16 : 128;
                p[4 * x + 1] = yuyv ? 128 : 16;
                p[4 * x + 2] = yuyv ? 16 : 128;
                p[4 * x + 3] = yuyv ? 128 : 16;
            }
        }
        return;
    }
    if (fr->format == AV_PIX_FMT_Y210LE) {
        for (int y = 0; y < fr->height; ++y) {
            uint16_t* p = reinterpret_cast<uint16_t*>(fr->data[0] + size_t(y) * fr->linesize[0]);
            for (int x = 0; x < fr->width / 2; ++x) {
                p[4 * x + 0] = 64 << 6;
                p[4 * x + 1] = 512 << 6;
                p[4 * x + 2] = 64 << 6;
                p[4 * x + 3] = 512 << 6;
            }
        }
        return;
    }
    if (fr->format == AV_PIX_FMT_NV12) {
        for (int y = 0; y < fr->height; ++y) std::memset(fr->data[0] + size_t(y) * fr->linesize[0], 16, size_t(fr->width));
        for (int y = 0; y < fr->height / 2; ++y) std::memset(fr->data[1] + size_t(y) * fr->linesize[1], 128, size_t(fr->width));
        return;
    }
    for (int p = 0; p < desc->nb_components && p < 3; ++p) {
        const int h = p ? AV_CEIL_RSHIFT(fr->height, desc->log2_chroma_h) : fr->height;
        const int w = p ? AV_CEIL_RSHIFT(fr->width, desc->log2_chroma_w) : fr->width;
        const int level = p ? clevel : ylevel;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = fr->data[p] + size_t(y) * fr->linesize[p];
            if (depth > 8) {
                uint16_t* r16 = reinterpret_cast<uint16_t*>(row);
                for (int x = 0; x < w; ++x) r16[x] = uint16_t(level);
            } else {
                std::memset(row, level, size_t(w));
            }
        }
    }
}

bool VideoEncoder::fillFrame(AVFrame* fr, const VideoFrame& f) {
    if (!f.data) {
        fillBlack(fr);
        return true;
    }
    const uint8_t* src = f.data->data();
    const int w = m_fmt.width, h = m_fmt.height, stride = m_fmt.bytesPerLine;
    const size_t need = (m_fmt.pixfmt == PixFmt::HM12) ? hm12Size(w, h)
                        : (m_fmt.pixfmt == PixFmt::NV12) ? size_t(stride) * h * 3 / 2
                                                         : size_t(stride) * h;
    if (f.size < need) {
        // Short buffer from the driver: keep what arrived, black the rest.
        fillBlack(fr);
        if (f.size == 0) return true;
    }
    const int rows = (f.size < need && stride) ? std::min<int>(h, int(f.size / size_t(stride))) : h;
    Planes8 p8{fr->data[0], fr->data[1], fr->data[2], fr->linesize[0], fr->linesize[1], fr->linesize[2]};
    Planes16 p16{reinterpret_cast<uint16_t*>(fr->data[0]), reinterpret_cast<uint16_t*>(fr->data[1]),
                 reinterpret_cast<uint16_t*>(fr->data[2]), fr->linesize[0] / 2, fr->linesize[1] / 2,
                 fr->linesize[2] / 2};
    switch (fr->format) {
    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422:
    case AV_PIX_FMT_Y210LE:
    case AV_PIX_FMT_GRAY8: {
        const size_t lineBytes = size_t(fr->format == AV_PIX_FMT_GRAY8 ? w : (fr->format == AV_PIX_FMT_Y210LE ? w * 4 : w * 2));
        for (int y = 0; y < rows; ++y) std::memcpy(fr->data[0] + size_t(y) * fr->linesize[0], src + size_t(y) * stride, lineBytes);
        return true;
    }
    case AV_PIX_FMT_NV12:
        if (f.size < need) return true;
        for (int y = 0; y < h; ++y) std::memcpy(fr->data[0] + size_t(y) * fr->linesize[0], src + size_t(y) * stride, size_t(w));
        for (int y = 0; y < h / 2; ++y)
            std::memcpy(fr->data[1] + size_t(y) * fr->linesize[1], src + size_t(stride) * h + size_t(y) * stride, size_t(w));
        return true;
    case AV_PIX_FMT_YUV422P:
        if (m_fmt.pixfmt == PixFmt::YUYV) yuyvToPlanar(src, stride, w, rows, p8);
        else uyvyToPlanar(src, stride, w, rows, p8);
        return true;
    case AV_PIX_FMT_YUV422P10LE:
        if (m_fmt.pixfmt == PixFmt::Y210) y210ToPlanar10(src, stride, w, rows, p16);
        else v210ToPlanar10(src, stride, w, rows, p16);
        return true;
    case AV_PIX_FMT_YUV420P:
        if (f.size < need) return true;
        if (m_fmt.pixfmt == PixFmt::HM12) hm12ToPlanar(src, w, h, p8);
        else nv12ToPlanar(src, stride, w, h, p8);
        return true;
    default:
        return false;
    }
}

bool VideoEncoder::drain(std::vector<AVPacket*>& out, int64_t pts) {
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        const int r = avcodec_receive_packet(m_ctx, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            av_packet_free(&pkt);
            return true;
        }
        if (r < 0) {
            av_packet_free(&pkt);
            return false;
        }
        if (pkt->pts == AV_NOPTS_VALUE) pkt->pts = pts;
        pkt->dts = pkt->pts;
        pkt->duration = 1;
        out.push_back(pkt);
    }
}

bool VideoEncoder::encode(const VideoFrame& f, int64_t pts, std::vector<AVPacket*>& out, std::string& err) {
    const void* key = f.data ? static_cast<const void*>(f.data.get()) : nullptr;
    if (m_lastPkt && key == m_lastSrc) {
        AVPacket* pkt = av_packet_clone(m_lastPkt);
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        out.push_back(pkt);
        return true;
    }
    if (m_passthrough) {
        if (!f.data) {
            // No black JPEG to hand: repeat the last one, or skip until the first.
            if (!m_lastPkt) return true;
            AVPacket* pkt = av_packet_clone(m_lastPkt);
            pkt->pts = pkt->dts = pts;
            out.push_back(pkt);
            return true;
        }
        AVPacket* pkt = av_packet_alloc();
        if (av_new_packet(pkt, int(f.size)) < 0) {
            av_packet_free(&pkt);
            err = "out of memory";
            return false;
        }
        std::memcpy(pkt->data, f.data->data(), f.size);
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        pkt->flags |= AV_PKT_FLAG_KEY;
        if (m_lastPkt) av_packet_free(&m_lastPkt);
        m_lastPkt = av_packet_clone(pkt);
        m_lastSrc = key;
        out.push_back(pkt);
        return true;
    }

    AVFrame* fr = av_frame_alloc();
    fr->format = m_ctx->pix_fmt;
    fr->width = m_ctx->width;
    fr->height = m_ctx->height;
    fr->sample_aspect_ratio = m_ctx->sample_aspect_ratio;
    fr->color_range = m_ctx->color_range;
    fr->color_primaries = m_ctx->color_primaries;
    fr->color_trc = m_ctx->color_trc;
    fr->colorspace = m_ctx->colorspace;
    fr->chroma_location = m_ctx->chroma_sample_location;
    if (av_frame_get_buffer(fr, 32) < 0) {
        av_frame_free(&fr);
        err = "out of memory allocating a frame";
        return false;
    }
    fillFrame(fr, f);
    fr->pts = pts;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 29, 100)
    fr->duration = 1;
#endif
    if (m_fmt.field == FieldOrder::TFF || m_fmt.field == FieldOrder::BFF) {
#ifdef AV_FRAME_FLAG_INTERLACED
        fr->flags |= AV_FRAME_FLAG_INTERLACED;
        if (m_fmt.field == FieldOrder::TFF) fr->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
#else
        fr->interlaced_frame = 1;
        fr->top_field_first = m_fmt.field == FieldOrder::TFF;
#endif
    }
    int r = avcodec_send_frame(m_ctx, fr);
    av_frame_free(&fr);
    if (r < 0) {
        err = "encoder rejected frame: " + avErr(r);
        return false;
    }
    const size_t before = out.size();
    if (!drain(out, pts)) {
        err = "encoder failed";
        return false;
    }
    if (out.size() == before + 1) {
        if (m_lastPkt) av_packet_free(&m_lastPkt);
        m_lastPkt = av_packet_clone(out.back());
        m_lastSrc = key;
    } else {
        m_lastSrc = reinterpret_cast<const void*>(-1);
    }
    return true;
}

bool VideoEncoder::flush(std::vector<AVPacket*>& out) {
    if (!m_ctx) return true;
    avcodec_send_frame(m_ctx, nullptr);
    return drain(out, 0);
}

// ---------------------------------------------------------------------------

AudioEncoder::~AudioEncoder() {
    if (m_ctx) avcodec_free_context(&m_ctx);
    if (m_par) avcodec_parameters_free(&m_par);
}

bool AudioEncoder::open(const AudioFormat& fmt, AudioCodec codec, Container container, std::string& err) {
    m_fmt = fmt;
    const int bits = sampleFmtBits(fmt.fmt);
    if (codec == AudioCodec::FLAC && container == Container::AVI) {
        err = "FLAC does not go in AVI; use PCM";
        return false;
    }
    AVCodecID id = AV_CODEC_ID_PCM_S16LE;
    if (codec == AudioCodec::FLAC) id = AV_CODEC_ID_FLAC;
    else if (bits == 24) id = AV_CODEC_ID_PCM_S24LE;
    else if (bits == 32) id = AV_CODEC_ID_PCM_S32LE;
    const AVCodec* c = avcodec_find_encoder(id);
    if (!c) {
        err = std::string("audio encoder not available: ") + avcodec_get_name(id);
        return false;
    }
    m_ctx = avcodec_alloc_context3(c);
    m_ctx->sample_fmt = bits == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
    m_ctx->sample_rate = fmt.rate;
    av_channel_layout_default(&m_ctx->ch_layout, fmt.channels);
    m_ctx->time_base = AVRational{1, fmt.rate};
    m_ctx->bits_per_raw_sample = bits;
    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (codec == AudioCodec::FLAC && bits > 24) m_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    int r = avcodec_open2(m_ctx, c, nullptr);
    if (r < 0) {
        err = std::string("could not open ") + c->name + ": " + avErr(r);
        return false;
    }
    m_frameSize = m_ctx->frame_size > 0 ? m_ctx->frame_size : 1024;
    m_par = avcodec_parameters_alloc();
    avcodec_parameters_from_context(m_par, m_ctx);
    char d[128];
    std::snprintf(d, sizeof d, "%s, %d Hz, %d-bit, %d ch", c->name, fmt.rate, bits, fmt.channels);
    m_desc = d;
    return true;
}

bool AudioEncoder::drain(std::vector<AVPacket*>& out) {
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        const int r = avcodec_receive_packet(m_ctx, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            av_packet_free(&pkt);
            return true;
        }
        if (r < 0) {
            av_packet_free(&pkt);
            return false;
        }
        out.push_back(pkt);
    }
}

bool AudioEncoder::encodeFrames(const int32_t* s, size_t frames, std::vector<AVPacket*>& out) {
    AVFrame* fr = av_frame_alloc();
    fr->nb_samples = int(frames);
    fr->format = m_ctx->sample_fmt;
    fr->sample_rate = m_ctx->sample_rate;
    av_channel_layout_copy(&fr->ch_layout, &m_ctx->ch_layout);
    if (av_frame_get_buffer(fr, 0) < 0) {
        av_frame_free(&fr);
        return false;
    }
    const size_t n = frames * size_t(m_fmt.channels);
    if (m_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
        int16_t* d = reinterpret_cast<int16_t*>(fr->data[0]);
        for (size_t i = 0; i < n; ++i) d[i] = int16_t(s[i] >> 16);
    } else {
        std::memcpy(fr->data[0], s, n * sizeof(int32_t));
    }
    fr->pts = m_pts;
    m_pts += int64_t(frames);
    const int r = avcodec_send_frame(m_ctx, fr);
    av_frame_free(&fr);
    if (r < 0) return false;
    return drain(out);
}

bool AudioEncoder::push(const int32_t* samples, size_t frames, std::vector<AVPacket*>& out) {
    const size_t ch = size_t(m_fmt.channels);
    m_total += frames;
    m_fifo.insert(m_fifo.end(), samples, samples + frames * ch);
    size_t have = m_fifo.size() / ch;
    size_t off = 0;
    bool ok = true;
    while (have - off >= size_t(m_frameSize)) {
        ok &= encodeFrames(m_fifo.data() + off * ch, size_t(m_frameSize), out);
        off += size_t(m_frameSize);
    }
    if (off) m_fifo.erase(m_fifo.begin(), m_fifo.begin() + long(off * ch));
    return ok;
}

bool AudioEncoder::flush(std::vector<AVPacket*>& out) {
    if (!m_ctx) return true;
    const size_t ch = size_t(m_fmt.channels);
    bool ok = true;
    if (!m_fifo.empty()) {
        const size_t frames = m_fifo.size() / ch;
        // Encoders with a fixed frame size need a full frame unless they
        // accept a short last one; PCM and FLAC both do.
        ok &= encodeFrames(m_fifo.data(), frames, out);
        m_fifo.clear();
    }
    avcodec_send_frame(m_ctx, nullptr);
    ok &= drain(out);
    return ok;
}

}  // namespace pd
