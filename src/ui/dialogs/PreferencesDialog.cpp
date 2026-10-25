// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreferencesDialog.h"

#include "ui/SessionController.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QVBoxLayout>

namespace pd {

PreferencesDialog::PreferencesDialog(const AppSettings& s, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Preferences"));
    auto* top = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    m_themeBefore = ThemeManager::instance().current();
    m_theme = new QComboBox(this);
    for (const QString& id : ThemeManager::instance().ids()) {
        m_theme->addItem(ThemeManager::instance().nameOf(id), id);
        if (id == m_themeBefore) m_theme->setCurrentIndex(m_theme->count() - 1);
    }
    // Live preview of the theme while choosing.
    connect(m_theme, &QComboBox::currentIndexChanged, this,
            [this] { ThemeManager::instance().apply(m_theme->currentData().toString()); });
    form->addRow(tr("Theme"), m_theme);
    auto* themeNote = new QLabel(tr("Add your own: copy a .theme file into ~/.config/pingdub/themes/."), this);
    themeNote->setObjectName("panelLabel");
    form->addRow(QString(), themeNote);
    m_start = new QKeySequenceEdit(QKeySequence(s.keyStart), this);
    m_stop = new QKeySequenceEdit(QKeySequence(s.keyStop), this);
    form->addRow(tr("Start capture key"), m_start);
    form->addRow(tr("Stop capture key"), m_stop);
    m_confirm = new QCheckBox(tr("Ask before stopping a capture that has run longer than a minute"), this);
    m_confirm->setChecked(s.confirmStop);
    form->addRow(QString(), m_confirm);
    m_vbi = new QCheckBox(tr("Capture VBI and closed captions when the device has a VBI node"), this);
    m_vbi->setChecked(s.captureVbi);
    form->addRow(QString(), m_vbi);
    m_notify = new QCheckBox(tr("Desktop notification when a capture stops on its own"), this);
    m_notify->setChecked(s.notifyAutoStop);
    m_notify->setToolTip(tr("A stop condition fired, the timer ran out, the device was lost or the disk filled."));
    form->addRow(QString(), m_notify);
    top->addLayout(form);

    auto* lv = new QGroupBox(tr("Crush and clip warnings"), this);
    auto* lf = new QFormLayout(lv);
    m_threshold = new QDoubleSpinBox(lv);
    m_threshold->setRange(0.01, 50.0);
    m_threshold->setDecimals(2);
    m_threshold->setSingleStep(0.1);
    m_threshold->setSuffix(" %");
    m_threshold->setValue(s.levelThresholdPct);
    lf->addRow(tr("Warn when more than"), m_threshold);
    auto* thNote = new QLabel(tr("of active-picture pixels are below 16, above 235, or past the chroma limit"), lv);
    thNote->setObjectName("panelLabel");
    thNote->setWordWrap(true);
    lf->addRow(QString(), thNote);
    m_seconds = new QDoubleSpinBox(lv);
    m_seconds->setRange(0.5, 600.0);
    m_seconds->setDecimals(1);
    m_seconds->setSuffix(" s");
    m_seconds->setValue(s.levelSeconds);
    lf->addRow(tr("for at least"), m_seconds);
    m_chroma = new QSpinBox(lv);
    m_chroma->setRange(60, 180);
    m_chroma->setValue(s.chromaLimit);
    m_chroma->setToolTip(tr("Chroma distance from neutral, in 8-bit codes. 112 is the edge of the 16-240 chroma range; "
                            "75% color bars sit near 90."));
    lf->addRow(tr("Chroma limit"), m_chroma);
    top->addWidget(lv);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
}

void PreferencesDialog::reject() {
    ThemeManager::instance().apply(m_themeBefore);
    QDialog::reject();
}

void PreferencesDialog::store(AppSettings& s) const {
    s.theme = m_theme->currentData().toString();
    if (!m_start->keySequence().isEmpty()) s.keyStart = m_start->keySequence().toString();
    if (!m_stop->keySequence().isEmpty()) s.keyStop = m_stop->keySequence().toString();
    s.confirmStop = m_confirm->isChecked();
    s.captureVbi = m_vbi->isChecked();
    s.notifyAutoStop = m_notify->isChecked();
    s.levelThresholdPct = m_threshold->value();
    s.levelSeconds = m_seconds->value();
    s.chromaLimit = m_chroma->value();
}

}  // namespace pd
