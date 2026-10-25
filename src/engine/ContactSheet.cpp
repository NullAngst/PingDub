// SPDX-License-Identifier: GPL-3.0-or-later
#include "ContactSheet.h"

#include "Chapters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace pd {

namespace {

// 5x7 digits and colon, bit 4 is the left column.
const uint8_t kGlyphs[11][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},  // :
};
constexpr int kScale = 2;
constexpr int kAdvance = 6 * kScale;
constexpr uint8_t kBackY = 40;  // dark gray page

inline uint8_t fullY(int v) { return uint8_t(std::clamp((v - 16) * 255 / 219, 0, 255)); }
inline uint8_t fullC(int v) { return uint8_t(std::clamp((v - 128) * 255 / 224 + 128, 0, 255)); }

// Area average of an 8-bit plane into a rectangle of the page.
void boxScale(const uint8_t* src, int sw, int sh, uint8_t* dst, int dstride, int dw, int dh, bool chroma) {
    std::vector<int> x0(size_t(dw) + 1);
    for (int i = 0; i <= dw; ++i) x0[size_t(i)] = int(int64_t(i) * sw / dw);
    for (int oy = 0; oy < dh; ++oy) {
        const int ya = int(int64_t(oy) * sh / dh);
        const int yb = std::max(ya + 1, int(int64_t(oy + 1) * sh / dh));
        uint8_t* out = dst + size_t(oy) * size_t(dstride);
        for (int ox = 0; ox < dw; ++ox) {
            const int xa = x0[size_t(ox)], xb = std::max(xa + 1, x0[size_t(ox) + 1]);
            uint32_t sum = 0;
            for (int y = ya; y < yb && y < sh; ++y) {
                const uint8_t* row = src + size_t(y) * size_t(sw);
                for (int x = xa; x < xb && x < sw; ++x) sum += row[x];
            }
            const int n = std::max(1, (std::min(yb, sh) - ya) * (std::min(xb, sw) - xa));
            const int v = int((sum + uint32_t(n / 2)) / uint32_t(n));
            out[ox] = chroma ? fullC(v) : fullY(v);
        }
    }
}

}  // namespace

ContactSheet::~ContactSheet() = default;

int ContactSheet::tileHeightFor(const VideoFormat& fmt, int tileWidth) {
    const double sar = fmt.sar.valid() ? fmt.sar.toDouble() : 1.0;
    const double dar = fmt.height > 0 ? double(fmt.width) * sar / double(fmt.height) : 4.0 / 3.0;
    const int h = int(std::lround(double(tileWidth) / std::max(0.1, dar) / 2.0)) * 2;
    return std::clamp(h, 16, 4 * tileWidth);
}

int ContactSheet::clockWidth(const std::string& text) { return int(text.size()) * kAdvance - kScale; }

void ContactSheet::drawClock(uint8_t* plane, int stride, int x, int y, const std::string& text, uint8_t value) {
    for (char ch : text) {
        int g = -1;
        if (ch >= '0' && ch <= '9') g = ch - '0';
        else if (ch == ':') g = 10;
        if (g >= 0) {
            for (int r = 0; r < 7; ++r)
                for (int c = 0; c < 5; ++c) {
                    if (!(kGlyphs[g][r] & (0x10 >> c))) continue;
                    for (int dy = 0; dy < kScale; ++dy)
                        for (int dx = 0; dx < kScale; ++dx)
                            plane[size_t(y + r * kScale + dy) * size_t(stride) + size_t(x + c * kScale + dx)] = value;
                }
        }
        x += kAdvance;
    }
}

bool ContactSheet::begin(const std::string& basePath, const VideoFormat& fmt, double intervalSec, std::string& err) {
    m_active = false;
    m_info = ContactSheetInfo{};
    m_error.clear();
    if (intervalSec <= 0) return true;
    if (!avcodec_find_encoder(AV_CODEC_ID_MJPEG)) {
        err = "this FFmpeg build has no JPEG encoder, so no contact sheet";
        m_error = err;
        return false;
    }
    m_base = basePath;
    m_fps = fmt.fps;
    m_interval = std::max<int64_t>(1, std::llround(intervalSec * fmt.fps.toDouble()));
    m_next = 0;
    m_info.intervalSec = intervalSec;
    m_info.tileWidth = kTileWidth;
    m_info.tileHeight = tileHeightFor(fmt, kTileWidth);
    m_info.columns = kColumns;
    m_info.rows = kRows;
    m_info.gap = kGap;
    m_info.labelHeight = kLabel;
    m_pageW = kColumns * kTileWidth + (kColumns + 1) * kGap;
    m_pageH = kRows * (m_info.tileHeight + kLabel) + (kRows + 1) * kGap;
    m_onPage = 0;
    clearPage();
    m_active = true;
    return true;
}

void ContactSheet::clearPage() {
    m_py.assign(size_t(m_pageW) * size_t(m_pageH), kBackY);
    m_pu.assign(size_t(m_pageW / 2) * size_t(m_pageH / 2), 128);
    m_pv.assign(m_pu.size(), 128);
}

void ContactSheet::add(const PlaneView& p, int64_t slot) {
    if (!m_active || !p.y) return;
    const int idx = m_onPage;
    const int tx = m_info.tileX(idx), ty = m_info.tileY(idx);
    const int tw = m_info.tileWidth, th = m_info.tileHeight;
    boxScale(p.y, p.width, p.height, m_py.data() + size_t(ty) * size_t(m_pageW) + size_t(tx), m_pageW, tw, th, false);
    uint8_t* u = m_pu.data() + size_t(ty / 2) * size_t(m_pageW / 2) + size_t(tx / 2);
    uint8_t* v = m_pv.data() + size_t(ty / 2) * size_t(m_pageW / 2) + size_t(tx / 2);
    if (p.cb && p.cr) {
        boxScale(p.cb, p.cwidth, p.cheight, u, m_pageW / 2, tw / 2, th / 2, true);
        boxScale(p.cr, p.cwidth, p.cheight, v, m_pageW / 2, tw / 2, th / 2, true);
    }
    const std::string clock = slotClock(slot, m_fps);
    const int cx = tx + (tw - clockWidth(clock)) / 2;
    const int cy = ty + th + (kLabel - 7 * kScale) / 2;
    drawClock(m_py.data(), m_pageW, cx, cy, clock, 235);
    m_info.thumbs.push_back({slot, int(m_info.pages.size()), idx});
    m_next = (slot / m_interval + 1) * m_interval;
    if (++m_onPage == kColumns * kRows) {
        writePage(kRows);
        m_onPage = 0;
        clearPage();
    }
}

void ContactSheet::finish() {
    if (!m_active) return;
    if (m_onPage > 0) writePage((m_onPage + kColumns - 1) / kColumns);
    m_onPage = 0;
    m_active = false;
}

bool ContactSheet::writePage(int rowsUsed) {
    char suffix[32];
    std::snprintf(suffix, sizeof suffix, ".contact%02d.jpg", int(m_info.pages.size()) + 1);
    const std::string path = m_base + suffix;
    int h = rowsUsed * (m_info.tileHeight + kLabel) + (rowsUsed + 1) * kGap;
    h = std::min(m_pageH, (h + 1) & ~1);
    const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!c) return false;
    AVCodecContext* ctx = nullptr;
    AVFrame* fr = nullptr;
    AVPacket* pkt = nullptr;
    bool ok = false;
    for (AVPixelFormat pf : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P}) {
        ctx = avcodec_alloc_context3(c);
        ctx->width = m_pageW;
        ctx->height = h;
        ctx->pix_fmt = pf;
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->time_base = AVRational{1, 25};
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = FF_QP2LAMBDA * 3;
        if (avcodec_open2(ctx, c, nullptr) == 0) break;
        avcodec_free_context(&ctx);
    }
    if (!ctx) {
        m_error = "could not open the JPEG encoder";
        return false;
    }
    fr = av_frame_alloc();
    fr->format = ctx->pix_fmt;
    fr->width = m_pageW;
    fr->height = h;
    fr->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(fr, 32) == 0) {
        for (int y = 0; y < h; ++y) std::memcpy(fr->data[0] + size_t(y) * size_t(fr->linesize[0]), m_py.data() + size_t(y) * size_t(m_pageW), size_t(m_pageW));
        for (int y = 0; y < h / 2; ++y) {
            std::memcpy(fr->data[1] + size_t(y) * size_t(fr->linesize[1]), m_pu.data() + size_t(y) * size_t(m_pageW / 2), size_t(m_pageW / 2));
            std::memcpy(fr->data[2] + size_t(y) * size_t(fr->linesize[2]), m_pv.data() + size_t(y) * size_t(m_pageW / 2), size_t(m_pageW / 2));
        }
        fr->pts = 0;
        fr->quality = ctx->global_quality;
        pkt = av_packet_alloc();
        if (avcodec_send_frame(ctx, fr) == 0 && avcodec_send_frame(ctx, nullptr) == 0 && avcodec_receive_packet(ctx, pkt) == 0) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f) {
                ok = std::fwrite(pkt->data, 1, size_t(pkt->size), f) == size_t(pkt->size);
                ok = (std::fclose(f) == 0) && ok;
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&ctx);
    if (ok) m_info.pages.push_back(path);
    else m_error = "could not write " + path;
    return ok;
}

}  // namespace pd
