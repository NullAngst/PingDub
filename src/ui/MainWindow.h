// SPDX-License-Identifier: GPL-3.0-or-later
// One window built for watching a tape go by: big preview, levels and stats
// always visible, one Capture button. Settings live in dialogs.
#pragma once

#include "SessionController.h"
#include "dialogs/TimedCaptureDialog.h"

#include <QMainWindow>
#include <QTimer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShortcut;
class QFrame;
class QAction;
class QSplitter;

namespace pd {

class PreviewWidget;
class Meters;
class Histogram;
class StatsStrip;
class CaptionReadout;
class DeviceScanner;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(DeviceScanner* scanner, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildMenus();
    void buildUi();
    void restoreLayout();
    void populateDevices();
    void populateSidePanel();
    void updateBanner();
    void onDeviceChanged();
    void onCaptureFinished(const CaptureSummary& s, bool automatic);
    void toggleCapture();
    void startCapture(double durationOverride = 0);
    void stopCapture();
    void tickFrame();
    void tickStats();
    void applyKeys();
    void editProfile(const std::function<void(Profile&)>& change);
    void openVideoSource();
    void openAudio();
    void openTiming();
    void openOutput();
    void openStopConditions();
    void openTimedCapture();
    void openDiagnostics();
    void openPreferences();
    void recover();
    void saveProfileAs();

    DeviceScanner* m_scanner;
    SessionController* m_ctl;
    PreviewWidget* m_preview = nullptr;
    CaptionReadout* m_caption = nullptr;
    Meters* m_meters = nullptr;
    Histogram* m_hist = nullptr;
    StatsStrip* m_stats = nullptr;
    QFrame* m_banner = nullptr;
    QLabel* m_bannerText = nullptr;
    QPushButton* m_bannerButton = nullptr;
    QFrame* m_side = nullptr;
    QSplitter* m_split = nullptr;
    QComboBox* m_device = nullptr;
    QComboBox* m_profileBox = nullptr;
    QComboBox* m_input = nullptr;
    QComboBox* m_standard = nullptr;
    QComboBox* m_format = nullptr;
    QComboBox* m_audio = nullptr;
    QComboBox* m_sync = nullptr;
    QComboBox* m_codec = nullptr;
    QLabel* m_dest = nullptr;
    QLabel* m_formatInfo = nullptr;
    QLineEdit* m_label = nullptr;
    QLabel* m_timedLabel = nullptr;
    QPushButton* m_capture = nullptr;
    QShortcut* m_startKey = nullptr;
    QShortcut* m_stopKey = nullptr;
    QAction* m_pausePreview = nullptr;
    QTimer m_frameTimer, m_statsTimer, m_meterTimer, m_armTimer;
    TimedCapture m_armed;
    uint64_t m_previewSerial = 0;
    bool m_populating = false;
    double m_durationOverride = 0;
};

}  // namespace pd
