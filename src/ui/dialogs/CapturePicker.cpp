// SPDX-License-Identifier: GPL-3.0-or-later
#include "CapturePicker.h"

#include "engine/FolderTemplate.h"
#include "ui/StatsStrip.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace pd {

CapturePicker::CapturePicker(const QString& title, const QString& baseFolder, QWidget* parent)
    : QDialog(parent), m_base(baseFolder) {
    setWindowTitle(title);
    auto* top = new QVBoxLayout(this);
    auto* where = new QLabel(tr("Captures in %1 and the folders below it, newest first:").arg(baseFolder), this);
    where->setWordWrap(true);
    top->addWidget(where);
    m_list = new QListWidget(this);
    const QDir base(baseFolder);
    for (const std::string& p : findCaptures(baseFolder.toStdString())) {
        const QString path = QString::fromStdString(p);
        const QFileInfo fi(path);
        auto* it = new QListWidgetItem(QStringLiteral("%1    %2, %3")
                                           .arg(base.relativeFilePath(path), humanBytes(double(fi.size())),
                                                fi.lastModified().toString("yyyy-MM-dd HH:mm")),
                                       m_list);
        it->setData(Qt::UserRole, path);
    }
    if (m_list->count() == 0) top->addWidget(new QLabel(tr("None found. Use Browse to pick a file elsewhere."), this));
    else m_list->setCurrentRow(0);
    top->addWidget(m_list, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    QPushButton* browse = bb->addButton(tr("Browse..."), QDialogButtonBox::ActionRole);
    top->addWidget(bb);
    auto accept1 = [this] {
        if (QListWidgetItem* it = m_list->currentItem()) {
            m_selected = it->data(Qt::UserRole).toString();
            accept();
        }
    };
    connect(bb, &QDialogButtonBox::accepted, this, accept1);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::itemDoubleClicked, this, accept1);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, windowTitle(), m_base, tr("Captures (*.mkv *.avi)"));
        if (!f.isEmpty()) {
            m_selected = f;
            accept();
        }
    });
    resize(640, 420);
}

QString CapturePicker::pick(QWidget* parent, const QString& title, const QString& baseFolder) {
    CapturePicker d(title, baseFolder, parent);
    return d.exec() == QDialog::Accepted ? d.selected() : QString();
}

}  // namespace pd
