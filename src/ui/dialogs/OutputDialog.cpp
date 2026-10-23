// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputDialog.h"

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
        const QString d = QFileDialog::getExistingDirectory(w, QObject::tr("Choose folder"), edit->text());
        if (!d.isEmpty()) edit->setText(d);
    });
    h->addWidget(b);
    return w;
}

OutputDialog::OutputDialog(const OutputSettings& o, QWidget* parent) : QDialog(parent), m_o(o) {
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
    fl->addRow(tr("Folder"), withBrowse(m_folder, files));
    m_naming = new QLabel(tr("YYYY-MM-DD_HHMMSS_<tape label>.mkv, with .log.csv and .json sidecars next to it"), files);
    m_naming->setObjectName("panelLabel");
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

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
    for (QComboBox* c : {m_container, m_vcodec, m_acodec, m_segMode})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    refresh();
    resize(560, 0);
}

void OutputDialog::refresh() {
    QStringList notes;
    if (m_container->currentIndex() == 1) {
        notes << tr("AVI cannot carry field order reliably; PingDub writes it to the sidecar instead.");
        if (m_acodec->currentIndex() == 1) notes << tr("FLAC does not go in AVI; PCM will be used.");
    }
    if (m_vcodec->currentData().toInt() == int(VideoCodec::FFV1))
        notes << tr("FFV1 level 3, one frame per GOP, range coder with the larger context model, slice CRCs so damage is detectable.");
    if (m_vcodec->currentData().toInt() == int(VideoCodec::UTVideo))
        notes << tr("UT Video cannot store 10-bit; a 10-bit source needs FFV1.");
    m_note->setText(notes.join("\n"));
    m_segValue->setEnabled(m_segMode->currentIndex() != 0);
    m_naming->setText(m_container->currentIndex() == 1 ? tr("YYYY-MM-DD_HHMMSS_<tape label>.avi, with .log.csv and .json sidecars")
                                                       : tr("YYYY-MM-DD_HHMMSS_<tape label>.mkv, with .log.csv and .json sidecars"));
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
    return o;
}

}  // namespace pd
