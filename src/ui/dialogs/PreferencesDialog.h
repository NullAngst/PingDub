// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

class QComboBox;
class QKeySequenceEdit;
class QCheckBox;

namespace pd {

struct AppSettings;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(const AppSettings& s, QWidget* parent = nullptr);
    void store(AppSettings& s) const;

protected:
    void reject() override;

private:
    QString m_themeBefore;
    QComboBox* m_theme;
    QKeySequenceEdit* m_start;
    QKeySequenceEdit* m_stop;
    QCheckBox* m_confirm;
    QCheckBox* m_vbi;
};

}  // namespace pd
