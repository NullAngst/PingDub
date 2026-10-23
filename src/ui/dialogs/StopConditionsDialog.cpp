// SPDX-License-Identifier: GPL-3.0-or-later
#include "StopConditionsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace pd {

StopConditionsDialog::StopConditionsDialog(const StopConditions& s, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Stop Conditions"));
    auto* top = new QVBoxLayout(this);
    auto* g = new QGridLayout;
    int row = 0;
    auto add = [&](const QString& text, double value, bool on, double lo, double hi, const QString& suffix) {
        Row r;
        r.on = new QCheckBox(text, this);
        r.value = new QDoubleSpinBox(this);
        r.value->setRange(lo, hi);
        r.value->setDecimals(1);
        r.value->setSuffix(suffix);
        r.value->setValue(value);
        r.on->setChecked(on);
        r.value->setEnabled(on);
        connect(r.on, &QCheckBox::toggled, r.value, &QWidget::setEnabled);
        g->addWidget(r.on, row, 0);
        g->addWidget(r.value, row, 1);
        ++row;
        return r;
    };
    m_duration = add(tr("Duration reached"), s.durationSec > 0 ? s.durationSec / 60.0 : 120, s.durationSec > 0, 0.1, 10000, tr(" min"));
    m_size = add(tr("File size reached"), s.fileBytes ? double(s.fileBytes) / double(1ull << 30) : 100, s.fileBytes > 0, 0.1, 100000, " GB");
    m_disk = add(tr("Free disk below (and no overflow folder left)"), s.minFreeBytes ? double(s.minFreeBytes) / double(1ull << 30) : 2,
                 s.minFreeBytes > 0, 0.1, 10000, " GB");
    m_drops = add(tr("Dropped/inserted frames per minute above"), s.maxDropPerMin > 0 ? s.maxDropPerMin : 30, s.maxDropPerMin > 0, 1,
                  100000, "");
    m_signal = add(tr("Signal lost for (end of tape, decks that output nothing)"), s.signalLostSec > 0 ? s.signalLostSec : 10,
                   s.signalLostSec > 0, 1, 3600, " s");
    m_static = add(tr("Static picture for (end of tape, blue or black screen)"), s.staticSec > 0 ? s.staticSec : 15, s.staticSec > 0, 1,
                   3600, " s");
    g->addWidget(new QLabel(tr("Static means luma standard deviation below"), this), row, 0);
    m_staticLevel = new QDoubleSpinBox(this);
    m_staticLevel->setRange(0.1, 50);
    m_staticLevel->setValue(s.staticStddev);
    g->addWidget(m_staticLevel, row, 1);
    top->addLayout(g);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
}

StopConditions StopConditionsDialog::result() const {
    StopConditions s;
    s.durationSec = m_duration.on->isChecked() ? m_duration.value->value() * 60.0 : 0;
    s.fileBytes = m_size.on->isChecked() ? uint64_t(m_size.value->value() * double(1ull << 30)) : 0;
    s.minFreeBytes = m_disk.on->isChecked() ? uint64_t(m_disk.value->value() * double(1ull << 30)) : 0;
    s.maxDropPerMin = m_drops.on->isChecked() ? m_drops.value->value() : 0;
    s.signalLostSec = m_signal.on->isChecked() ? m_signal.value->value() : 0;
    s.staticSec = m_static.on->isChecked() ? m_static.value->value() : 0;
    s.staticStddev = m_staticLevel->value();
    return s;
}

}  // namespace pd
