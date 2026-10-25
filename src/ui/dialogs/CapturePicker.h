// SPDX-License-Identifier: GPL-3.0-or-later
// Picks a capture: every .mkv and .avi under the base output folder (the
// part before the first token) and every folder below it, newest first, with
// Browse for anything elsewhere. Used by Recover, Verify and Open report.
#pragma once

#include <QDialog>

class QListWidget;

namespace pd {

class CapturePicker : public QDialog {
    Q_OBJECT
public:
    CapturePicker(const QString& title, const QString& baseFolder, QWidget* parent = nullptr);
    QString selected() const { return m_selected; }
    // Runs the dialog; empty if cancelled.
    static QString pick(QWidget* parent, const QString& title, const QString& baseFolder);

private:
    QListWidget* m_list;
    QString m_base;
    QString m_selected;
};

}  // namespace pd
