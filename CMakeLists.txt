cmake_minimum_required(VERSION 3.22)

# Version comes from `git describe`, falling back when built outside a checkout.
find_package(Git QUIET)
set(PINGDUB_VERSION "0.0.0-unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE PINGDUB_GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(PINGDUB_GIT_DESCRIBE)
        string(REGEX REPLACE "^v" "" PINGDUB_VERSION "${PINGDUB_GIT_DESCRIBE}")
    endif()
endif()
if(DEFINED ENV{PINGDUB_VERSION})
    set(PINGDUB_VERSION "$ENV{PINGDUB_VERSION}")
endif()

project(PingDub LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(PINGDUB_BUILD_GUI "Build the Qt GUI" ON)
option(PINGDUB_BUILD_TESTS "Build the test suite" ON)
option(PINGDUB_LTO "Link-time optimization" ON)

if(PINGDUB_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
    if(ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)
pkg_check_modules(SOXR REQUIRED IMPORTED_TARGET soxr)
pkg_check_modules(ZVBI REQUIRED IMPORTED_TARGET zvbi-0.2)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# --- engine: no Qt, linked by the app, the tests and --self-test -------------
add_library(pingdub_engine STATIC
    src/engine/Types.cpp
    src/engine/SyncEngine.cpp
    src/engine/PixelConvert.cpp
    src/engine/Encoder.cpp
    src/engine/Muxer.cpp
    src/engine/Output.cpp
    src/engine/EventLog.cpp
    src/engine/SysInfo.cpp
    src/engine/Recover.cpp
    src/engine/VbiCapture.cpp
    src/engine/Cea608.cpp
    src/engine/CaptureSession.cpp
    src/engine/SelfTest.cpp
    src/backends/V4l2Backend.cpp
    src/backends/V4l2Logic.cpp
    src/backends/AlsaBackend.cpp
    src/backends/SomagicBackend.cpp
    src/backends/SomagicParse.cpp
    src/backends/DeckLinkBackend.cpp
    src/backends/FakeBackends.cpp
    src/platform/RtKit.cpp
    third_party/decklink/DeckLinkAPIDispatch.cpp
)
target_include_directories(pingdub_engine PUBLIC src)
target_include_directories(pingdub_engine SYSTEM PUBLIC third_party/decklink)
target_compile_definitions(pingdub_engine PUBLIC PINGDUB_VERSION="${PINGDUB_VERSION}")
set_source_files_properties(third_party/decklink/DeckLinkAPIDispatch.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(pingdub_engine PUBLIC
    PkgConfig::FFMPEG PkgConfig::SOXR PkgConfig::ZVBI PkgConfig::ALSA
    PkgConfig::UDEV PkgConfig::LIBUSB PkgConfig::DBUS Threads::Threads ${CMAKE_DL_LIBS})

# --- GUI ---------------------------------------------------------------------
if(PINGDUB_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
    file(GLOB PINGDUB_THEMES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} src/ui/themes/*.theme)
    qt_add_executable(pingdub
        src/main.cpp
        src/devices/DeviceScanner.cpp
        src/devices/KnownDevices.cpp
        src/devices/Profiles.cpp
        src/ui/MainWindow.cpp
        src/ui/PreviewWidget.cpp
        src/ui/Meters.cpp
        src/ui/Histogram.cpp
        src/ui/StatsStrip.cpp
        src/ui/CaptionReadout.cpp
        src/ui/Theme.cpp
        src/ui/SessionController.cpp
        src/ui/dialogs/VideoSourceDialog.cpp
        src/ui/dialogs/AudioDialog.cpp
        src/ui/dialogs/TimingDialog.cpp
        src/ui/dialogs/OutputDialog.cpp
        src/ui/dialogs/StopConditionsDialog.cpp
        src/ui/dialogs/TimedCaptureDialog.cpp
        src/ui/dialogs/DiagnosticsDialog.cpp
        src/ui/dialogs/PreferencesDialog.cpp
        src/ui/dialogs/PreflightDialog.cpp
        src/ui/dialogs/SomagicSetupDialog.cpp
    )
    qt_add_resources(pingdub "themes"
        PREFIX "/"
        BASE src/ui
        FILES ${PINGDUB_THEMES} src/ui/themes/style.qss)
    qt_add_resources(pingdub "icons"
        PREFIX "/"
        BASE packaging
        FILES packaging/pingdub.svg)
    target_link_libraries(pingdub PRIVATE pingdub_engine Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
    target_include_directories(pingdub PRIVATE src)
    set_target_properties(pingdub PROPERTIES OUTPUT_NAME pingdub AUTOMOC ON AUTORCC ON)
    install(TARGETS pingdub RUNTIME DESTINATION bin)
    install(FILES packaging/pingdub.desktop DESTINATION share/applications RENAME io.github.nullangst.PingDub.desktop)
    install(FILES packaging/pingdub.svg DESTINATION share/icons/hicolor/scalable/apps RENAME io.github.nullangst.PingDub.svg)
    install(FILES packaging/io.github.nullangst.PingDub.metainfo.xml DESTINATION share/metainfo)
    install(FILES packaging/70-pingdub-somagic.rules DESTINATION share/pingdub)
endif()

# --- tests -------------------------------------------------------------------
if(PINGDUB_BUILD_TESTS)
    enable_testing()
    foreach(t sync ring output v4l2_parse vbi somagic_parse)
        add_executable(test_${t} tests/test_${t}.cpp)
        target_link_libraries(test_${t} PRIVATE pingdub_engine)
        target_include_directories(test_${t} PRIVATE tests src)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    set_tests_properties(sync PROPERTIES TIMEOUT 1800)
    set_tests_properties(output PROPERTIES TIMEOUT 600)
endif()
