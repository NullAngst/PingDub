// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreviewWidget.h"

#include "Theme.h"
#include "engine/PixelConvert.h"

#include <QPainter>
#include <QTimer>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace pd {

static const char* kVert = R"(
attribute vec2 aPos;
attribute vec2 aTex;
varying vec2 vTex;
void main() {
    vTex = aTex;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* kFrag = R"(
#ifdef GL_ES
precision highp float;
#endif
varying vec2 vTex;
uniform sampler2D tY;
uniform sampler2D tU;
uniform sampler2D tV;
uniform int mode;      // 0 YUYV, 1 UYVY, 2 planar, 3 gray
uniform vec2 size;     // luma size in pixels
uniform vec2 csize;    // chroma plane size
uniform int deint;     // 0 weave, 1 bob, 2 blend
uniform int field;     // bob: 0 top, 1 bottom

vec3 sampleYuv(float col, float row) {
    float y; float u; float v;
    if (mode == 0 || mode == 1) {
        float tx = floor(col / 2.0);
        vec4 t = texture2D(tY, vec2((tx + 0.5) / (size.x / 2.0), (row + 0.5) / size.y));
        bool odd = mod(col, 2.0) >= 1.0;
        if (mode == 0) { y = odd ? t.b : t.r; u = t.g; v = t.a; }
        else { y = odd ? t.a : t.g; u = t.r; v = t.b; }
    } else {
        y = texture2D(tY, vec2((col + 0.5) / size.x, (row + 0.5) / size.y)).r;
        if (mode == 3) { u = 0.5; v = 0.5; }
        else {
            float cx = floor(col * csize.x / size.x);
            float cy = floor(row * csize.y / size.y);
            vec2 cc = vec2((cx + 0.5) / csize.x, (cy + 0.5) / csize.y);
            u = texture2D(tU, cc).r;
            v = texture2D(tV, cc).r;
        }
    }
    return vec3(y, u, v);
}

void main() {
    float col = floor(vTex.x * size.x);
    float row = floor(vTex.y * size.y);
    vec3 yuv;
    if (deint == 1) {
        float r = 2.0 * floor(row / 2.0) + float(field);
        yuv = sampleYuv(col, min(r, size.y - 1.0));
    } else if (deint == 2) {
        float other = mod(row, 2.0) >= 1.0 ? row - 1.0 : min(row + 1.0, size.y - 1.0);
        yuv = 0.5 * (sampleYuv(col, row) + sampleYuv(col, other));
    } else {
        yuv = sampleYuv(col, row);
    }
    // BT.601, limited range
    float Y = (yuv.x * 255.0 - 16.0) / 219.0;
    float U = (yuv.y * 255.0 - 128.0) / 224.0;
    float V = (yuv.z * 255.0 - 128.0) / 224.0;
    vec3 rgb = vec3(Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U);
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

PreviewWidget::PreviewWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(320, 240);
    m_message = tr("No device");
}

PreviewWidget::~PreviewWidget() {
    makeCurrent();
    if (m_glReady) glDeleteTextures(4, m_tex);
    doneCurrent();
    if (m_mjpeg) avcodec_free_context(&m_mjpeg);
    if (m_decoded) av_frame_free(&m_decoded);
    if (m_pkt) av_packet_free(&m_pkt);
}

void PreviewWidget::setFormat(const VideoFormat& fmt) {
    m_fmt = fmt;
    m_haveFrame = false;
    update();
}

void PreviewWidget::clearFrame() {
    m_haveFrame = false;
    update();
}

void PreviewWidget::setMessage(const QString& m) {
    m_message = m;
    update();
}

void PreviewWidget::setDeinterlace(PreviewDeinterlace d) {
    m_deint = d;
    update();
}

void PreviewWidget::initializeGL() {
    initializeOpenGLFunctions();
    m_prog.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
    m_prog.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    m_prog.bindAttributeLocation("aPos", 0);
    m_prog.bindAttributeLocation("aTex", 1);
    m_prog.link();
    glGenTextures(4, m_tex);
    for (unsigned t : m_tex) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    m_glReady = true;
}

void PreviewWidget::resizeGL(int, int) {}

void PreviewWidget::uploadPlane(int idx, const uint8_t* data, int w, int h, int stride, bool rgba) {
    const int bpp = rgba ? 4 : 1;
    const uint8_t* src = data;
    std::vector<uint8_t> tight;
    if (stride != w * bpp) {
        tight.resize(size_t(w) * bpp * h);
        for (int r = 0; r < h; ++r) std::memcpy(tight.data() + size_t(r) * w * bpp, data + size_t(r) * stride, size_t(w) * bpp);
        src = tight.data();
    }
    glBindTexture(GL_TEXTURE_2D, m_tex[idx]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLenum f = rgba ? GL_RGBA : GL_LUMINANCE;
    if (m_texW[idx] != w || m_texH[idx] != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(f), w, h, 0, f, GL_UNSIGNED_BYTE, src);
        m_texW[idx] = w;
        m_texH[idx] = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, f, GL_UNSIGNED_BYTE, src);
    }
}

bool PreviewWidget::decodeMjpeg(const VideoFrame& f) {
    if (!m_mjpeg) {
        const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!c) return false;
        m_mjpeg = avcodec_alloc_context3(c);
        if (avcodec_open2(m_mjpeg, c, nullptr) < 0) return false;
        m_decoded = av_frame_alloc();
        m_pkt = av_packet_alloc();
    }
    av_packet_unref(m_pkt);
    if (av_new_packet(m_pkt, int(f.size)) < 0) return false;
    std::memcpy(m_pkt->data, f.data->data(), f.size);
    if (avcodec_send_packet(m_mjpeg, m_pkt) < 0) return false;
    return avcodec_receive_frame(m_mjpeg, m_decoded) == 0;
}

void PreviewWidget::prepare(const VideoFrame& f) {
    const int w = m_fmt.width, h = m_fmt.height, stride = m_fmt.bytesPerLine;
    const uint8_t* src = f.data->data();
    auto luma = std::make_shared<LumaPlane>();
    luma->width = w / 2;
    luma->height = h;
    luma->y.resize(size_t(luma->width) * h);
    switch (m_fmt.pixfmt) {
    case PixFmt::YUYV:
    case PixFmt::UYVY: {
        if (f.size < size_t(stride) * h) return;
        m_mode = m_fmt.pixfmt == PixFmt::YUYV ? 0 : 1;
        makeCurrent();
        uploadPlane(0, src, w / 2, h, stride, true);
        doneCurrent();
        const int off = m_mode == 0 ? 0 : 1;
        for (int r = 0; r < h; ++r) {
            const uint8_t* s = src + size_t(r) * stride + off;
            uint8_t* d = luma->y.data() + size_t(r) * luma->width;
            for (int x = 0; x < luma->width; ++x) d[x] = s[4 * x];
        }
        break;
    }
    case PixFmt::GREY: {
        if (f.size < size_t(stride) * h) return;
        m_mode = 3;
        makeCurrent();
        uploadPlane(0, src, w, h, stride, false);
        doneCurrent();
        for (int r = 0; r < h; ++r)
            for (int x = 0; x < luma->width; ++x) luma->y[size_t(r) * luma->width + x] = src[size_t(r) * stride + 2 * x];
        break;
    }
    default: {
        int cw = w / 2, ch = h;
        m_py.resize(size_t(w) * h);
        if (m_fmt.pixfmt == PixFmt::Y210 || m_fmt.pixfmt == PixFmt::V210) {
            std::vector<uint16_t> y(size_t(w) * h), u(size_t(cw) * h), v(size_t(cw) * h);
            Planes16 p{y.data(), u.data(), v.data(), w, cw, cw};
            if (m_fmt.pixfmt == PixFmt::Y210) y210ToPlanar10(src, stride, w, h, p);
            else v210ToPlanar10(src, stride, w, h, p);
            m_pu.resize(u.size());
            m_pv.resize(v.size());
            for (size_t i = 0; i < y.size(); ++i) m_py[i] = uint8_t(y[i] >> 2);
            for (size_t i = 0; i < u.size(); ++i) {
                m_pu[i] = uint8_t(u[i] >> 2);
                m_pv[i] = uint8_t(v[i] >> 2);
            }
        } else if (m_fmt.pixfmt == PixFmt::NV12 || m_fmt.pixfmt == PixFmt::HM12) {
            ch = h / 2;
            m_pu.resize(size_t(cw) * ch);
            m_pv.resize(size_t(cw) * ch);
            Planes8 p{m_py.data(), m_pu.data(), m_pv.data(), w, cw, cw};
            if (m_fmt.pixfmt == PixFmt::NV12) {
                if (f.size < size_t(stride) * h * 3 / 2) return;
                nv12ToPlanar(src, stride, w, h, p);
            } else {
                if (f.size < hm12Size(w, h)) return;
                hm12ToPlanar(src, w, h, p);
            }
        } else if (m_fmt.pixfmt == PixFmt::MJPEG) {
            if (!decodeMjpeg(f)) return;
            const AVFrame* d = m_decoded;
            if (d->width != w || d->height != h) return;
            const bool c420 = d->format == AV_PIX_FMT_YUVJ420P || d->format == AV_PIX_FMT_YUV420P;
            ch = c420 ? h / 2 : h;
            m_pu.resize(size_t(cw) * ch);
            m_pv.resize(size_t(cw) * ch);
            for (int r = 0; r < h; ++r) std::memcpy(m_py.data() + size_t(r) * w, d->data[0] + size_t(r) * d->linesize[0], size_t(w));
            for (int r = 0; r < ch; ++r) {
                std::memcpy(m_pu.data() + size_t(r) * cw, d->data[1] + size_t(r) * d->linesize[1], size_t(cw));
                std::memcpy(m_pv.data() + size_t(r) * cw, d->data[2] + size_t(r) * d->linesize[2], size_t(cw));
            }
        } else {
            return;
        }
        m_mode = 2;
        m_chromaW = cw;
        m_chromaH = ch;
        makeCurrent();
        uploadPlane(0, m_py.data(), w, h, w, false);
        uploadPlane(1, m_pu.data(), cw, ch, cw, false);
        uploadPlane(2, m_pv.data(), cw, ch, cw, false);
        doneCurrent();
        for (int r = 0; r < h; ++r)
            for (int x = 0; x < luma->width; ++x) luma->y[size_t(r) * luma->width + x] = m_py[size_t(r) * w + 2 * x];
        break;
    }
    }
    m_luma = luma;
    m_haveFrame = true;
}

void PreviewWidget::showFrame(const VideoFrame& f) {
    if (!f.data || !m_glReady) return;
    if (m_paused) return;
    prepare(f);
    if (m_deint == PreviewDeinterlace::Bob) {
        // Show the first field now and the second half a frame later.
        const bool bottomFirst = m_fmt.field == FieldOrder::BFF;
        m_bobField = bottomFirst ? 1 : 0;
        const int half = int(500.0 * m_fmt.fps.den / std::max(1, m_fmt.fps.num));
        QTimer::singleShot(half, this, [this, bottomFirst] {
            m_bobField = bottomFirst ? 0 : 1;
            update();
        });
    }
    update();
}

void PreviewWidget::paintGL() {
    const QColor s = StatusColor::PreviewSurround;
    glClearColor(s.redF(), s.greenF(), s.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_haveFrame || m_fmt.width <= 0) {
        QPainter p(this);
        p.setPen(QColor(0xE0, 0xE0, 0xE0));
        p.drawText(rect(), Qt::AlignCenter, m_message);
        return;
    }
    // Display aspect from the pixel aspect; scale to fit, centered.
    const double dar = (double(m_fmt.width) * m_fmt.sar.num / std::max(1, m_fmt.sar.den)) / double(m_fmt.height);
    const double dpr = devicePixelRatioF();
    const int W = int(width() * dpr), H = int(height() * dpr);
    int vw = W, vh = int(W / dar);
    if (vh > H) {
        vh = H;
        vw = int(H * dar);
    }
    glViewport((W - vw) / 2, (H - vh) / 2, vw, vh);
    m_prog.bind();
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + unsigned(i));
        glBindTexture(GL_TEXTURE_2D, m_tex[i]);
    }
    glActiveTexture(GL_TEXTURE0);
    m_prog.setUniformValue("tY", 0);
    m_prog.setUniformValue("tU", 1);
    m_prog.setUniformValue("tV", 2);
    m_prog.setUniformValue("mode", m_mode);
    m_prog.setUniformValue("size", QVector2D(float(m_fmt.width), float(m_fmt.height)));
    m_prog.setUniformValue("csize", QVector2D(float(std::max(1, m_chromaW)), float(std::max(1, m_chromaH))));
    m_prog.setUniformValue("deint", int(m_deint));
    m_prog.setUniformValue("field", m_bobField);
    static const GLfloat pos[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const GLfloat tex[] = {0, 1, 1, 1, 0, 0, 1, 0};
    m_prog.enableAttributeArray(0);
    m_prog.enableAttributeArray(1);
    m_prog.setAttributeArray(0, GL_FLOAT, pos, 2);
    m_prog.setAttributeArray(1, GL_FLOAT, tex, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_prog.disableAttributeArray(0);
    m_prog.disableAttributeArray(1);
    m_prog.release();
    glViewport(0, 0, W, H);
}

}  // namespace pd
