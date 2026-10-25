// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/CaptureConfig.h"
#include "engine/FolderTemplate.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;

namespace pd {

// Container, codec, audio codec, folder template, naming, segmenting,
// overflow folder, chapters and contact sheet. `preview` holds the current
// tape label, device, input and profile, so the folder shows resolved as you type.
class OutputDialog : public QDialog {
    Q_OBJECT
public:
    OutputDialog(const OutputSettings& o, const TemplateValues& preview, QWidget* parent = nullptr);
    OutputSettings result() const;

private:
    void refresh();
    void refreshFolder();
    OutputSettings m_o;
    TemplateValues m_preview;
    QComboBox* m_container;
    QComboBox* m_vcodec;
    QComboBox* m_acodec;
    QLineEdit* m_folder;
    QLabel* m_resolved;
    QComboBox* m_segMode;
    QDoubleSpinBox* m_segValue;
    QLineEdit* m_overflow;
    QSpinBox* m_overflowGb;
    QComboBox* m_chapterInterval;
    QCheckBox* m_chapterBounds;
    QLabel* m_chapterNote;
    QComboBox* m_contact;
    QLabel* m_note;
    QLabel* m_naming;
    QDialogButtonBox* m_buttons;
};

}  // namespace pd
