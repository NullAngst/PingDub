// SPDX-License-Identifier: GPL-3.0-or-later
#include "VideoSourceDialog.h"

#include "backends/V4l2Logic.h"
#include "ui/SessionController.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pd {

VideoSourceDialog::VideoSourceDialog(SessionController* ctl, QWidget* parent) : QDialog(parent), m_ctl(ctl) {
    setWindowTitle(tr("Video Source"));
    m_profile = ctl->profile();
    VideoBackend* v = ctl->session().video();
    auto* top = new QVBoxLayout(this);
    if (!v) {
        top->addWidget(new QLabel(tr("No video device is open."), this));
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        top->addWidget(bb);
        return;
    }
    const VideoFormat& vf = ctl->session().videoFormat();

    auto* src = new QGroupBox(tr("Source"), this);
    auto* form = new QFormLayout(src);
    m_input = new QComboBox(src);
    const int curIn = v->currentInput();
    for (const auto& in : v->inputs()) {
        m_input->addItem(QString::fromStdString(in.name), in.index);
        if (in.index == curIn) m_input->setCurrentIndex(m_input->count() - 1);
    }
    form->addRow(tr("Input"), m_input);

    m_std = new QComboBox(src);
    const uint64_t curStd = v->currentStandard();
    for (const auto& s : v->standards()) {
        m_std->addItem(QString::fromStdString(s.name), QVariant::fromValue<qulonglong>(s.id));
        if (s.id == curStd) m_std->setCurrentIndex(m_std->count() - 1);
    }
    form->addRow(tr("Standard"), m_std);
    m_detected = new QLabel(src);
    const SignalStatus sig = ctl->session().stats().signal;
    m_detected->setText(sig.detected.empty() ? tr("Detected: unknown (the driver cannot tell, or no signal)")
                                             : tr("Detected: %1").arg(QString::fromStdString(sig.detected)));
    form->addRow(QString(), m_detected);
    auto* note = new QLabel(tr("NTSC-J has no 7.5 IRE setup: a US tape captured as NTSC-J has lifted blacks. "
                               "PingDub uses NTSC-M unless you pick otherwise, even on Japanese hardware like the GV-USB2."),
                            src);
    note->setWordWrap(true);
    note->setObjectName("panelLabel");
    form->addRow(QString(), note);

    m_fmt = new QComboBox(src);
    m_fmt->addItem(tr("Automatic (most native)"), QString());
    for (const auto& f : v->formats()) {
        QString label = QString::fromStdString(f.description);
        if (!f.archival) label += tr("  (not archival)");
        m_fmt->addItem(label, pixFmtToString(f.pixfmt));
        if (pixFmtToString(f.pixfmt) == m_profile.pixfmt) m_fmt->setCurrentIndex(m_fmt->count() - 1);
    }
    form->addRow(tr("Pixel format"), m_fmt);
    auto* size = new QLabel(tr("%1 x %2, the full native frame. Never scaled or cropped; crop in HandBrake.\n%3")
                                .arg(vf.width)
                                .arg(vf.height)
                                .arg(QString::fromStdString(v->cropSummary())),
                            src);
    size->setWordWrap(true);
    form->addRow(tr("Frame size"), size);

    m_field = new QComboBox(src);
    m_field->addItem(tr("From driver"), QString());
    m_field->addItem(tr("Top field first"), "TFF");
    m_field->addItem(tr("Bottom field first"), "BFF");
    m_field->addItem(tr("Progressive"), "progressive");
    m_field->setCurrentIndex(std::max(0, m_field->findData(m_profile.fieldOverride)));
    m_field->setToolTip(tr("Some devices report the wrong field order. Current: %1").arg(fieldOrderName(vf.field)));
    form->addRow(tr("Field order"), m_field);

    m_sar = new QComboBox(src);
    m_sar->addItem(tr("From sampling (%1:%2)").arg(sarFor(vf.width, vf.lines525).num).arg(sarFor(vf.width, vf.lines525).den), QString());
    for (const char* s : {"10:11", "12:11", "1:1", "8:9", "16:15"}) m_sar->addItem(s, s);
    m_sar->setEditable(true);
    if (!m_profile.sarOverride.isEmpty()) {
        const int i = m_sar->findData(m_profile.sarOverride);
        if (i >= 0) m_sar->setCurrentIndex(i);
        else m_sar->setEditText(m_profile.sarOverride);
    }
    m_sar->setToolTip(tr("Pixel aspect describes pixel shape only and discards nothing. BT.601 sampling is 10:11 for "
                         "525-line and 12:11 for 625-line; square-pixel devices are 1:1."));
    form->addRow(tr("Pixel aspect"), m_sar);
    top->addWidget(src);

    auto* controls = new QGroupBox(tr("Driver controls (applied live)"), this);
    auto* cl = new QVBoxLayout(controls);
    auto* scroll = new QScrollArea(controls);
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget(scroll);
    auto* il = new QVBoxLayout(inner);
    buildControls(il);
    il->addStretch();
    scroll->setWidget(inner);
    scroll->setMinimumHeight(220);
    cl->addWidget(scroll);
    top->addWidget(controls, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
    resize(560, 680);
}

void VideoSourceDialog::buildControls(QVBoxLayout* into) {
    VideoBackend* v = m_ctl->session().video();
    const auto ctrls = v->controls();
    if (ctrls.empty()) into->addWidget(new QLabel(tr("This driver exposes no controls.")));
    for (const VideoControl& c : ctrls) {
        m_original[c.id] = c.value;
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* name = new QLabel(QString::fromStdString(c.name) + (c.privateControl ? tr(" (driver)") : QString()));
        name->setMinimumWidth(180);
        h->addWidget(name);
        const uint32_t id = c.id;
        auto apply = [this, id](qint64 val) {
            std::string err;
            m_ctl->session().setControl(id, val, err);
            m_profile.controls[id] = val;
        };
        QWidget* editor = nullptr;
        switch (c.type) {
        case VideoControl::Type::Boolean: {
            auto* cb = new QCheckBox;
            cb->setChecked(c.value != 0);
            connect(cb, &QCheckBox::toggled, this, [apply](bool on) { apply(on ? 1 : 0); });
            editor = cb;
            break;
        }
        case VideoControl::Type::Menu:
        case VideoControl::Type::IntegerMenu: {
            auto* cb = new QComboBox;
            for (const auto& m : c.menu) {
                cb->addItem(QString::fromStdString(m.second), qlonglong(m.first));
                if (m.first == c.value) cb->setCurrentIndex(cb->count() - 1);
            }
            connect(cb, &QComboBox::currentIndexChanged, this, [cb, apply](int) { apply(cb->currentData().toLongLong()); });
            editor = cb;
            break;
        }
        case VideoControl::Type::Button: {
            auto* b = new QPushButton(tr("Run"));
            connect(b, &QPushButton::clicked, this, [apply] { apply(1); });
            editor = b;
            break;
        }
        default: {
            auto* w = new QWidget;
            auto* wl = new QHBoxLayout(w);
            wl->setContentsMargins(0, 0, 0, 0);
            auto* sl = new QSlider(Qt::Horizontal);
            auto* sp = new QSpinBox;
            const int lo = int(std::max<int64_t>(c.minimum, INT32_MIN)), hi = int(std::min<int64_t>(c.maximum, INT32_MAX));
            sl->setRange(lo, hi);
            sp->setRange(lo, hi);
            sl->setSingleStep(int(c.step));
            sp->setSingleStep(int(c.step));
            sl->setValue(int(c.value));
            sp->setValue(int(c.value));
            connect(sl, &QSlider::valueChanged, sp, &QSpinBox::setValue);
            connect(sp, &QSpinBox::valueChanged, sl, &QSlider::setValue);
            connect(sp, &QSpinBox::valueChanged, this, [apply](int val) { apply(val); });
            auto* reset = new QPushButton(tr("Reset"));
            const int def = int(c.defaultValue);
            connect(reset, &QPushButton::clicked, sp, [sp, def] { sp->setValue(def); });
            wl->addWidget(sl, 1);
            wl->addWidget(sp);
            wl->addWidget(reset);
            editor = w;
            break;
        }
        }
        editor->setEnabled(!c.readOnly);
        h->addWidget(editor, 1);
        into->addWidget(row);
    }
}

void VideoSourceDialog::reject() {
    // Put live control changes back.
    for (auto it = m_original.begin(); it != m_original.end(); ++it) {
        if (m_profile.controls.value(it.key(), it.value()) != it.value()) {
            std::string err;
            m_ctl->session().setControl(it.key(), it.value(), err);
        }
    }
    QDialog::reject();
}

void VideoSourceDialog::accept() {
    if (m_input) {
        m_profile.input = m_input->currentData().toInt();
        m_profile.standard = m_std->currentData().toULongLong();
        m_profile.pixfmt = m_fmt->currentData().toString();
        m_profile.fieldOverride = m_field->currentData().toString();
        const bool fromSampling = m_sar->currentIndex() == 0 && m_sar->currentText() == m_sar->itemText(0);
        const QString sarText = fromSampling ? QString() : m_sar->currentText().trimmed();
        m_profile.sarOverride = sarFromString(sarText).valid() ? sarText : QString();
        QString err;
        if (!m_ctl->applyProfile(m_profile, err)) {
            QMessageBox::warning(this, tr("Video Source"), err);
            return;
        }
    }
    QDialog::accept();
}

}  // namespace pd
