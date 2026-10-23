// SPDX-License-Identifier: GPL-3.0-or-later
#include "TimedCaptureDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace pd {

TimedCaptureDialog::TimedCaptureDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Timed Capture"));
    auto* top = new QVBoxLayout(this);
    auto* start = new QGroupBox(tr("Start"), this);
    auto* sg = new QGridLayout(start);
    m_now = new QRadioButton(tr("Now"), start);
    m_at = new QRadioButton(tr("At"), start);
    m_after = new QRadioButton(tr("After"), start);
    m_now->setChecked(true);
    m_time = new QTimeEdit(QTime::currentTime().addSecs(600), start);
    m_time->setDisplayFormat("HH:mm");
    m_delay = new QSpinBox(start);
    m_delay->setRange(1, 24 * 60);
    m_delay->setValue(5);
    m_delay->setSuffix(tr(" min"));
    sg->addWidget(m_now, 0, 0);
    sg->addWidget(m_at, 1, 0);
    sg->addWidget(m_time, 1, 1);
    sg->addWidget(m_after, 2, 0);
    sg->addWidget(m_delay, 2, 1);
    top->addWidget(start);

    auto* stop = new QGroupBox(tr("Stop"), this);
    auto* tg = new QGridLayout(stop);
    m_stop = new QCheckBox(tr("Stop after"), stop);
    m_stop->setChecked(true);
    m_preset = new QComboBox(stop);
    for (int m : {30, 60, 120, 160, 240, 360}) m_preset->addItem(tr("%1 minutes (T-%1)").arg(m), m);
    m_preset->addItem(tr("Custom"), 0);
    m_preset->setCurrentIndex(2);
    m_minutes = new QSpinBox(stop);
    m_minutes->setRange(1, 24 * 60);
    m_minutes->setValue(120);
    m_minutes->setSuffix(tr(" min"));
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this] {
        const int m = m_preset->currentData().toInt();
        if (m) m_minutes->setValue(m);
        m_minutes->setEnabled(m == 0);
    });
    m_minutes->setEnabled(false);
    tg->addWidget(m_stop, 0, 0);
    tg->addWidget(m_preset, 0, 1);
    tg->addWidget(m_minutes, 1, 1);
    top->addWidget(stop);
    auto* note = new QLabel(tr("The file ends at exactly this length; other stop conditions still apply."), this);
    note->setObjectName("panelLabel");
    top->addWidget(note);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Arm"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
}

TimedCapture TimedCaptureDialog::result() const {
    TimedCapture t;
    if (m_at->isChecked()) {
        QDateTime when(QDate::currentDate(), m_time->time());
        if (when < QDateTime::currentDateTime()) when = when.addDays(1);
        t.startAt = when;
    } else if (m_after->isChecked()) {
        t.startAt = QDateTime::currentDateTime().addSecs(qint64(m_delay->value()) * 60);
    }
    t.durationSec = m_stop->isChecked() ? m_minutes->value() * 60.0 : 0;
    return t;
}

}  // namespace pd
