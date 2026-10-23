// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreflightDialog.h"

#include "ui/Theme.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace pd {

bool PreflightDialog::needsAttention(const std::vector<PreflightItem>& items) {
    for (const auto& i : items)
        if (i.level != PreflightItem::Ok) return true;
    return false;
}

PreflightDialog::PreflightDialog(const std::vector<PreflightItem>& items, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Before capturing"));
    auto* top = new QVBoxLayout(this);
    for (const PreflightItem& i : items) {
        auto* row = new QHBoxLayout;
        auto* dot = new QLabel(QString::fromUtf8("●"), this);
        const QColor c = i.level == PreflightItem::Ok ? ThemeManager::instance().okColor()
                         : i.level == PreflightItem::Warn ? StatusColor::Warn
                                                          : StatusColor::Bad;
        dot->setStyleSheet(QString("color: %1;").arg(c.name()));
        auto* text = new QLabel(QString::fromStdString(i.text), this);
        text->setWordWrap(true);
        text->setMinimumWidth(460);
        row->addWidget(dot);
        row->addWidget(text, 1);
        top->addLayout(row);
    }
    auto* bb = new QDialogButtonBox(this);
    bb->addButton(tr("Capture anyway"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
    top->setSizeConstraint(QLayout::SetMinimumSize);
}

}  // namespace pd
