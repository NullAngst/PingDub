// SPDX-License-Identifier: GPL-3.0-or-later
#include "SetLevelsDialog.h"

#include "ui/SessionController.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <utility>

namespace pd {

namespace {

QString readingText(const LevelReading& r, int chromaLimit) {
    if (!r.valid()) return QObject::tr("No usable frames measured.");
    QString s = QObject::tr("Measured %1 s: black %2 (target 16), white %3 (target 235)")
                    .arg(r.seconds, 0, 'f', 1)
                    .arg(r.black)
                    .arg(r.white);
    if (r.peakSat >= 0) s += QObject::tr(", peak saturation %1 (limit %2)").arg(r.peakSat).arg(chromaLimit);
    return s + ".";
}

}  // namespace

SetLevelsDialog::SetLevelsDialog(SessionController* ctl, const LevelReading& reading, QWidget* parent)
    : QDialog(parent), m_ctl(ctl) {
    setWindowTitle(tr("Set Levels"));
    auto* top = new QVBoxLayout(this);
    m_reading = new QLabel(this);
    m_reading->setWordWrap(true);
    top->addWidget(m_reading);
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({tr("Control"), tr("Now"), tr("Suggested")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    top->addWidget(m_table);
    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    top->addWidget(m_message);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->hide();
    top->addWidget(m_progress);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    m_revert = new QPushButton(tr("Revert"), this);
    m_keep = new QPushButton(tr("Keep"), this);
    m_apply = new QPushButton(tr("Apply"), this);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_apply->setDefault(true);
    for (QPushButton* b : {m_revert, m_keep, m_apply, m_cancel}) row->addWidget(b);
    top->addLayout(row);
    m_revert->hide();
    m_keep->hide();
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_timer.stop();
        if (m_checking) revert();
        reject();
    });
    connect(m_revert, &QPushButton::clicked, this, [this] {
        revert();
        accept();
    });
    connect(m_keep, &QPushButton::clicked, this, [this] {
        // The values go into the profile for this device and input.
        m_ctl->saveControlsToProfile();
        accept();
    });
    connect(m_apply, &QPushButton::clicked, this, [this] {
        const LevelProposal p = m_current;
        apply(p);
        if (p.kind == LevelProposal::Kind::RaiseBrightness || p.kind == LevelProposal::Kind::LowerContrast) {
            m_remeasure = true;
            m_ctl->session().restartLevelMeasure(300);
            m_ctl->saveControlsToProfile();
            accept();
            return;
        }
        startCheck();
    });
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &SetLevelsDialog::finishCheck);

    m_start = ctl->session().procAmp();
    for (const ProcControl* c : {&m_start.brightness, &m_start.contrast, &m_start.saturation})
        if (c->present) m_before[c->id] = c->value;
    LevelTargets t;
    t.chroma = ctl->settings().chromaLimit;
    m_solver.begin(m_start, t);
    m_reading->setText(readingText(reading, t.chroma));
    showProposal(m_solver.propose(reading));
    resize(520, 0);
}

static void fitRows(QTableWidget* t) {
    int h = t->horizontalHeader()->height() + 2 * t->frameWidth();
    for (int r = 0; r < t->rowCount(); ++r) h += t->rowHeight(r);
    t->setFixedHeight(h);
}

void SetLevelsDialog::fillTable(const LevelProposal& p) {
    m_table->setRowCount(0);
    const ProcAmp& now = m_solver.current();
    struct R {
        const ProcControl* c;
        int64_t next;
        bool changed;
    };
    for (R r : {R{&now.brightness, p.brightness, p.brightnessChanged}, R{&now.contrast, p.contrast, p.contrastChanged},
                R{&now.saturation, p.saturation, p.saturationChanged}}) {
        if (!r.c->present) continue;
        const int i = m_table->rowCount();
        m_table->insertRow(i);
        m_table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(r.c->name)));
        m_table->setItem(i, 1, new QTableWidgetItem(QString::number(r.c->value)));
        auto* n = new QTableWidgetItem(r.changed ? QString::number(r.next) : tr("unchanged"));
        if (r.changed) {
            QFont f = n->font();
            f.setBold(true);
            n->setFont(f);
        }
        m_table->setItem(i, 2, n);
    }
    fitRows(m_table);
}

void SetLevelsDialog::showProposal(const LevelProposal& p) {
    m_current = p;
    fillTable(p);
    QString msg = QString::fromStdString(p.message);
    m_apply->setText(tr("Apply"));
    switch (p.kind) {
    case LevelProposal::Kind::Apply:
        if (p.narrow) {
            msg = tr("The sample never got near both black and white (a dark scene only?), so the stretch is a guess. "
                     "Measure a section with both, or apply anyway.\n") + msg;
            m_apply->setText(tr("Apply anyway"));
        }
        m_apply->setEnabled(true);
        break;
    case LevelProposal::Kind::RaiseBrightness:
    case LevelProposal::Kind::LowerContrast:
        m_apply->setText(tr("Apply and keep measuring"));
        m_apply->setEnabled(p.brightnessChanged || p.contrastChanged);
        break;
    default:
        m_apply->setEnabled(false);
        m_table->setVisible(p.kind != LevelProposal::Kind::NoControls);
        m_cancel->setText(tr("Close"));
        break;
    }
    m_message->setText(msg);
}

void SetLevelsDialog::apply(const LevelProposal& p) {
    CaptureSession& s = m_ctl->session();
    std::string err;
    const ProcAmp& cur = m_solver.current();
    if (cur.brightness.present && p.brightness != cur.brightness.value) s.setControl(cur.brightness.id, p.brightness, err);
    if (cur.contrast.present && p.contrast != cur.contrast.value) s.setControl(cur.contrast.id, p.contrast, err);
    if (cur.saturation.present && p.saturation != cur.saturation.value) s.setControl(cur.saturation.id, p.saturation, err);
    m_solver.applied(p);
    m_changed = true;
}

void SetLevelsDialog::startCheck() {
    m_checking = true;
    m_apply->hide();
    m_progress->show();
    m_message->setText(tr("Checking: measuring 3 seconds of live video. Keep the tape playing.%1")
                           .arg(m_solver.corrections() ? tr(" (correction %1 of %2)").arg(m_solver.corrections()).arg(LevelSolver::kMaxCorrections)
                                                       : QString()));
    std::string err;
    // Frames still in flight from before the change are skipped.
    m_ctl->session().startLevelMeasure(err);
    m_ctl->session().restartLevelMeasure(300);
    m_timer.start(3300);
}

void SetLevelsDialog::finishCheck() {
    const LevelReading r = m_ctl->session().levelReading();
    m_reading->setText(readingText(r, m_ctl->settings().chromaLimit));
    if (r.frames < 15) {
        conclude(tr("Not enough locked frames in the last 3 seconds. Is the tape still playing?"), false);
        return;
    }
    if (m_solver.converged(r)) {
        conclude(tr("On target: black %1, white %2.").arg(r.black).arg(r.white), true);
        return;
    }
    if (m_solver.corrections() >= LevelSolver::kMaxCorrections) {
        conclude(tr("Closest after %1 corrections: black %2, white %3. The controls may not reach further on this device.")
                     .arg(m_solver.corrections())
                     .arg(r.black)
                     .arg(r.white),
                 false);
        return;
    }
    const LevelProposal p = m_solver.correct(r);
    fillTable(p);
    if (p.kind == LevelProposal::Kind::Done) {
        conclude(QString::fromStdString(p.message) + tr(" Black %1, white %2.").arg(r.black).arg(r.white), false);
        return;
    }
    apply(p);
    startCheck();
}

void SetLevelsDialog::conclude(const QString& text, bool ok) {
    m_checking = false;
    m_progress->hide();
    m_message->setText(text);
    m_message->setStyleSheet(ok ? QString("color: %1;").arg(StatusColor::Ok.name()) : QString());
    m_cancel->hide();
    m_revert->show();
    m_keep->show();
    m_keep->setDefault(true);
    // Before and after, so Keep or Revert is an informed choice.
    m_table->setRowCount(0);
    m_table->setHorizontalHeaderLabels({tr("Control"), tr("Before"), tr("Now")});
    const ProcAmp& now = m_solver.current();
    for (auto pr : {std::make_pair(&m_start.brightness, &now.brightness), std::make_pair(&m_start.contrast, &now.contrast),
                    std::make_pair(&m_start.saturation, &now.saturation)}) {
        if (!pr.first->present) continue;
        const int i = m_table->rowCount();
        m_table->insertRow(i);
        m_table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(pr.first->name)));
        m_table->setItem(i, 1, new QTableWidgetItem(QString::number(pr.first->value)));
        m_table->setItem(i, 2, new QTableWidgetItem(QString::number(pr.second->value)));
    }
    fitRows(m_table);
}

void SetLevelsDialog::revert() {
    std::string err;
    for (auto it = m_before.begin(); it != m_before.end(); ++it) m_ctl->session().setControl(it.key(), it.value(), err);
    m_changed = false;
    m_ctl->session().restartLevelMeasure(300);
}

}  // namespace pd
