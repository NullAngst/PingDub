// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

class QPlainTextEdit;

namespace pd {

class SessionController;

// Detected hardware with IDs, bound and missing drivers, ALSA cards, group
// membership, a full V4L2 capability dump, and a Copy button for bug reports.
class DiagnosticsDialog : public QDialog {
    Q_OBJECT
public:
    DiagnosticsDialog(SessionController* ctl, QWidget* parent = nullptr);
    static QString report(SessionController* ctl);

private:
    void refresh();
    SessionController* m_ctl;
    QPlainTextEdit* m_text;
};

}  // namespace pd
