// SPDX-License-Identifier: GPL-3.0-or-later
// Audio meters: peak and RMS per channel in dBFS, a peak-hold tick, and a
// clip light that latches until clicked.
#pragma once

#include "engine/CaptureSession.h"

#include <QWidget>

namespace pd {

class Meters : public QWidget {
    Q_OBJECT
public:
    explicit Meters(QWidget* parent = nullptr);
    void setLevels(const AudioLevels& l);
    QSize sizeHint() const override { return {260, 90}; }

signals:
    void clipReset();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    AudioLevels m_l;
    float m_hold[8];
    qint64 m_holdTime[8];
};

}  // namespace pd
