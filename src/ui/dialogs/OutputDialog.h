// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/CaptureConfig.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;

namespace pd {

// Container, codec, audio codec, folder, naming, segmenting, overflow folder.
class OutputDialog : public QDialog {
    Q_OBJECT
public:
    OutputDialog(const OutputSettings& o, QWidget* parent = nullptr);
    OutputSettings result() const;

private:
    void refresh();
    OutputSettings m_o;
    QComboBox* m_container;
    QComboBox* m_vcodec;
    QComboBox* m_acodec;
    QLineEdit* m_folder;
    QComboBox* m_segMode;
    QDoubleSpinBox* m_segValue;
    QLineEdit* m_overflow;
    QSpinBox* m_overflowGb;
    QLabel* m_note;
    QLabel* m_naming;
};

}  // namespace pd
