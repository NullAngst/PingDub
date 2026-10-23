// SPDX-License-Identifier: GPL-3.0-or-later
#include "TimingDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pd {

TimingDialog::TimingDialog(const TimingSettings& t, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Timing"));
    auto* top = new QVBoxLayout(this);
    auto* sync = new QGroupBox(tr("Sync mode"), this);
    auto* sl = new QVBoxLayout(sync);
    m_video = new QRadioButton(tr("Video master (recommended): every frame kept, audio resampled by a few ppm to fit"), sync);
    m_audio = new QRadioButton(tr("Audio master: audio bit-exact, frames repeated or dropped to match its length"), sync);
    m_none = new QRadioButton(tr("No correction: frames and samples as delivered (diagnosing a device; streams will drift)"), sync);
    for (auto* r : {m_video, m_audio, m_none}) sl->addWidget(r);
    (t.sync == SyncMode::AudioMaster ? m_audio : t.sync == SyncMode::NoCorrection ? m_none : m_video)->setChecked(true);
    top->addWidget(sync);

    auto* box = new QGroupBox(tr("Buffers"), this);
    auto* form = new QFormLayout(box);
    m_gap = new QComboBox(box);
    m_gap->addItem(tr("Repeat the last frame"));
    m_gap->addItem(tr("Black"));
    m_gap->setCurrentIndex(t.gapFill == GapFill::Black ? 1 : 0);
    form->addRow(tr("Fill gaps with"), m_gap);
    m_buffers = new QSpinBox(box);
    m_buffers->setRange(8, 64);
    m_buffers->setValue(t.v4l2Buffers);
    m_buffers->setToolTip(tr("Driver buffers. More absorbs longer scheduling stalls; takes effect when the device reopens."));
    form->addRow(tr("V4L2 buffers"), m_buffers);
    m_ring = new QSpinBox(box);
    m_ring->setRange(256, 16384);
    m_ring->setSingleStep(256);
    m_ring->setSuffix(" MB");
    m_ring->setValue(int(t.ringBytes >> 20));
    m_ring->setToolTip(tr("RAM between capture and encoder. 2 GB holds about 100 seconds of raw SD video."));
    form->addRow(tr("RAM buffer"), m_ring);
    top->addWidget(box);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
}

TimingSettings TimingDialog::result() const {
    TimingSettings t;
    t.sync = m_audio->isChecked() ? SyncMode::AudioMaster : m_none->isChecked() ? SyncMode::NoCorrection : SyncMode::VideoMaster;
    t.gapFill = m_gap->currentIndex() == 1 ? GapFill::Black : GapFill::LastFrame;
    t.v4l2Buffers = m_buffers->value();
    t.ringBytes = uint64_t(m_ring->value()) << 20;
    return t;
}

}  // namespace pd
