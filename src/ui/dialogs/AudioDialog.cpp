// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioDialog.h"

#include "ui/Meters.h"
#include "ui/SessionController.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace pd {

AudioDialog::AudioDialog(SessionController* ctl, QWidget* parent) : QDialog(parent), m_ctl(ctl) {
    setWindowTitle(tr("Audio"));
    m_profile = ctl->profile();
    auto* top = new QVBoxLayout(this);
    auto* box = new QGroupBox(tr("Device"), this);
    auto* form = new QFormLayout(box);
    m_device = new QComboBox(box);
    for (const QString& c : ctl->audioChoices()) {
        m_device->addItem(ctl->audioChoiceLabel(c), c);
        if (c == m_profile.audioDevice) m_device->setCurrentIndex(m_device->count() - 1);
    }
    form->addRow(tr("Capture device"), m_device);
    auto* hint = new QLabel(tr("hw: devices are read directly with no resampling. PipeWire and default are fallbacks "
                               "for when the hw: device is busy; PipeWire may resample. Many PCI cards expect a cable "
                               "into the sound card's line in: pick that device here."),
                            box);
    hint->setWordWrap(true);
    hint->setObjectName("panelLabel");
    form->addRow(QString(), hint);
    m_rate = new QComboBox(box);
    std::vector<int> rates;
    if (AudioBackend* a = ctl->session().audio()) rates = a->supportedRates();
    if (rates.empty()) rates = {32000, 44100, 48000, 96000};
    for (int r : rates) {
        m_rate->addItem(QString("%1 Hz").arg(r), r);
        if (r == m_profile.audioRate) m_rate->setCurrentIndex(m_rate->count() - 1);
    }
    form->addRow(tr("Sample rate"), m_rate);
    m_format = new QLabel(box);
    if (ctl->session().hasAudio()) {
        const AudioFormat& af = ctl->session().audioFormat();
        m_format->setText(tr("%1, %2 Hz, %3 channel(s): the device's native format, kept as-is")
                              .arg(sampleFmtName(af.fmt))
                              .arg(af.rate)
                              .arg(af.channels));
    } else {
        m_format->setText(tr("No audio device open"));
    }
    form->addRow(tr("Format"), m_format);
    top->addWidget(box);

    auto* mix = new QGroupBox(tr("Mixer (applied live)"), this);
    auto* ml = new QFormLayout(mix);
    if (AudioBackend* a = ctl->session().audio()) {
        const auto controls = a->mixer();
        if (controls.empty()) ml->addRow(new QLabel(tr("No capture controls on this card.")));
        for (const MixerControl& c : controls) {
            const QString label = QString::fromStdString(c.name) + (c.index ? QString(" %1").arg(c.index) : QString());
            const QString key = QString::fromStdString(c.name) + "#" + QString::number(c.index);
            auto set = [this, a, c, key](long v) {
                std::string err;
                a->setMixer(c.name, c.index, v, err);
                m_profile.mixer[key] = v;
            };
            if (c.kind == MixerControl::Kind::Volume) {
                auto* s = new QSlider(Qt::Horizontal, mix);
                s->setRange(int(c.minimum), int(c.maximum));
                s->setValue(int(c.value));
                connect(s, &QSlider::valueChanged, this, [set](int v) { set(v); });
                ml->addRow(label + tr(" volume"), s);
            } else if (c.kind == MixerControl::Kind::Switch) {
                auto* cb = new QCheckBox(tr("capture"), mix);
                cb->setChecked(c.value != 0);
                connect(cb, &QCheckBox::toggled, this, [set](bool on) { set(on ? 1 : 0); });
                ml->addRow(label, cb);
            } else {
                auto* cb = new QComboBox(mix);
                for (const auto& it : c.items) cb->addItem(QString::fromStdString(it));
                cb->setCurrentIndex(int(c.value));
                connect(cb, &QComboBox::currentIndexChanged, this, [set](int i) { set(i); });
                ml->addRow(label, cb);
            }
        }
    } else {
        ml->addRow(new QLabel(tr("Open a device to see its mixer.")));
    }
    top->addWidget(mix);

    auto* test = new QGroupBox(tr("Test meter"), this);
    auto* tl = new QVBoxLayout(test);
    m_meter = new Meters(test);
    tl->addWidget(m_meter);
    top->addWidget(test);
    connect(m_meter, &Meters::clipReset, this, [ctl] { ctl->session().resetClip(); });
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] { m_meter->setLevels(m_ctl->session().levels()); });
    m_timer->start(33);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
    resize(520, 560);
}

void AudioDialog::accept() {
    m_profile.audioDevice = m_device->currentData().toString();
    m_profile.audioRate = m_rate->currentData().toInt();
    QString err;
    if (!m_ctl->applyProfile(m_profile, err)) {
        QMessageBox::warning(this, tr("Audio"), err);
        return;
    }
    QDialog::accept();
}

}  // namespace pd
