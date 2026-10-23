// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreferencesDialog.h"

#include "ui/SessionController.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
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
    top->addLayout(form);
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
}

}  // namespace pd
