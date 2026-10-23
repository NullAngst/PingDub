# PingDub

PingDub captures analog video (composite or S-video) on Linux and writes the untouched video and audio to disk with correct timing, for anyone archiving VHS, S-VHS, Hi8, Betamax, LaserDisc, camcorders or old consoles.

It is the capture mode of VirtualDub 1.10.4 and VirtualDub2, rewritten natively for Linux. No VirtualDub code is reused. What carries over is the feature set (preview, audio meters, histogram, timing control, auto-stop conditions, spill to another drive) and the timing model. Deinterlacing, cropping, denoise and encoding to something small happen later, in HandBrake.

Three rules drive everything:

1. **Bit-exact video.** Pixels the driver hands over reach the file unchanged. No scaling, cropping, deinterlacing or color conversion. Packed YUYV is repacked into planar 4:2:2 for the encoder, which is lossless.
2. **Correct timing.** Every frame is accounted for, field order is recorded, and audio is in sync at the end of a 6-hour tape.
3. **Nothing hidden.** Dropped frames, inserted frames, signal loss, buffer fill, clock drift and levels are on screen during capture and written to a log afterward.

## Prerequisites

- An x86_64 Linux desktop from 2022 or later (glibc 2.35+). I run Kubuntu with KDE Plasma on Wayland; X11 works too.
- A capture device whose driver gives a V4L2 capture node, plus its ALSA audio card. That covers nearly every analog capture device that works on Linux at all. Somagic EasyCaps and Blackmagic cards have their own backends (see below).
- Disk space. FFV1 at SD runs around 5 to 12 MB/s depending on how noisy the tape is, so a 2-hour tape is 35 to 90 GB.

## Install and run

1. Download `PingDub-x86_64.AppImage` from the Releases page.
2. `chmod +x PingDub-x86_64.AppImage`, since downloads are not executable.
3. Run it: `./PingDub-x86_64.AppImage`. No libfuse2 needed; it uses the static AppImage runtime.
4. Pick your device in the side panel. PingDub picks the paired audio card on its own.
5. Set levels with the meters and the histogram, type a tape label, press **F5**.
6. Press **Esc** to stop. After the first minute it asks first, so a stray key does not end a 4-hour capture.

Files land in `~/Videos/PingDub/` as `YYYY-MM-DD_HHMMSS_<label>.mkv` with sidecars next to them.

## GV-USB2 driver

The I-O Data GV-USB2 is the device I capture with. Its driver is not in the mainline kernel, so you build it against your running kernel. There is no DKMS here.

**REBUILD THE MODULE AFTER EVERY KERNEL UPDATE.** If you skip this, the device shows up with no driver after the next reboot and PingDub tells you exactly that.

1. `sudo apt install build-essential linux-headers-$(uname -r) git`, since the module builds against your running kernel.
2. Clone the driver and `cd` into it. Try upstream first: `git clone https://github.com/Isaac-Lozano/GV-USB2-Driver`. If it does not build on your kernel, try the fork: `git clone https://github.com/jkl1337/GV-USB2-Driver`. Neither is confirmed on current kernels yet, so expect build errors on new releases.
3. `make`
4. `sudo mkdir -p /lib/modules/$(uname -r)/extra && sudo cp *.ko /lib/modules/$(uname -r)/extra/`
5. `sudo depmod -a`
6. Check `cat /proc/asound/cards`, then set the sound module's ALSA index to one past the highest card number (N below), since the module has a card index quirk: `printf 'options gvusb2-sound index=N\n' | sudo tee /etc/modprobe.d/gvusb2.conf`
7. Load on boot: `printf 'gvusb2-video\ngvusb2-sound\n' | sudo tee /etc/modules-load.d/gvusb2.conf`
8. `sudo modprobe gvusb2-video && sudo modprobe gvusb2-sound`, then plug in the device. **Tools > Diagnostics** should show it bound to both drivers.

I use apt, so in my case that is step 1 as written. It depends on your system: swap apt for whichever package manager you use; the header package name varies (`kernel-devel` on openSUSE and Fedora, `linux-headers` on Arch).

The GV-USB2 is a Japanese device, and NTSC-J has no 7.5 IRE setup. A US tape captured as NTSC-J has lifted blacks. PingDub defaults to NTSC-M unless you pick otherwise.

## Supported hardware

Support is decided at runtime by asking the driver, never by a hardcoded list. The devices below are the ones PingDub knows quirks for.

| Device | Driver | Notes |
|---|---|---|
| I-O Data GV-USB2 | out of tree (above) | Primary test device. Hotplug while running is handled. |
| EasyCap, Fushicai USBTV007 | `usbtv` | Audio works; old guides saying it does not are out of date. |
| EasyCap, Syntek STK1160 | `stk1160` | Silent until the mixer source is Line. PingDub sets it. |
| EasyCap, Empia EM2860/2861 (also Elgato, Dazzle, Pinnacle) | `em28xx` | Audio through `em28xx-alsa`. |
| Hauppauge USB-Live2 | `cx231xx` | |
| Bt878, CX2388x, SAA7134, CX23885 PCI cards | `bttv`, `cx88`, `saa7134`, `cx23885` | Many Bt878 cards have no audio capture: run a cable to the sound card's line in and pick that device in **Audio**. |
| Hauppauge PVR (cx18, ivtv) | `cx18`, `ivtv` | Use the raw YUV node (HM12), never the MPEG node. |
| Magewell Pro Capture | vendor driver | Works through the normal path. |
| Generic UVC "AV to USB" dongles | `uvcvideo` | Many scale, deinterlace or MJPEG-compress inside. PingDub labels MJPEG-only modes "not archival". |
| Somagic EasyCap (DC60, EasyCAP002) | PingDub userspace backend | **Experimental, untested on real hardware.** |
| Blackmagic DeckLink / Intensity (analog in) | DeckLink API | **Experimental, untested on real hardware.** Needs Desktop Video. |

When a known device is plugged in with no driver bound, PingDub says what it is, which driver it needs, and whether that driver is in mainline.

### Somagic EasyCap setup

There is no mainline driver and the 2013 kernel driver never landed, so PingDub talks to Somagic EasyCaps over libusb, porting the protocol code of the somagic-easycap tools.

1. Get `SmiUsbGrabber3C.sys` from the Windows driver that came with the device, since the firmware lives inside it and cannot be redistributed.
2. Open **Tools > Diagnostics > Somagic setup**.
3. Click **Extract firmware** and pick the `.sys` file. It is stored in `~/.local/share/pingdub/firmware/`.
4. Click **Install udev rule**. PingDub shows the rule text first, then writes `/etc/udev/rules.d/70-pingdub-somagic.rules` through pkexec.
5. Click **Upload firmware** each time you plug the device in, since it shows up as `1c88:0007` until then.

### Blackmagic

PingDub loads `libDeckLinkAPI.so` at runtime. If Desktop Video is not installed, the backend does not appear. Capture uses the analog input in 10-bit 4:2:2 where the card offers it, with 48 kHz audio and the card's own stream clock.

## Using it

### Main window

- **Preview** in the middle. Weave by default so you see the real fields; bob and blend are under **View** and affect the screen only, never the file. Pause the preview during long captures to save CPU.
- **Side panel**: device, profile, input, standard, format, audio, sync mode, codec, destination. Read-only while capturing.
- **Meters**: peak and RMS per channel in dBFS with peak hold. The clip light stays on until you click the meters.
- **Histogram**: luma with 16 and 235 marked and the pixel counts outside them, so you can set brightness and contrast without crushing or clipping. Double-click for a luma waveform.
- **Stats strip**: frames captured, inserted, dropped, driver drops, buffer overflow, xruns, A/V offset, drift, data rate, file size, disk free and time left, RAM buffer fill, signal lock, detected standard. Anything abnormal turns amber, then red.
- **Caption readout** under the preview shows CC1 live when the driver has a VBI node.

### Dialogs (Capture menu)

- **Video Source**: input, standard, pixel format, field order override, pixel aspect override, and every driver control (brightness, contrast, saturation, hue, plus driver-private ones like chroma AGC or comb filter). Controls apply live.
- **Audio**: device, rate, mixer controls, test meter.
- **Timing**: sync mode, gap fill (last frame or black), V4L2 buffer count, RAM buffer size.
- **Output**: container, codec, audio codec, folder, segmenting, overflow folder.
- **Stop Conditions**: duration, file size, free disk, dropped frame rate, signal lost for N seconds (decks that output nothing at the end of a tape), static picture for N seconds (decks that show a blue or black screen).
- **Timed Capture**: start at a clock time or after a delay, stop after 30, 60, 120, 160, 240 or 360 minutes. The file ends at exactly that length.

### Profiles

Settings are saved per device and per named profile, so plugging in the GV-USB2 brings back its input, standard, controls and audio setup. **File > Save profile as** names one ("VCR S-video NTSC"); you can make it the device's default. Profiles live in `~/.config/pingdub/profiles/*.ini`, app settings in `~/.config/pingdub/pingdub.ini`. Export and import are file copies.

### Keys

F5 starts, Esc stops. Both can be changed in **Tools > Preferences**.

### Themes

Light, Dark, Kawaii Light, Kawaii Dark and Copper Deep-Sea, switched live in Preferences. The first run picks Light or Dark from your desktop. The preview surround is always neutral gray in every theme, and meter colors keep their meaning: green OK, amber warning, red clip or failure. To add your own, copy one of the files in `src/ui/themes/` to `~/.config/pingdub/themes/`, change the colors, and it shows up in Preferences. No code needed.

## What ends up on disk

Default output is FFV1 4:2:2 video and PCM audio in Matroska, tagged so HandBrake reads field order, aspect and color with no manual settings.

| Codec | Size | Notes |
|---|---|---|
| FFV1 v3 (default) | Smallest | Level 3, every frame a keyframe, range coder with the larger context model, slice CRCs so damage is detectable, slices matched to your cores. |
| UT Video | Larger | Faster. For slow CPUs or laptops. No 10-bit. |
| FFVHuff / HuffYUV | Larger | VirtualDub classic. HuffYUV is used in AVI when the format allows. |
| Uncompressed | About 75 GB/hour | Fallback when nothing else keeps up. Stored as the packed YUYV the driver delivered. |

Chroma and bit depth always match the capture: YUYV becomes yuv422p, HM12 and NV12 stay 4:2:0, GREY stays gray, 10-bit stays 10-bit. Nothing is ever downgraded for the encoder's convenience. MJPEG from a dongle is stored exactly as the dongle sent it.

Tags written into the file:

- **Field order** from the driver's field type (Matroska FlagInterlaced and FieldOrder). Plain V4L2 INTERLACED means BFF for NTSC and TFF for everything else, per the V4L2 spec. Some devices lie, so the field order override is in Video Source.
- **Pixel aspect** from how the device samples a line: 10:11 for 525-line, 12:11 for 625-line, 1:1 for square-pixel devices (640 or 768 wide). It sets display geometry only and crops nothing.
- **Color**: BT.601 limited range, smpte170m for 525-line, bt470bg for 625-line.
- **Totals and context**: device, driver, input, standard, sync mode, measured drift, inserted, dropped, driver drops, buffer overflow and xrun totals, start and end time, PingDub version, tape label, XDS program name and rating when present.

Sidecars next to the file:

- `<name>.log.csv`: every insert, drop, xrun, signal change and segment switch with its timeline position. Columns: `wallclock,timeline_frame,timeline_time,event,count,detail`.
- `<name>.json`: settings, device, formats, totals, warnings.
- `<name>.vbi`: raw VBI lines with frame numbers, the master copy of everything in the blanking interval. Format: the bytes `PINGDUBVBI` plus two version bytes, a little-endian u32 header length, a JSON header with the sampling parameters, then records of `u64 timeline_frame, u32 size, bytes`.
- `<name>.cc1.scc`: Scenarist captions with positioning and styling, imports into most subtitle tools.
- `<name>.ccN.srt`: plain text, one file per caption channel that had data.

Segments (by size or duration, in Output) are frame- and sample-continuous, so joining them back loses nothing. The overflow folder is VirtualDub's spill: when free space on the first drive drops below the threshold, the next segment goes to the other drive.

### Frame area

This is the master file, so PingDub asks the driver for its full crop bounds and the largest unscaled size for the standard (720x486 over 720x480 when offered) and records no crop metadata. What is in the file is what gets shown. Crop in HandBrake.

## Timing and sync

Video and audio run on different clocks. The video decoder locks to the tape's sync pulses; the audio converter runs off its own cheap crystal. A 50 ppm mismatch is about 1 second of drift over a 6-hour tape.

| Mode | Video | Audio | Use it when |
|---|---|---|---|
| Video master (default) | Every frame kept | Resampled by a few ppm so its length matches the frame count | Almost always |
| Audio master | Frames repeated or dropped to match the audio | Bit-exact | Music sources where you want the exact samples |
| No correction | As delivered | As delivered | Diagnosing a device; the streams will drift |

Video master measures the A/V offset on the shared monotonic clock and steers a libsoxr variable-rate resampler with a slow loop (about 30 seconds), clamped to plus or minus 0.5%, so timestamp jitter never reaches the pitch. If the ratio hits the clamp, something is wrong (wrong audio device, broken source) and PingDub warns. Gaps in the driver's sequence numbers or timestamps get repeats of the last good frame; when the driver stops delivering altogether (blank tape, signal loss), PingDub keeps filling at the nominal rate so audio stays aligned. Overruns on the audio side are filled with exactly as much silence as was lost.

The test suite runs six simulated hours at +100 and -100 ppm with timestamp jitter, driver drops, a signal loss and audio xruns. A tone placed one minute before the end lands within 0.1 ms of where it belongs, and every injected fault is counted exactly once.

## Reliability

A capture cannot be redone without wearing the tape again, so every failure ends in a playable file and a log entry.

- A 2 GB RAM buffer (256 MB to 16 GB) sits between capture and encoder to absorb disk stalls. If it ever fills, frames are dropped at the buffer, counted as "Buffer ovf", and the strip goes red.
- Matroska clusters are cut every second and synced to disk every second, so a crash or power cut loses at most the last second.
- **File > Recover** remuxes a truncated file to rebuild its index. Nothing is re-encoded.
- Disk full, device unplugged, driver crash: the capture stops, the file is finalized, and the log says what happened.
- Device busy: PingDub names the process holding `/dev/videoN` or the ALSA device.
- No permission: PingDub says which group is missing and gives the command.
- USB 1.1 link: warned before capture, since it cannot carry uncompressed SD.
- Video and audio threads ask rtkit for realtime priority. If that fails, PingDub says so once and carries on.

Before each capture a preflight checks estimated data rate times planned duration against free space, signal present, standard matching detection, and audio delivering samples. These are warnings, not blocks.

## Building from source

Prerequisites on Kubuntu 24.04 (I use apt, so in my case this is the line; it depends on your system):

```
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev libqt6opengl6-dev \
  libgl-dev libavcodec-dev libavformat-dev libavutil-dev libsoxr-dev libzvbi-dev \
  libasound2-dev libudev-dev libusb-1.0-0-dev libdbus-1-dev ffmpeg
```

1. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
2. `cmake --build build`
3. `cd build && ctest --output-on-failure`, since the sync test takes a couple of minutes (six simulated hours, twice).
4. `./build/pingdub --self-test` runs a real 10-second capture through test-pattern devices and checks the file.
5. `./build/pingdub` to run it. `PINGDUB_FAKE_DEVICES=1 ./build/pingdub` adds a test-pattern device if you want to try the UI with no hardware.

To build the AppImage the same way CI does: `packaging/build-appimage.sh`. It downloads the latest Qt 6 with aqtinstall and builds the latest FFmpeg, libsoxr and zvbi from source. Add `USE_SYSTEM_QT=1` to use your distro's Qt instead.

### CI

`.github/workflows/build.yml` builds `PingDub-x86_64.AppImage` inside an `ubuntu:22.04` container on every push, runs the tests and a smoke test of the AppImage, uploads it with its `.zsync` as a workflow artifact, and publishes both to a Release on every `v*` tag.

Qt, FFmpeg, libsoxr, zvbi, linuxdeploy and appimagetool are not pinned: each build takes the latest release. A new upstream release can break a build; when it does, the fix is a code change and the workflow log shows the cause. The GitHub Actions themselves are referenced by major version, since there is no other way to name them.

The build fails if `libasound`, `libudev`, `libGL`, `libEGL`, `libdbus-1` or `libc` end up inside the AppImage. A bundled alsa-lib cannot load the host's ALSA plugins, which breaks the pipewire PCM and any `.asoundrc`.

## Tests

| Test | What it checks |
|---|---|
| `test_sync` | Sync engine: 6 hours at plus and minus 100 ppm, audio master, no correction, gap fill, stride detection, ring drops. |
| `test_ring` | RAM ring and buffer pool: order, capacity, overflow, threads, back-pressure. |
| `test_output` | Every codec and container written and read back with ffprobe: pixel format, size, frame rate, field order, SAR, color tags, totals in tags; first frame decoded and compared bit for bit; segments; recover. |
| `test_v4l2_parse` | Format preference, field order mapping, SAR, standards, frame size choice, YUYV/UYVY/v210/Y210/HM12 repacks, field weaving. |
| `test_vbi` | Synthetic line 21 waveforms through zvbi into SCC and SRT, with timing. |
| `test_somagic_parse` | A hand-built Somagic stream (BT.656 plus audio blocks) parsed back exactly; firmware search. |

## Known limits

- I have not run the hardware acceptance tests (2-hour GV-USB2 capture, HandBrake detection, unplug and kill -9 on real hardware) from this build environment; CI has no capture hardware. Those need a GV-USB2 driver that builds on your kernel, and none is confirmed on current kernels yet.
- The Somagic and DeckLink backends compile, pass their unit tests, and stay hidden when their hardware or library is absent, but nobody has run them on real hardware. They are marked experimental in the UI.
- The HM12 detiler follows the documented ivtv/cx18 tile layout; it has not been checked against a real cx18 card.
- Preview uses QOpenGLWidget rather than QRhiWidget. It runs on every Qt 6 release and needs no shader baking step in the build; the YUV to RGB work is still done in a shader.
- AVI cannot carry field order reliably; PingDub writes it to the sidecar instead and warns.
- Drivers that only deliver alternating single fields (V4L2_FIELD_ALTERNATE) are refused for now.

## License

GPL-3.0-or-later. The Somagic protocol code is ported from somagic-easycap (GPL-2.0-or-later). zvbi is GPL-2.0-or-later. The vendored DeckLink SDK headers in `third_party/decklink/` carry Blackmagic's permissive license, included in each file.

Now PingDub is installed, your device is picked up with its audio, captures land in `~/Videos/PingDub/` as lossless FFV1 with every frame accounted for, and HandBrake opens them with the right field order and aspect. Tinker with profiles, stop conditions and themes as you see fit.
