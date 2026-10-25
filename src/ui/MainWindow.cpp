// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"

#include "CaptionReadout.h"
#include "Histogram.h"
#include "Meters.h"
#include "PreviewWidget.h"
#include "StatsStrip.h"
#include "Theme.h"
#include "backends/V4l2Logic.h"
#include "devices/DeviceScanner.h"
#include "dialogs/AudioDialog.h"
#include "dialogs/DiagnosticsDialog.h"
#include "dialogs/OutputDialog.h"
#include "dialogs/PreferencesDialog.h"
#include "dialogs/PreflightDialog.h"
#include "dialogs/StopConditionsDialog.h"
#include "dialogs/TimingDialog.h"
#include "dialogs/VideoSourceDialog.h"
#include "engine/Recover.h"
#include "engine/SysInfo.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <thread>

namespace pd {

MainWindow::MainWindow(DeviceScanner* scanner, QWidget* parent) : QMainWindow(parent), m_scanner(scanner) {
    m_ctl = new SessionController(scanner, this);
    setWindowTitle("PingDub");
    setWindowIcon(QIcon(":/pingdub.svg"));
    buildMenus();
    buildUi();
    applyKeys();

    connect(m_ctl, &SessionController::deviceChanged, this, &MainWindow::onDeviceChanged);
    connect(m_ctl, &SessionController::captureFinished, this, &MainWindow::onCaptureFinished);
    connect(m_ctl, &SessionController::captureStarted, this, [this] { tickStats(); });
    connect(m_ctl, &SessionController::message, this, [this](const QString& t) { statusBar()->showMessage(t, 8000); });
    connect(m_ctl, &SessionController::deviceLost, this, [this](const QString& why) {
        statusBar()->showMessage(tr("Device lost: %1").arg(why), 15000);
        m_preview->clearFrame();
        m_preview->setMessage(tr("Device lost: %1").arg(why));
    });
    connect(scanner, &DeviceScanner::changed, this, [this] {
        populateDevices();
        // A device that went away (unplugged, powered off) and came back is
        // reopened once udev has settled, so a power-cycled VCR chain needs no clicks.
        if (!m_ctl->session().isOpen() && !m_ctl->capturing()) {
            const QString last = m_ctl->settings().lastDevice;
            for (const auto& d : m_scanner->devices()) {
                if (QString::fromStdString(d.id) != last) continue;
                QString err;
                if (m_ctl->openDevice(last, err)) statusBar()->showMessage(tr("Reopened %1").arg(QString::fromStdString(d.name)), 6000);
                break;
            }
        }
        updateBanner();
    });

    connect(&m_frameTimer, &QTimer::timeout, this, &MainWindow::tickFrame);
    connect(&m_statsTimer, &QTimer::timeout, this, &MainWindow::tickStats);
    connect(&m_meterTimer, &QTimer::timeout, this, [this] { m_meters->setLevels(m_ctl->session().levels()); });
    m_frameTimer.start(16);
    m_statsTimer.start(100);
    m_meterTimer.start(33);
    m_armTimer.setSingleShot(true);
    connect(&m_armTimer, &QTimer::timeout, this, [this] {
        m_timedLabel->clear();
        startCapture(m_armed.durationSec);
    });

    populateDevices();
    // Reopen the last device, or the first one found.
    const auto& devs = scanner->devices();
    QString pick = m_ctl->settings().lastDevice;
    bool found = false;
    for (const auto& d : devs) found |= QString::fromStdString(d.id) == pick;
    if (!found && !devs.empty()) pick = QString::fromStdString(devs.front().id);
    if (!pick.isEmpty()) {
        QString err;
        if (!m_ctl->openDevice(pick, err)) statusBar()->showMessage(err, 15000);
    } else {
        onDeviceChanged();
    }
    updateBanner();
    restoreLayout();
}

void MainWindow::restoreLayout() {
    QSettings st(ProfileStore::appSettingsPath(), QSettings::IniFormat);
    const bool restored = restoreGeometry(st.value("ui/geometry").toByteArray());
    m_split->restoreState(st.value("ui/splitter").toByteArray());
    // The window has to fit the screen it opens on. With desktop scaling a
    // 1920x1080 screen at 150% is 1280x720 to Qt, so a fixed 1280x860 hung
    // off the right and bottom edges.
    QScreen* sc = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!sc) {
        if (!restored) resize(1280, 860);
        return;
    }
    const QRect avail = sc->availableGeometry();
    const QSize room(avail.width() * 95 / 100, avail.height() * 92 / 100);
    QSize want = restored ? size() : QSize(1280, 860);
    want = want.boundedTo(room).expandedTo(minimumSizeHint());
    if (!restored || want != size()) {
        resize(want);
        move(avail.center() - QPoint(want.width() / 2, want.height() / 2));
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("Open capture folder"), this, [this] {
        const std::string f = expandHome(m_ctl->profile().output.folder.empty() ? "~/Videos/PingDub" : m_ctl->profile().output.folder);
        makeDirs(f);
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(f)));
    });
    file->addAction(tr("&Recover..."), this, &MainWindow::recover);
    file->addSeparator();
    file->addAction(tr("Save profile as..."), this, &MainWindow::saveProfileAs);
    file->addAction(tr("Import profile..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Import profile"), QString(), tr("Profiles (*.ini)"));
        QString name;
        if (!f.isEmpty() && m_ctl->profiles().importFrom(f, name)) {
            populateSidePanel();
            statusBar()->showMessage(tr("Imported profile %1").arg(name), 5000);
        }
    });
    file->addAction(tr("Export profile..."), this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Export profile"), m_ctl->profile().name + ".ini", tr("Profiles (*.ini)"));
        if (!f.isEmpty() && !m_ctl->profiles().exportTo(m_ctl->profile().name, f))
            QMessageBox::warning(this, tr("Export"), tr("Save the profile first, then export it."));
    });
    file->addSeparator();
    QAction* quit = file->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* cap = menuBar()->addMenu(tr("&Capture"));
    cap->addAction(tr("Start / stop"), this, &MainWindow::toggleCapture);
    cap->addSeparator();
    cap->addAction(tr("&Video Source..."), this, &MainWindow::openVideoSource);
    cap->addAction(tr("&Audio..."), this, &MainWindow::openAudio);
    cap->addAction(tr("&Timing..."), this, &MainWindow::openTiming);
    cap->addAction(tr("&Output..."), this, &MainWindow::openOutput);
    cap->addAction(tr("&Stop Conditions..."), this, &MainWindow::openStopConditions);
    cap->addAction(tr("Timed &Capture..."), this, &MainWindow::openTimedCapture);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    auto* deint = new QActionGroup(this);
    struct D {
        const char* label;
        PreviewDeinterlace mode;
        const char* key;
    };
    for (D d : {D{"Weave (real fields)", PreviewDeinterlace::Weave, "weave"}, D{"Bob", PreviewDeinterlace::Bob, "bob"},
                D{"Blend", PreviewDeinterlace::Blend, "blend"}}) {
        QAction* a = view->addAction(tr(d.label));
        a->setCheckable(true);
        a->setChecked(m_ctl->settings().previewDeinterlace == d.key);
        deint->addAction(a);
        const PreviewDeinterlace mode = d.mode;
        const QString key = d.key;
        connect(a, &QAction::triggered, this, [this, mode, key] {
            m_preview->setDeinterlace(mode);
            m_ctl->settings().previewDeinterlace = key;
            m_ctl->settings().save();
        });
    }
    view->addSeparator();
    m_pausePreview = view->addAction(tr("Pause preview (saves CPU)"));
    m_pausePreview->setCheckable(true);
    connect(m_pausePreview, &QAction::toggled, this, [this](bool on) { m_preview->setPaused(on); });
    QAction* wave = view->addAction(tr("Luma waveform instead of histogram"));
    wave->setCheckable(true);
    connect(wave, &QAction::toggled, this, [this](bool on) { m_hist->setWaveform(on); });

    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(tr("&Diagnostics..."), this, &MainWindow::openDiagnostics);
    tools->addAction(tr("Rescan devices"), this, [this] {
        m_scanner->rescan();
        populateDevices();
        updateBanner();
    });
    tools->addAction(tr("&Preferences..."), this, &MainWindow::openPreferences);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("About PingDub"), this, [this] {
        QMessageBox::about(this, tr("About PingDub"),
                           tr("<b>PingDub %1</b><br>Analog video capture for Linux, modeled on VirtualDub's capture mode.<br>"
                              "Captures the untouched stream with correct timing; process it later in HandBrake.<br><br>"
                              "GPL-3.0-or-later.")
                               .arg(PINGDUB_VERSION));
    });
}

static QFrame* panel(const char* name, QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setObjectName(name);
    return f;
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(6);

    m_banner = panel("banner", central);
    auto* bl = new QHBoxLayout(m_banner);
    m_bannerText = new QLabel(m_banner);
    m_bannerText->setWordWrap(true);
    m_bannerButton = new QPushButton(tr("Diagnostics"), m_banner);
    connect(m_bannerButton, &QPushButton::clicked, this, &MainWindow::openDiagnostics);
    bl->addWidget(m_bannerText, 1);
    bl->addWidget(m_bannerButton);
    m_banner->hide();
    v->addWidget(m_banner);

    // The side panel sits in a splitter so its width follows the font and the
    // user can drag it wider; a fixed pixel width clipped the combo boxes and
    // the browse button with larger desktop fonts.
    m_split = new QSplitter(Qt::Horizontal, central);
    m_split->setChildrenCollapsible(false);
    auto* leftBox = new QWidget(m_split);
    auto* left = new QVBoxLayout(leftBox);
    left->setContentsMargins(0, 0, 0, 0);
    m_preview = new PreviewWidget(leftBox);
    const QString d = m_ctl->settings().previewDeinterlace;
    m_preview->setDeinterlace(d == "bob" ? PreviewDeinterlace::Bob : d == "blend" ? PreviewDeinterlace::Blend : PreviewDeinterlace::Weave);
    left->addWidget(m_preview, 1);
    m_caption = new CaptionReadout(leftBox);
    left->addWidget(m_caption);

    m_side = panel("sidePanel", m_split);
    auto* form = new QFormLayout(m_side);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight);
    auto label = [&](const QString& t) {
        auto* l = new QLabel(t, m_side);
        l->setObjectName("panelLabel");
        return l;
    };
    m_device = new QComboBox(m_side);
    m_profileBox = new QComboBox(m_side);
    m_input = new QComboBox(m_side);
    m_standard = new QComboBox(m_side);
    m_format = new QComboBox(m_side);
    m_audio = new QComboBox(m_side);
    m_sync = new QComboBox(m_side);
    m_codec = new QComboBox(m_side);
    for (QComboBox* c : {m_device, m_profileBox, m_input, m_standard, m_format, m_audio, m_sync, m_codec}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(18);
        // Long entries still fit in the popup; the tooltip shows the full current one.
        connect(c, &QComboBox::currentTextChanged, c, [c](const QString& t) { c->setToolTip(t); });
    }
    m_dest = new QLabel(m_side);
    m_dest->setWordWrap(true);
    m_formatInfo = new QLabel(m_side);
    m_formatInfo->setWordWrap(true);
    m_formatInfo->setObjectName("panelLabel");
    form->addRow(label(tr("Device")), m_device);
    form->addRow(label(tr("Profile")), m_profileBox);
    form->addRow(label(tr("Input")), m_input);
    form->addRow(label(tr("Standard")), m_standard);
    form->addRow(label(tr("Format")), m_format);
    form->addRow(QString(), m_formatInfo);
    form->addRow(label(tr("Audio")), m_audio);
    form->addRow(label(tr("Sync")), m_sync);
    form->addRow(label(tr("Codec")), m_codec);
    auto* destRow = new QWidget(m_side);
    auto* dr = new QHBoxLayout(destRow);
    dr->setContentsMargins(0, 0, 0, 0);
    dr->addWidget(m_dest, 1);
    auto* destBtn = new QPushButton(tr("Change..."), destRow);
    destBtn->setToolTip(tr("Output folder, container and audio format"));
    destBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(destBtn, &QPushButton::clicked, this, &MainWindow::openOutput);
    dr->addWidget(destBtn, 0, Qt::AlignTop);
    form->addRow(label(tr("Destination")), destRow);

    // Never narrower than the form needs at the current font; start at its preferred width.
    m_side->setMinimumWidth(m_side->minimumSizeHint().width());
    m_split->addWidget(leftBox);
    m_split->addWidget(m_side);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);
    m_split->setSizes({4 * m_side->sizeHint().width(), m_side->sizeHint().width()});
    v->addWidget(m_split, 1);

    auto* bottom = panel("bottomPanel", central);
    auto* bh = new QHBoxLayout(bottom);
    m_meters = new Meters(bottom);
    m_hist = new Histogram(bottom);
    bh->addWidget(m_meters, 1);
    bh->addWidget(m_hist, 1);
    bottom->setFixedHeight(110);
    v->addWidget(bottom);
    connect(m_meters, &Meters::clipReset, this, [this] { m_ctl->session().resetClip(); });

    m_stats = new StatsStrip(central);
    v->addWidget(m_stats);

    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Tape label"), central));
    m_label = new QLineEdit(m_ctl->settings().lastLabel, central);
    m_label->setPlaceholderText(tr("goes into the file name and tags"));
    row->addWidget(m_label, 1);
    m_timedLabel = new QLabel(central);
    row->addWidget(m_timedLabel);
    m_capture = new QPushButton(tr("Capture"), central);
    m_capture->setObjectName("captureButton");
    m_capture->setMinimumWidth(180);
    connect(m_capture, &QPushButton::clicked, this, &MainWindow::toggleCapture);
    row->addWidget(m_capture);
    v->addLayout(row);
    setCentralWidget(central);

    // Side panel edits.
    connect(m_device, &QComboBox::activated, this, [this](int) {
        if (m_populating) return;
        QString err;
        if (!m_ctl->openDevice(m_device->currentData().toString(), err)) QMessageBox::warning(this, tr("Device"), err);
    });
    connect(m_profileBox, &QComboBox::activated, this, [this](int) {
        if (m_populating) return;
        QString err;
        if (!m_ctl->switchProfile(m_profileBox->currentText(), err)) QMessageBox::warning(this, tr("Profile"), err);
        else m_ctl->profiles().setDefault(m_ctl->deviceKey(), m_profileBox->currentText());
    });
    connect(m_input, &QComboBox::activated, this, [this](int) { editProfile([this](Profile& p) { p.input = m_input->currentData().toInt(); }); });
    connect(m_standard, &QComboBox::activated, this,
            [this](int) { editProfile([this](Profile& p) { p.standard = m_standard->currentData().toULongLong(); }); });
    connect(m_format, &QComboBox::activated, this, [this](int) { editProfile([this](Profile& p) { p.pixfmt = m_format->currentData().toString(); }); });
    connect(m_audio, &QComboBox::activated, this, [this](int) { editProfile([this](Profile& p) { p.audioDevice = m_audio->currentData().toString(); }); });
    connect(m_sync, &QComboBox::activated, this, [this](int) { editProfile([this](Profile& p) { p.timing.sync = SyncMode(m_sync->currentData().toInt()); }); });
    connect(m_codec, &QComboBox::activated, this, [this](int) { editProfile([this](Profile& p) { p.output.vcodec = VideoCodec(m_codec->currentData().toInt()); }); });
}

void MainWindow::applyKeys() {
    delete m_startKey;
    delete m_stopKey;
    m_startKey = new QShortcut(QKeySequence(m_ctl->settings().keyStart), this);
    m_stopKey = new QShortcut(QKeySequence(m_ctl->settings().keyStop), this);
    connect(m_startKey, &QShortcut::activated, this, [this] {
        if (!m_ctl->capturing()) startCapture();
    });
    connect(m_stopKey, &QShortcut::activated, this, [this] {
        if (m_ctl->stopping()) return;
        if (m_ctl->capturing()) stopCapture();
        else if (m_armTimer.isActive()) {
            m_armTimer.stop();
            m_timedLabel->clear();
            statusBar()->showMessage(tr("Timed capture disarmed"), 4000);
        }
    });
    m_capture->setToolTip(tr("Start: %1, stop: %2").arg(m_ctl->settings().keyStart, m_ctl->settings().keyStop));
}

void MainWindow::editProfile(const std::function<void(Profile&)>& change) {
    if (m_populating) return;
    Profile p = m_ctl->profile();
    change(p);
    QString err;
    if (!m_ctl->applyProfile(p, err)) QMessageBox::warning(this, tr("Settings"), err);
}

void MainWindow::populateDevices() {
    m_populating = true;
    m_device->clear();
    for (const CaptureDevice& d : m_scanner->devices()) m_device->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
    if (m_device->count() == 0) m_device->addItem(tr("No capture device found"), QString());
    const int i = m_device->findData(m_ctl->deviceId());
    if (i >= 0) m_device->setCurrentIndex(i);
    m_populating = false;
}

void MainWindow::populateSidePanel() {
    m_populating = true;
    const Profile& p = m_ctl->profile();
    CaptureSession& s = m_ctl->session();
    VideoBackend* v = s.video();
    m_profileBox->clear();
    QStringList names = m_ctl->profiles().profilesFor(m_ctl->deviceKey());
    if (!names.contains(p.name)) names.prepend(p.name);
    m_profileBox->addItems(names);
    m_profileBox->setCurrentText(p.name);
    m_input->clear();
    m_standard->clear();
    m_format->clear();
    if (v) {
        const int cur = v->currentInput();
        for (const auto& in : v->inputs()) {
            m_input->addItem(QString::fromStdString(in.name), in.index);
            if (in.index == cur) m_input->setCurrentIndex(m_input->count() - 1);
        }
        const uint64_t cs = v->currentStandard();
        for (const auto& st : v->standards()) {
            m_standard->addItem(QString::fromStdString(st.name), QVariant::fromValue<qulonglong>(st.id));
            if (st.id == cs) m_standard->setCurrentIndex(m_standard->count() - 1);
        }
        m_format->addItem(tr("Automatic"), QString());
        for (const auto& f : v->formats()) {
            m_format->addItem(QString::fromLatin1(pixFmtName(f.pixfmt)) + (f.archival ? "" : tr(" (not archival)")), pixFmtToString(f.pixfmt));
            if (pixFmtToString(f.pixfmt) == p.pixfmt) m_format->setCurrentIndex(m_format->count() - 1);
        }
        const VideoFormat& vf = s.videoFormat();
        m_formatInfo->setText(tr("%1 %2x%3, %4, SAR %5:%6")
                                  .arg(pixFmtName(vf.pixfmt))
                                  .arg(vf.width)
                                  .arg(vf.height)
                                  .arg(fieldOrderName(vf.field))
                                  .arg(vf.sar.num)
                                  .arg(vf.sar.den));
    } else {
        m_formatInfo->clear();
    }
    m_audio->clear();
    for (const QString& c : m_ctl->audioChoices()) {
        m_audio->addItem(m_ctl->audioChoiceLabel(c), c);
        if (c == p.audioDevice) m_audio->setCurrentIndex(m_audio->count() - 1);
    }
    m_sync->clear();
    m_sync->addItem(tr("Video master"), int(SyncMode::VideoMaster));
    m_sync->addItem(tr("Audio master"), int(SyncMode::AudioMaster));
    m_sync->addItem(tr("No correction"), int(SyncMode::NoCorrection));
    m_sync->setCurrentIndex(m_sync->findData(int(p.timing.sync)));
    m_codec->clear();
    m_codec->addItem("FFV1", int(VideoCodec::FFV1));
    m_codec->addItem("UT Video", int(VideoCodec::UTVideo));
    m_codec->addItem("FFVHuff / HuffYUV", int(VideoCodec::Huff));
    m_codec->addItem(tr("Uncompressed"), int(VideoCodec::Raw));
    m_codec->setCurrentIndex(m_codec->findData(int(p.output.vcodec)));
    const QString folder = QString::fromStdString(p.output.folder.empty() ? "~/Videos/PingDub" : p.output.folder);
    m_dest->setText(folder + (p.output.container == Container::AVI ? " (AVI)" : " (MKV)") +
                    (p.output.acodec == AudioCodec::FLAC ? ", FLAC" : ", PCM"));
    m_populating = false;
}

void MainWindow::updateBanner() {
    QStringList lines;
    const auto& missing = m_scanner->missing();
    for (const auto& m : missing) lines << QString::fromStdString(m.message);
    if (m_scanner->devices().empty() && missing.empty())
        lines << tr("No capture device found. Diagnostics shows what is connected and which drivers are bound.");
    if (const CaptureDevice* d = m_ctl->device(); d && d->experimental)
        lines << tr("%1 uses an experimental backend that has not been tested on real hardware.").arg(QString::fromStdString(d->name));
    for (const auto& w : m_ctl->session().warnings()) lines << QString::fromStdString(w);
    m_banner->setVisible(!lines.isEmpty());
    m_bannerText->setText(lines.join("\n"));
}

void MainWindow::onDeviceChanged() {
    populateDevices();
    populateSidePanel();
    CaptureSession& s = m_ctl->session();
    if (s.isOpen()) {
        m_preview->setFormat(s.videoFormat());
        m_preview->setMessage(tr("Waiting for frames..."));
    } else {
        m_preview->clearFrame();
        m_preview->setMessage(m_scanner->devices().empty() ? tr("No capture device found.\nTools > Diagnostics explains why.")
                                                           : tr("No device open"));
    }
    m_previewSerial = 0;
    updateBanner();
}

void MainWindow::tickFrame() {
    VideoFrame f;
    if (m_ctl->session().latestFrame(f, m_previewSerial)) m_preview->showFrame(f);
}

void MainWindow::tickStats() {
    CaptureSession& s = m_ctl->session();
    const SessionStats st = s.stats();
    m_stats->showStats(st, s.videoFormat(), s.videoFormat().standardName);
    m_caption->setCaption(st.vbiActive, QString::fromStdString(st.ccLive));
    static int n = 0;
    if (++n % 2 == 0) m_hist->setLuma(m_preview->luma());
    const bool cap = st.state == SessionState::Capturing || st.state == SessionState::Stopping;
    m_side->setEnabled(!cap);
    m_capture->setEnabled(s.isOpen() && st.state != SessionState::Stopping && !m_ctl->stopping());
    m_capture->setText(cap ? tr("Stop  (%1)").arg(m_ctl->settings().keyStop) : tr("Capture  (%1)").arg(m_ctl->settings().keyStart));
    if (m_capture->property("capturing").toBool() != cap) {
        m_capture->setProperty("capturing", cap);
        m_capture->style()->unpolish(m_capture);
        m_capture->style()->polish(m_capture);
    }
    m_label->setEnabled(!cap);
    if (m_armTimer.isActive()) {
        const qint64 left = QDateTime::currentDateTime().secsTo(m_armed.startAt);
        m_timedLabel->setText(tr("Starts in %1").arg(humanDuration(double(left))));
    }
}

void MainWindow::toggleCapture() {
    if (m_ctl->stopping()) return;
    if (m_ctl->capturing()) stopCapture();
    else startCapture();
}

void MainWindow::startCapture(double durationOverride) {
    CaptureSession& s = m_ctl->session();
    if (!s.isOpen()) {
        statusBar()->showMessage(tr("No device open."), 5000);
        return;
    }
    const double planned = durationOverride > 0 ? durationOverride : m_ctl->settings().stop.durationSec;
    const auto items = s.preflight(planned);
    if (PreflightDialog::needsAttention(items) && !m_armed.startAt.isValid()) {
        PreflightDialog d(items, this);
        if (d.exec() != QDialog::Accepted) return;
    }
    m_armed = TimedCapture{};
    QString err;
    if (!m_ctl->startCapture(m_label->text().trimmed(), err, durationOverride)) {
        QMessageBox::critical(this, tr("Capture"), err);
        return;
    }
    statusBar()->showMessage(tr("Capturing to %1").arg(QString::fromStdString(s.stats().currentFile)));
    updateBanner();
}

void MainWindow::stopCapture() {
    const SessionStats st = m_ctl->session().stats();
    if (m_ctl->settings().confirmStop && st.elapsedSec > 60) {
        if (QMessageBox::question(this, tr("Stop capture"), tr("Stop the capture after %1?").arg(humanDuration(st.elapsedSec))) !=
            QMessageBox::Yes)
            return;
    }
    statusBar()->showMessage(tr("Finishing the file..."));
    m_ctl->stopCaptureAsync();
}

void MainWindow::onCaptureFinished(const CaptureSummary& s, bool automatic) {
    if (s.files.empty() && s.stopReason.empty()) return;
    const SyncCounters& c = s.sync;
    QString text = tr("%1 of video, %2 frames (%3 inserted, %4 dropped, %5 driver drops, %6 buffer overflow, %7 xruns). "
                      "Stopped: %8.")
                       .arg(humanDuration(s.seconds))
                       .arg(c.framesOut)
                       .arg(c.inserted)
                       .arg(c.dropped)
                       .arg(c.driverDrops)
                       .arg(c.ringDrops)
                       .arg(c.xruns)
                       .arg(QString::fromStdString(s.stopReason));
    for (const auto& f : s.files) text += "\n" + QString::fromStdString(f);
    if (!s.ok) {
        QMessageBox::warning(this, tr("Capture finished with an error"), QString::fromStdString(s.error) + "\n\n" + text);
    } else if (automatic) {
        QMessageBox::information(this, tr("Capture finished"), text);
    }
    statusBar()->showMessage(text.section('\n', 0, 0), 30000);
    if (s.stopReason.rfind("device lost", 0) == 0) {
        m_ctl->closeDevice();
        m_scanner->rescan();
        populateDevices();
    }
    updateBanner();
}

void MainWindow::openVideoSource() {
    VideoSourceDialog d(m_ctl, this);
    d.exec();
    populateSidePanel();
}

void MainWindow::openAudio() {
    AudioDialog d(m_ctl, this);
    d.exec();
    populateSidePanel();
}

void MainWindow::openTiming() {
    TimingDialog d(m_ctl->profile().timing, this);
    if (d.exec() == QDialog::Accepted) editProfile([&](Profile& p) { p.timing = d.result(); });
}

void MainWindow::openOutput() {
    OutputDialog d(m_ctl->profile().output, this);
    if (d.exec() == QDialog::Accepted) editProfile([&](Profile& p) { p.output = d.result(); });
    populateSidePanel();
}

void MainWindow::openStopConditions() {
    StopConditionsDialog d(m_ctl->settings().stop, this);
    if (d.exec() == QDialog::Accepted) m_ctl->applyStopConditions(d.result());
}

void MainWindow::openTimedCapture() {
    if (m_ctl->capturing()) return;
    TimedCaptureDialog d(this);
    if (d.exec() != QDialog::Accepted) return;
    const TimedCapture t = d.result();
    if (!t.startAt.isValid()) {
        startCapture(t.durationSec);
        return;
    }
    // Check the setup now rather than at 3 a.m.
    const auto items = m_ctl->session().preflight(t.durationSec);
    if (PreflightDialog::needsAttention(items)) {
        PreflightDialog pd(items, this);
        if (pd.exec() != QDialog::Accepted) return;
    }
    m_armed = t;
    const qint64 ms = std::max<qint64>(0, QDateTime::currentDateTime().msecsTo(t.startAt));
    m_armTimer.start(int(std::min<qint64>(ms, INT32_MAX)));
    statusBar()->showMessage(tr("Timed capture armed for %1 (%2 to disarm)").arg(t.startAt.toString("HH:mm"), m_ctl->settings().keyStop));
}

void MainWindow::openDiagnostics() {
    DiagnosticsDialog d(m_ctl, this);
    d.exec();
    populateDevices();
    updateBanner();
}

void MainWindow::openPreferences() {
    PreferencesDialog d(m_ctl->settings(), this);
    if (d.exec() == QDialog::Accepted) {
        d.store(m_ctl->settings());
        m_ctl->settings().save();
        ThemeManager::instance().apply(m_ctl->settings().theme);
        applyKeys();
        m_ctl->session().setRuntimeSettings(m_ctl->profile().output, m_ctl->profile().timing, m_ctl->settings().stop,
                                            m_ctl->settings().captureVbi);
    }
}

void MainWindow::saveProfileAs() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save profile"), tr("Profile name, e.g. \"VCR S-video NTSC\""),
                                               QLineEdit::Normal, m_ctl->profile().name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    if (!m_ctl->saveProfileAs(name.trimmed())) {
        QMessageBox::warning(this, tr("Save profile"), tr("Could not write the profile."));
        return;
    }
    if (QMessageBox::question(this, tr("Save profile"), tr("Use \"%1\" by default for this device?").arg(name)) == QMessageBox::Yes)
        m_ctl->profiles().setDefault(m_ctl->deviceKey(), name.trimmed());
    populateSidePanel();
}

void MainWindow::recover() {
    const QString f = QFileDialog::getOpenFileName(this, tr("Recover a capture"), QString(), tr("Captures (*.mkv *.avi)"));
    if (f.isEmpty()) return;
    auto* dlg = new QProgressDialog(tr("Rebuilding the index of %1").arg(f), tr("Cancel"), 0, 1000, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    std::thread([this, f, dlg, cancel] {
        RecoverResult r = recoverCapture(f.toStdString(), {}, [dlg, cancel](uint64_t done, uint64_t total) {
            const int v = total ? int(1000.0 * double(done) / double(total)) : 0;
            QMetaObject::invokeMethod(dlg, [dlg, v] { dlg->setValue(v); }, Qt::QueuedConnection);
            return !cancel->load();
        });
        QMetaObject::invokeMethod(this, [this, dlg, r] {
            dlg->close();
            dlg->deleteLater();
            if (r.ok) QMessageBox::information(this, tr("Recover"), QString::fromStdString(r.message + "\n" + r.outPath));
            else QMessageBox::warning(this, tr("Recover"), QString::fromStdString(r.message));
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (m_ctl->stopping()) {
        statusBar()->showMessage(tr("Still finishing the last capture; try again in a moment."), 5000);
        e->ignore();
        return;
    }
    if (m_ctl->capturing()) {
        if (QMessageBox::question(this, tr("Quit"), tr("A capture is running. Stop it and quit?")) != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_ctl->stopCapture("PingDub closed");
        QApplication::restoreOverrideCursor();
    }
    m_frameTimer.stop();
    m_statsTimer.stop();
    m_meterTimer.stop();
    m_ctl->session().close();
    {
        QSettings st(ProfileStore::appSettingsPath(), QSettings::IniFormat);
        st.setValue("ui/geometry", saveGeometry());
        st.setValue("ui/splitter", m_split->saveState());
    }
    e->accept();
}

}  // namespace pd
