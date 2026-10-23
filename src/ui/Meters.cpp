// SPDX-License-Identifier: GPL-3.0-or-later
#include "Meters.h"

#include "Theme.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>

namespace pd {

static constexpr float kFloor = -60.0f;

Meters::Meters(QWidget* parent) : QWidget(parent) {
    for (int i = 0; i < 8; ++i) {
        m_hold[i] = -120.0f;
        m_holdTime[i] = 0;
    }
    setToolTip(tr("Peak (bar) and RMS (inner bar) in dBFS. The clip light stays on until you click the meters."));
    setMinimumHeight(70);
}

void Meters::setLevels(const AudioLevels& l) {
    m_l = l;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < l.channels && i < 8; ++i) {
        if (l.peakDb[i] >= m_hold[i] || now - m_holdTime[i] > 2000) {
            m_hold[i] = l.peakDb[i];
            m_holdTime[i] = now;
        }
    }
    update();
}

void Meters::mousePressEvent(QMouseEvent*) { emit clipReset(); }

void Meters::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QPalette pal = palette();
    const int n = std::max(1, m_l.channels);
    const int labelW = 26, clipW = 34;
    const int top = 4, scaleH = 14;
    const int barArea = height() - top - scaleH - 4;
    const int barH = std::max(6, (barArea - (n - 1) * 3) / n);
    const int x0 = labelW, x1 = width() - clipW - 6;
    auto xFor = [&](float db) {
        const float t = std::clamp((db - kFloor) / -kFloor, 0.0f, 1.0f);
        return x0 + int(t * float(x1 - x0));
    };
    if (m_l.channels == 0) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, tr("No audio"));
        return;
    }
    const QColor ok = ThemeManager::instance().okColor();
    for (int c = 0; c < n; ++c) {
        const int y = top + c * (barH + 3);
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(QRect(0, y, labelW - 4, barH), Qt::AlignRight | Qt::AlignVCenter,
                   n == 2 ? (c == 0 ? "L" : "R") : QString::number(c + 1));
        p.fillRect(QRect(x0, y, x1 - x0, barH), pal.color(QPalette::AlternateBase).darker(110));
        const float peak = m_l.peakDb[c];
        const int px = xFor(peak);
        // Green below -18 dBFS, amber to -6, red above.
        const int g = xFor(-18), a = xFor(-6);
        if (px > x0) p.fillRect(QRect(x0, y, std::min(px, g) - x0, barH), ok);
        if (px > g) p.fillRect(QRect(g, y, std::min(px, a) - g, barH), StatusColor::Warn);
        if (px > a) p.fillRect(QRect(a, y, px - a, barH), StatusColor::Bad);
        const int rx = xFor(m_l.rmsDb[c]);
        if (rx > x0) p.fillRect(QRect(x0, y + barH / 3, rx - x0, barH / 3), QColor(255, 255, 255, 90));
        const int hx = xFor(m_hold[c]);
        p.fillRect(QRect(hx - 1, y, 2, barH), pal.color(QPalette::Text));
        const bool clip = m_l.clipped[c];
        const QRect clipR(x1 + 6, y, clipW - 6, barH);
        p.fillRect(clipR, clip ? StatusColor::Bad : pal.color(QPalette::AlternateBase).darker(120));
        p.setPen(clip ? QColor("#FFFFFF") : pal.color(QPalette::PlaceholderText));
        p.drawText(clipR, Qt::AlignCenter, "CLIP");
    }
    // Scale
    p.setPen(pal.color(QPalette::PlaceholderText));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    const int sy = height() - scaleH;
    for (int db : {-60, -48, -36, -24, -18, -12, -6, -3, 0}) {
        const int x = xFor(float(db));
        p.drawLine(x, sy - 2, x, sy + 1);
        p.drawText(QRect(x - 14, sy, 28, scaleH), Qt::AlignHCenter | Qt::AlignTop, QString::number(db));
    }
}

}  // namespace pd
