// SPDX-License-Identifier: GPL-3.0-or-later
// Luma histogram with the 16 and 235 limits marked and the share of pixels
// outside them, so brightness and contrast can be set without crushing or
// clipping. A luma waveform view is a toggle on the same widget.
#pragma once

#include "PreviewWidget.h"

#include <QImage>
#include <QWidget>

#include <array>
#include <memory>

namespace pd {

class Histogram : public QWidget {
    Q_OBJECT
public:
    explicit Histogram(QWidget* parent = nullptr);
    void setLuma(std::shared_ptr<const LumaPlane> luma);
    void setWaveform(bool on);
    bool waveform() const { return m_wave; }
    QSize sizeHint() const override { return {300, 90}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    std::array<uint32_t, 256> m_hist{};
    uint64_t m_total = 0, m_below = 0, m_above = 0;
    bool m_wave = false;
    QImage m_waveImg;
};

}  // namespace pd
