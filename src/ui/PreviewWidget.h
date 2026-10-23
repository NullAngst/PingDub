// SPDX-License-Identifier: GPL-3.0-or-later
// Preview: YUV goes to the GPU as textures and is converted in a shader, so
// preview costs almost no CPU and the cores stay free for FFV1. Packed YUYV
// and UYVY upload as-is; other formats are split into 8-bit planes first.
// Deinterlacing here is for display only and never touches the file.
//
// QOpenGLWidget is used rather than QRhiWidget: it runs on every Qt 6 release
// and needs no shader baking step (Qt Shader Tools) in the build.
#pragma once

#include "engine/Types.h"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

#include <memory>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace pd {

enum class PreviewDeinterlace { Weave, Bob, Blend };

// 8-bit luma plane shared with the histogram.
struct LumaPlane {
    std::vector<uint8_t> y;
    int width = 0, height = 0;
};

class PreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;

    void setFormat(const VideoFormat& fmt);
    void showFrame(const VideoFrame& f);
    void clearFrame();
    void setDeinterlace(PreviewDeinterlace d);
    PreviewDeinterlace deinterlace() const { return m_deint; }
    void setPaused(bool p) { m_paused = p; }
    bool paused() const { return m_paused; }
    void setMessage(const QString& m);

    // Luma of the most recent frame, subsampled 2x horizontally.
    std::shared_ptr<const LumaPlane> luma() const { return m_luma; }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void prepare(const VideoFrame& f);
    void uploadPlane(int idx, const uint8_t* data, int w, int h, int stride, bool rgba);
    bool decodeMjpeg(const VideoFrame& f);

    VideoFormat m_fmt;
    QOpenGLShaderProgram m_prog;
    unsigned m_tex[4] = {0, 0, 0, 0};
    int m_texW[4] = {}, m_texH[4] = {};
    bool m_glReady = false;
    bool m_haveFrame = false;
    bool m_paused = false;
    int m_mode = 0;  // 0 YUYV packed, 1 UYVY packed, 2 planar, 3 gray
    int m_chromaW = 0, m_chromaH = 0;
    PreviewDeinterlace m_deint = PreviewDeinterlace::Weave;
    int m_bobField = 0;
    QString m_message;
    std::vector<uint8_t> m_py, m_pu, m_pv, m_packed;
    std::shared_ptr<const LumaPlane> m_luma;
    AVCodecContext* m_mjpeg = nullptr;
    AVFrame* m_decoded = nullptr;
    AVPacket* m_pkt = nullptr;
};

}  // namespace pd
