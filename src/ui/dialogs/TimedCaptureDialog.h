// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QDialog>

class QRadioButton;
class QTimeEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;

namespace pd {

struct TimedCapture {
    QDateTime startAt;      // invalid: start now
    double durationSec = 0; // 0: run until stopped
};

class TimedCaptureDialog : public QDialog {
    Q_OBJECT
public:
    explicit TimedCaptureDialog(QWidget* parent = nullptr);
    TimedCapture result() const;

private:
    QRadioButton* m_now;
    QRadioButton* m_at;
    QRadioButton* m_after;
    QTimeEdit* m_time;
    QSpinBox* m_delay;
    QCheckBox* m_stop;
    QComboBox* m_preset;
    QSpinBox* m_minutes;
};

}  // namespace pd
