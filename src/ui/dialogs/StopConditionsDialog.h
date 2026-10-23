// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/CaptureConfig.h"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;

namespace pd {

// VirtualDub's auto-stop conditions plus two for tapes: signal lost and static picture.
class StopConditionsDialog : public QDialog {
    Q_OBJECT
public:
    StopConditionsDialog(const StopConditions& s, QWidget* parent = nullptr);
    StopConditions result() const;

private:
    struct Row {
        QCheckBox* on;
        QDoubleSpinBox* value;
    };
    Row m_duration, m_size, m_disk, m_drops, m_signal, m_static;
    QDoubleSpinBox* m_staticLevel;
};

}  // namespace pd
