// SPDX-License-Identifier: GPL-3.0-or-later
#include "LevelsPanel.h"

#include "SessionController.h"
#include "Theme.h"
#include "dialogs/SetLevelsDialog.h"

#include <QFontMetrics>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>

#include <cstdlib>

namespace pd {

LevelsPanel::LevelsPanel(SessionController* ctl, QWidget* parent) : QWidget(parent), m_ctl(ctl) {
    auto* g = new QGridLayout(this);
    g->setContentsMargins(4, 0, 4, 0);
    g->setVerticalSpacing(2);
    m_measureBtn = new QPushButton(tr("Measure Levels"), this);
    m_measureBtn->setCheckable(true);
    m_measureBtn->setToolTip(tr("Play the tape and let it collect. Fast-forward to other parts and play again if you like: "
                                "the measurement adds up. Frames without signal lock are left out on their own."));
    m_setBtn = new QPushButton(tr("Set Levels..."), this);
    m_setBtn->setToolTip(tr("Suggests brightness, contrast and saturation from the measurement, applies them when you "
                            "approve, and checks them on a few seconds of live video. Keep the tape playing."));
    m_revertBtn = new QPushButton(tr("Revert"), this);
    m_revertBtn->setToolTip(tr("Puts back the values from before the last Set Levels."));
    m_time = new QLabel(tr("not measuring"), this);
    m_black = new QLabel(this);
    m_white = new QLabel(this);
    m_sat = new QLabel(this);
    for (QLabel* l : {m_time, m_black, m_white, m_sat}) {
        QFont f = l->font();
        f.setFamily("monospace");
        f.setStyleHint(QFont::Monospace);
        l->setFont(f);
        // Fixed width, so the bottom panel does not shift as the numbers change.
        l->setMinimumWidth(QFontMetrics(f).horizontalAdvance(QStringLiteral("measuring 9999.9 s, 99999 skipped")));
    }
    g->addWidget(m_measureBtn, 0, 0);
    g->addWidget(m_setBtn, 1, 0);
    g->addWidget(m_revertBtn, 2, 0);
    g->addWidget(m_time, 0, 1);
    g->addWidget(m_black, 1, 1);
    g->addWidget(m_white, 2, 1);
    g->addWidget(m_sat, 3, 1);
    g->setRowStretch(4, 1);
    connect(m_measureBtn, &QPushButton::clicked, this, [this] { toggleMeasure(); });
    connect(m_setBtn, &QPushButton::clicked, this, [this] { openSetLevels(); });
    connect(m_revertBtn, &QPushButton::clicked, this, [this] { revert(); });
    refresh();
}

void LevelsPanel::toggleMeasure() {
    CaptureSession& s = m_ctl->session();
    if (s.levelMeasuring()) {
        s.stopLevelMeasure();
    } else {
        std::string err;
        if (!s.startLevelMeasure(err)) emit message(QString::fromStdString(err));
    }
    refresh();
}

void LevelsPanel::openSetLevels() {
    CaptureSession& s = m_ctl->session();
    if (m_capturing || !s.isOpen()) return;
    const LevelReading r = s.levelReading();
    if (!r.valid()) {
        QMessageBox::information(this, tr("Set Levels"),
                                 tr("Nothing measured yet. Play the tape, click Measure Levels, let it run, then Set Levels."));
        return;
    }
    SetLevelsDialog d(m_ctl, r, this);
    d.exec();
    if (d.changed()) m_revert = d.before();
    if (d.remeasure()) emit message(tr("Measuring again with the new value. Click Set Levels in a few seconds."));
    refresh();
}

void LevelsPanel::revert() {
    if (m_revert.isEmpty() || m_capturing) return;
    std::string err;
    for (auto it = m_revert.begin(); it != m_revert.end(); ++it) m_ctl->session().setControl(it.key(), it.value(), err);
    m_ctl->saveControlsToProfile();
    m_ctl->session().restartLevelMeasure(300);
    m_revert.clear();
    emit message(tr("Levels reverted to the values before Set Levels."));
    refresh();
}

void LevelsPanel::setCapturing(bool on) {
    if (on == m_capturing) return;
    m_capturing = on;
    if (on) m_ctl->session().stopLevelMeasure();
    refresh();
}

void LevelsPanel::refresh() {
    CaptureSession& s = m_ctl->session();
    const bool open = s.isOpen();
    const bool measuring = s.levelMeasuring();
    m_measureBtn->setEnabled(open && !m_capturing);
    m_measureBtn->setChecked(measuring);
    m_measureBtn->setText(measuring ? tr("Stop measuring") : tr("Measure Levels"));
    m_setBtn->setEnabled(open && !m_capturing);
    m_revertBtn->setEnabled(open && !m_capturing && !m_revert.isEmpty());
    const QString why = tr("Levels are set in preview only, never during a capture.");
    for (QPushButton* b : {m_measureBtn, m_setBtn, m_revertBtn}) b->setStatusTip(m_capturing ? why : QString());
    if (m_capturing) {
        m_time->setText(tr("locked during capture"));
        m_black->clear();
        m_white->clear();
        m_sat->clear();
        return;
    }
    const LevelReading r = s.levelReading();
    if (!measuring && !r.valid()) {
        m_time->setText(tr("not measuring"));
        m_black->clear();
        m_white->clear();
        m_sat->clear();
        return;
    }
    m_time->setText(tr("%1 %2 s, %3 skipped").arg(measuring ? tr("measuring") : tr("measured")).arg(r.seconds, 0, 'f', 1).arg(r.skipped));
    auto style = [](QLabel* l, int value, int target, int tol) {
        const int off = std::abs(value - target);
        l->setStyleSheet(value < 0 || off <= tol ? QString()
                                                 : QString("color: %1; font-weight: bold;").arg((off > 3 * tol ? StatusColor::Bad : StatusColor::Warn).name()));
    };
    m_black->setText(r.black >= 0 ? tr("black %1 / 16%2").arg(r.black, 3).arg(r.blackClipped ? tr(" crushed") : QString()) : tr("black -"));
    m_white->setText(r.white >= 0 ? tr("white %1 / 235%2").arg(r.white, 3).arg(r.whiteClipped ? tr(" clipped") : QString()) : tr("white -"));
    style(m_black, r.black, 16, 2);
    style(m_white, r.white, 235, 3);
    const int lim = m_ctl->settings().chromaLimit;
    m_sat->setText(r.peakSat >= 0 ? tr("sat   %1 / %2").arg(r.peakSat, 3).arg(lim) : tr("sat   -"));
    m_sat->setStyleSheet(r.peakSat > lim ? QString("color: %1; font-weight: bold;").arg(StatusColor::Warn.name()) : QString());
}

}  // namespace pd
