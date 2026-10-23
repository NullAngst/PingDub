// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/CaptureConfig.h"

#include <QDialog>

class QRadioButton;
class QComboBox;
class QSpinBox;

namespace pd {

// Sync mode, gap fill, V4L2 buffer count, RAM ring size.
class TimingDialog : public QDialog {
    Q_OBJECT
public:
    TimingDialog(const TimingSettings& t, QWidget* parent = nullptr);
    TimingSettings result() const;

private:
    QRadioButton* m_video;
    QRadioButton* m_audio;
    QRadioButton* m_none;
    QComboBox* m_gap;
    QSpinBox* m_buffers;
    QSpinBox* m_ring;
};

}  // namespace pd
