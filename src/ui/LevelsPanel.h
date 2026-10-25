// SPDX-License-Identifier: GPL-3.0-or-later
// Level Measure and Set Levels in the bottom panel. Measure collects while
// the tape plays (fast-forward and play elsewhere as much as you like; the
// measurement accumulates), Set Levels suggests values from it, Revert puts
// back what was there before. Disabled while capturing.
#pragma once

#include <QMap>
#include <QWidget>

class QLabel;
class QPushButton;

namespace pd {

class SessionController;

class LevelsPanel : public QWidget {
    Q_OBJECT
public:
    explicit LevelsPanel(SessionController* ctl, QWidget* parent = nullptr);
    void refresh();
    void setCapturing(bool on);
    void toggleMeasure();
    void openSetLevels();
    void revert();
    bool canRevert() const { return !m_revert.isEmpty(); }

signals:
    void message(const QString& text);

private:
    SessionController* m_ctl;
    QPushButton* m_measureBtn;
    QPushButton* m_setBtn;
    QPushButton* m_revertBtn;
    QLabel* m_time;
    QLabel* m_black;
    QLabel* m_white;
    QLabel* m_sat;
    QMap<quint32, qint64> m_revert;
    bool m_capturing = false;
};

}  // namespace pd
