// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputDialog.h"

#include "engine/SysInfo.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pd {

static QWidget* withBrowse(QLineEdit* edit, QWidget* parent) {
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(edit, 1);
    auto* b = new QPushButton(QObject::tr("Browse..."), w);
    QObject::connect(b, &QPushButton::clicked, w, [edit, w] {
        const QString start = QString::fromStdString(templateBase(edit->text().toStdString()));
        const QString d = QFileDialog::getExistingDirectory(w, QObject::tr("Choose folder"), start);
        if (!d.isEmpty()) edit->setText(d);
    });
    h->addWidget(b);
    return w;
}

OutputDialog::OutputDialog(const OutputSettings& o, const TemplateValues& preview, QWidget* parent)
    : QDialog(parent), m_o(o), m_preview(preview) {
    setWindowTitle(tr("Output"));
    auto* top = new QVBoxLayout(this);
    auto* fmt = new QGroupBox(tr("Format"), this);
    auto* ff = new QFormLayout(fmt);
    m_container = new QComboBox(fmt);
    m_container->addItem(tr("Matroska (.mkv)"));
    m_container->addItem(tr("AVI (OpenDML)"));
    m_container->setCurrentIndex(o.container == Container::AVI ? 1 : 0);
    ff->addRow(tr("Container"), m_container);
    m_vcodec = new QComboBox(fmt);
    m_vcodec->addItem(tr("FFV1 v3: smallest lossless files, archival standard"), int(VideoCodec::FFV1));
    m_vcodec->addItem(tr("UT Video: faster, larger files"), int(VideoCodec::UTVideo));
    m_vcodec->addItem(tr("FFVHuff / HuffYUV: VirtualDub classic"), int(VideoCodec::Huff));
    m_vcodec->addItem(tr("Uncompressed: about 75 GB per hour"), int(VideoCodec::Raw));
    m_vcodec->setCurrentIndex(std::max(0, m_vcodec->findData(int(o.vcodec))));
    ff->addRow(tr("Video codec"), m_vcodec);
    m_acodec = new QComboBox(fmt);
    m_acodec->addItem(tr("PCM (as captured)"));
    m_acodec->addItem(tr("FLAC (lossless, about half the size)"));
    m_acodec->setCurrentIndex(o.acodec == AudioCodec::FLAC ? 1 : 0);
    ff->addRow(tr("Audio codec"), m_acodec);
    m_note = new QLabel(fmt);
    m_note->setWordWrap(true);
    m_note->setObjectName("panelLabel");
    ff->addRow(QString(), m_note);
    top->addWidget(fmt);

    auto* files = new QGroupBox(tr("Files"), this);
    auto* fl = new QFormLayout(files);
    m_folder = new QLineEdit(QString::fromStdString(o.folder.empty() ? "~/Videos/PingDub" : o.folder), files);
    m_folder->setToolTip(tr("Tokens: {title} {date} {year} {month} {time} {device} {input} {profile}.\n"
                            "Resolved once when a capture starts. Example: ~/Videos/PingDub/{title}/"));
    fl->addRow(tr("Folder"), withBrowse(m_folder, files));
    m_resolved = new QLabel(files);
    m_resolved->setWordWrap(true);
    m_resolved->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fl->addRow(QString(), m_resolved);
    auto* tokens = new QLabel(tr("Tokens: {title} {date} {year} {month} {time} {device} {input} {profile}"), files);
    tokens->setObjectName("panelLabel");
    tokens->setWordWrap(true);
    fl->addRow(QString(), tokens);
    m_naming = new QLabel(files);
    m_naming->setObjectName("panelLabel");
    m_naming->setWordWrap(true);
    fl->addRow(tr("Naming"), m_naming);
    m_segMode = new QComboBox(files);
    m_segMode->addItem(tr("One file"));
    m_segMode->addItem(tr("Split by size (GB)"));
    m_segMode->addItem(tr("Split by duration (minutes)"));
    m_segValue = new QDoubleSpinBox(files);
    m_segValue->setRange(0.1, 100000);
    if (o.segmentBytes) {
        m_segMode->setCurrentIndex(1);
        m_segValue->setValue(double(o.segmentBytes) / double(1ull << 30));
    } else if (o.segmentSeconds > 0) {
        m_segMode->setCurrentIndex(2);
        m_segValue->setValue(o.segmentSeconds / 60.0);
    } else {
        m_segValue->setValue(50);
    }
    auto* seg = new QWidget(files);
    auto* sh = new QHBoxLayout(seg);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->addWidget(m_segMode);
    sh->addWidget(m_segValue);
    fl->addRow(tr("Segments"), seg);
    auto* segNote = new QLabel(tr("Segments are frame- and sample-continuous: joining them back loses nothing."), files);
    segNote->setObjectName("panelLabel");
    fl->addRow(QString(), segNote);
    m_overflow = new QLineEdit(QString::fromStdString(o.overflowFolder), files);
    m_overflow->setPlaceholderText(tr("none"));
    fl->addRow(tr("Overflow folder"), withBrowse(m_overflow, files));
    m_overflowGb = new QSpinBox(files);
    m_overflowGb->setRange(1, 10000);
    m_overflowGb->setSuffix(" GB");
    m_overflowGb->setValue(int(o.overflowFreeBytes >> 30));
    fl->addRow(tr("Switch when free space drops below"), m_overflowGb);
    top->addWidget(files);

    auto* marks = new QGroupBox(tr("Chapters and contact sheet"), this);
    auto* ml = new QFormLayout(marks);
    m_chapterInterval = new QComboBox(marks);
    m_chapterInterval->addItem(tr("Off"), 0);
    for (int m : {5, 10, 15, 30}) m_chapterInterval->addItem(tr("Every %1 minutes").arg(m), m);
    m_chapterInterval->setCurrentIndex(std::max(0, m_chapterInterval->findData(o.chapterIntervalMin)));
    ml->addRow(tr("Fixed interval"), m_chapterInterval);
    m_chapterBounds = new QCheckBox(tr("A chapter where each recording on the tape starts"), marks);
    m_chapterBounds->setChecked(o.chapterBoundaries);
    m_chapterBounds->setToolTip(tr("A sync loss or blank stretch of 0.5 s or more, then picture again: where someone pressed stop, then record."));
    ml->addRow(tr("Recording boundaries"), m_chapterBounds);
    m_chapterNote = new QLabel(marks);
    m_chapterNote->setObjectName("panelLabel");
    m_chapterNote->setWordWrap(true);
    ml->addRow(QString(), m_chapterNote);
    m_contact = new QComboBox(marks);
    m_contact->addItem(tr("Off"), 0.0);
    for (int s : {10, 15, 30, 60, 120}) m_contact->addItem(tr("A thumbnail every %1 seconds").arg(s), double(s));
    int ci = m_contact->findData(o.contactSheetSec);
    if (ci < 0) {
        m_contact->addItem(tr("A thumbnail every %1 seconds").arg(o.contactSheetSec), o.contactSheetSec);
        ci = m_contact->count() - 1;
    }
    m_contact->setCurrentIndex(ci);
    ml->addRow(tr("Contact sheet"), m_contact);
    top->addWidget(marks);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(m_buttons);
    for (QComboBox* c : {m_container, m_vcodec, m_acodec, m_segMode, m_chapterInterval})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(m_chapterBounds, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(m_folder, &QLineEdit::textChanged, this, [this] { refreshFolder(); });
    connect(m_overflow, &QLineEdit::textChanged, this, [this] { refreshFolder(); });
    refresh();
    refreshFolder();
    resize(600, 0);
}

void OutputDialog::refreshFolder() {
    std::string out, err;
    const std::string tmpl = m_folder->text().trimmed().toStdString();
    bool ok = resolveFolderTemplate(tmpl, m_preview, out, err);
    QString text;
    if (ok) {
        text = tr("Resolves to: %1").arg(QString::fromStdString(out));
        if (templateUsesToken(tmpl, "title") && cleanTemplateValue(m_preview.title).empty())
            text += "\n" + tr("The tape label is empty, so {title} becomes \"Untitled\".");
    } else {
        text = tr("Folder: %1").arg(QString::fromStdString(err));
    }
    const std::string ov = m_overflow->text().trimmed().toStdString();
    if (ok && !ov.empty()) {
        std::string o2, e2;
        if (!resolveFolderTemplate(ov, m_preview, o2, e2)) {
            ok = false;
            text = tr("Overflow folder: %1").arg(QString::fromStdString(e2));
        }
    }
    m_resolved->setText(text);
    m_resolved->setStyleSheet(ok ? QString() : QString("color: %1; font-weight: bold;").arg(StatusColor::Bad.name()));
    // A folder with an unknown token is never saved.
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
}

void OutputDialog::refresh() {
    QStringList notes;
    const bool avi = m_container->currentIndex() == 1;
    if (avi) {
        notes << tr("AVI cannot carry field order reliably; PingDub writes it to the sidecar instead.");
        if (m_acodec->currentIndex() == 1) notes << tr("FLAC does not go in AVI; PCM will be used.");
    }
    if (m_vcodec->currentData().toInt() == int(VideoCodec::FFV1))
        notes << tr("FFV1 level 3, one frame per GOP, range coder with the larger context model, slice CRCs so damage is detectable.");
    if (m_vcodec->currentData().toInt() == int(VideoCodec::UTVideo))
        notes << tr("UT Video cannot store 10-bit; a 10-bit source needs FFV1.");
    m_note->setText(notes.join("\n"));
    m_segValue->setEnabled(m_segMode->currentIndex() != 0);
    const QString ext = avi ? ".avi" : ".mkv";
    m_naming->setText(tr("YYYY-MM-DD_HHMMSS_<tape label>%1, sidecars next to it").arg(ext));
    const bool chapters = m_chapterInterval->currentData().toInt() > 0 || m_chapterBounds->isChecked();
    if (!chapters) m_chapterNote->setText(tr("No chapters."));
    else if (avi) m_chapterNote->setText(tr("AVI has no chapters; they go in a .chapters.txt sidecar in FFmpeg metadata format."));
    else m_chapterNote->setText(tr("Matroska chapters: the file stays one master, and players can jump between them."));
}

OutputSettings OutputDialog::result() const {
    OutputSettings o = m_o;
    o.container = m_container->currentIndex() == 1 ? Container::AVI : Container::MKV;
    o.vcodec = VideoCodec(m_vcodec->currentData().toInt());
    o.acodec = m_acodec->currentIndex() == 1 ? AudioCodec::FLAC : AudioCodec::PCM;
    o.folder = m_folder->text().trimmed().toStdString();
    o.segmentBytes = m_segMode->currentIndex() == 1 ? uint64_t(m_segValue->value() * double(1ull << 30)) : 0;
    o.segmentSeconds = m_segMode->currentIndex() == 2 ? m_segValue->value() * 60.0 : 0;
    o.overflowFolder = m_overflow->text().trimmed().toStdString();
    o.overflowFreeBytes = uint64_t(m_overflowGb->value()) << 30;
    o.chapterIntervalMin = m_chapterInterval->currentData().toInt();
    o.chapterBoundaries = m_chapterBounds->isChecked();
    o.contactSheetSec = m_contact->currentData().toDouble();
    return o;
}

}  // namespace pd
