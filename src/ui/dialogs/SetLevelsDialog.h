// SPDX-License-Identifier: GPL-3.0-or-later
// Set Levels: shows the suggested brightness, contrast and saturation next to
// the current ones, applies them when approved, measures 3 seconds of live
// video and corrects up to twice. Preview only; a capture never adjusts itself.
#pragma once

#include "engine/Levels.h"

#include <QDialog>
#include <QMap>
#include <QTimer>

class QLabel;
class QPushButton;
class QProgressBar;
class QTableWidget;

namespace pd {

class SessionController;

class SetLevelsDialog : public QDialog {
    Q_OBJECT
public:
    SetLevelsDialog(SessionController* ctl, const LevelReading& reading, QWidget* parent = nullptr);
    // Control id -> value before anything changed, for Revert. Empty if nothing changed.
    QMap<quint32, qint64> before() const { return m_before; }
    bool changed() const { return m_changed; }
    // The dialog only raised brightness or lowered contrast; the panel keeps measuring.
    bool remeasure() const { return m_remeasure; }

private:
    void showProposal(const LevelProposal& p);
    void apply(const LevelProposal& p);
    void startCheck();
    void finishCheck();
    void conclude(const QString& text, bool ok);
    void revert();
    void fillTable(const LevelProposal& p);

    SessionController* m_ctl;
    LevelSolver m_solver;
    ProcAmp m_start;
    LevelProposal m_current;
    QMap<quint32, qint64> m_before;
    bool m_changed = false;
    bool m_remeasure = false;
    bool m_checking = false;
    QTimer m_timer;
    QLabel* m_reading;
    QLabel* m_message;
    QTableWidget* m_table;
    QProgressBar* m_progress;
    QPushButton* m_apply;
    QPushButton* m_cancel;
    QPushButton* m_revert;
    QPushButton* m_keep;
};

}  // namespace pd
