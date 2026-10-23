// SPDX-License-Identifier: GPL-3.0-or-later
#include "Recover.h"

#include "Encoder.h"

#include <sys/stat.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace pd {

RecoverResult recoverCapture(const std::string& inPath, const std::string& outPathArg,
                             std::function<bool(uint64_t, uint64_t)> progress) {
    RecoverResult res;
    std::string outPath = outPathArg;
    const bool avi = inPath.size() > 4 && inPath.compare(inPath.size() - 4, 4, ".avi") == 0;
    if (outPath.empty()) {
        const size_t dot = inPath.find_last_of('.');
        outPath = (dot == std::string::npos ? inPath : inPath.substr(0, dot)) + (avi ? ".recovered.avi" : ".recovered.mkv");
    }
    res.outPath = outPath;
    struct stat st;
    const uint64_t total = stat(inPath.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;

    AVFormatContext* in = nullptr;
    int r = avformat_open_input(&in, inPath.c_str(), nullptr, nullptr);
    if (r < 0) {
        res.message = "cannot open " + inPath + ": " + avErr(r);
        return res;
    }
    // Truncated files may lack the info this needs; carry on with what the header says.
    avformat_find_stream_info(in, nullptr);

    AVFormatContext* out = nullptr;
    avformat_alloc_output_context2(&out, nullptr, avi ? "avi" : "matroska", outPath.c_str());
    if (!out) {
        avformat_close_input(&in);
        res.message = "cannot create output";
        return res;
    }
    av_dict_copy(&out->metadata, in->metadata, 0);
    std::vector<int> map(in->nb_streams, -1);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* is = in->streams[i];
        const AVMediaType t = is->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) continue;
        AVStream* os = avformat_new_stream(out, nullptr);
        avcodec_parameters_copy(os->codecpar, is->codecpar);
        os->time_base = is->time_base;
        os->sample_aspect_ratio = is->sample_aspect_ratio;
        os->avg_frame_rate = is->avg_frame_rate;
        av_dict_copy(&os->metadata, is->metadata, 0);
        map[i] = os->index;
    }
    r = avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE);
    if (r < 0) {
        avformat_free_context(out);
        avformat_close_input(&in);
        res.message = "cannot create " + outPath + ": " + avErr(r);
        return res;
    }
    r = avformat_write_header(out, nullptr);
    if (r < 0) {
        avio_closep(&out->pb);
        avformat_free_context(out);
        avformat_close_input(&in);
        res.message = "cannot write header: " + avErr(r);
        return res;
    }

    AVPacket* pkt = av_packet_alloc();
    int64_t lastVideoPts = 0;
    AVRational videoTb{1, 1};
    bool cancelled = false;
    while (true) {
        r = av_read_frame(in, pkt);
        if (r < 0) break;  // EOF or the truncation point
        const int si = pkt->stream_index;
        if (si < 0 || si >= int(map.size()) || map[size_t(si)] < 0) {
            av_packet_unref(pkt);
            continue;
        }
        const AVStream* is = in->streams[si];
        AVStream* os = out->streams[map[size_t(si)]];
        if (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && pkt->pts != AV_NOPTS_VALUE) {
            lastVideoPts = pkt->pts + pkt->duration;
            videoTb = is->time_base;
        }
        av_packet_rescale_ts(pkt, is->time_base, os->time_base);
        pkt->stream_index = os->index;
        pkt->pos = -1;
        if (av_interleaved_write_frame(out, pkt) < 0) {
            av_packet_unref(pkt);
            break;
        }
        ++res.packets;
        if (progress && (res.packets % 256) == 0) {
            const int64_t pos = in->pb ? avio_tell(in->pb) : 0;
            if (!progress(uint64_t(std::max<int64_t>(pos, 0)), total)) {
                cancelled = true;
                break;
            }
        }
    }
    av_packet_free(&pkt);
    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    res.seconds = double(lastVideoPts) * av_q2d(videoTb);
    res.ok = !cancelled && res.packets > 0;
    char msg[256];
    std::snprintf(msg, sizeof msg, "%s: %llu packets, %.1f seconds of video recovered", cancelled ? "cancelled" : "done",
                  (unsigned long long)res.packets, res.seconds);
    res.message = msg;
    return res;
}

}  // namespace pd
