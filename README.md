# PingDub

PingDub captures analog video (composite or S-video) on Linux and writes the untouched video and audio to disk with correct timing, for anyone archiving VHS, S-VHS, Hi8, Betamax, LaserDisc, camcorders or old consoles.

It is the capture mode of VirtualDub 1.10.4 and VirtualDub2, rewritten natively for Linux. No VirtualDub code is reused. What carries over is the feature set (preview, audio meters, histogram, timing control, auto-stop conditions, spill to another drive) and the timing model. On top of that, PingDub helps get a tape into a good master file and proves it stayed good: it measures and sets levels before the capture, warns about crushed blacks and hi-fi audio faults during it, and leaves a report, chapters, a contact sheet and checksums after it. Nothing it does changes the picture or sound after capture. Deinterlacing, cropping, denoise and encoding to something small happen later, in whatever tool you pick; I use HandBrake.

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
5. Play the tape, click **Measure Levels**, let it run, then **Set Levels** (see [Setting levels](#setting-levels)). Rewind.
6. Type a tape label, press play, press **F5**.
7. Press **Esc** to stop. After the first minute it asks first, so a stray key does not end a 4-hour capture.
8. The capture report opens. "Clean" means nothing needs a look; "Review 4 spots" tells you where to look.

Files land in `~/Videos/PingDub/` as `YYYY-MM-DD_HHMMSS_<label>.mkv` with sidecars next to them. Set the folder to `~/Videos/PingDub/{title}/` in **Capture > Output** and each tape gets its own folder (see [Output folder templates](#output-folder-templates)).

## GV-USB2 driver

The I-O Data GV-USB2 is the device I capture with. Its driver is not in the mainline kernel, so it builds against your running kernel. I maintain it at [NullAngst/GV-USB2-Driver](https://github.com/NullAngst/GV-USB2-Driver), and its scripts handle the build and the module install.

**REBUILD THE MODULE AFTER EVERY KERNEL UPDATE.** If you skip this, the device shows up with no driver after the next reboot and PingDub tells you exactly that.

1. `git clone https://github.com/NullAngst/GV-USB2-Driver.git`
2. `cd GV-USB2-Driver`
3. `scripts/install.sh`, or `scripts/install.sh -d` if you don't have kernel headers and a compiler yet, since that installs them first.
4. Plug in the device. **Tools > Diagnostics** should show it bound to both the video and the sound driver.
5. After every kernel update: `scripts/update.sh` in the same folder.

I use apt, so in my case `install.sh -d` pulls `linux-headers-$(uname -r)`. It depends on your system: the script knows the header package for Fedora, Arch and openSUSE too.

Use a driver build from 2026-10-24 or later. Earlier builds stamped each frame with the time a 32 ms USB transfer finished instead of when the frame ended, and didn't report frames they lost. PingDub copes with the coarse timestamps (see Sync below), but the newer driver gives it exact ones.

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
- **Levels panel**: Measure Levels, Set Levels and Revert, with the live readout of time measured, black point, white point and peak saturation against their targets.
- **Scopes**, picked with the tabs or from **View**:
  - **Histogram**: luma with 16 and 235 marked and the pixel counts outside them.
  - **Waveform**: luma per column of the picture, 0 to 255, with lines at 16 and 235. **View > Waveform parade** shows Y, Cb and Cr side by side.
  - **Vectorscope**: Cb across, Cr up, with target boxes for 75% color bars and the circle of the chroma limit.

  The waveform and vectorscope draw from a downsampled copy of each frame, made on the analysis thread at preview rate. Capture never waits on them.
- **Stats strip**: frames captured, inserted, dropped, driver drops, buffer overflow, xruns, A/V offset, drift, data rate, file size, disk free and time left, RAM buffer fill, signal lock, detected standard, Levels (crush, clip and chroma warnings) and Audio (hi-fi faults found). Anything abnormal turns amber, then red. While a capture finishes, State says what it is doing: analysis, finalizing the file, checksum with a percentage, sidecars.
- **Caption readout** under the preview shows CC1 live when the driver has a VBI node.

### Dialogs (Capture menu)

- **Video Source**: input, standard, pixel format, field order override, pixel aspect override, and every driver control (brightness, contrast, saturation, hue, plus driver-private ones like chroma AGC or comb filter). Controls apply live.
- **Audio**: device, rate, mixer controls, test meter.
- **Timing**: sync mode, gap fill (last frame or black), V4L2 buffer count, RAM buffer size.
- **Output**: container, codec, audio codec, folder (with tokens), segmenting, overflow folder, chapters and contact sheet.
- **Stop Conditions**: duration, file size, free disk, dropped frame rate, signal lost for N seconds (decks that output nothing at the end of a tape), static picture for N seconds (decks that show a blue or black screen).
- **Timed Capture**: start at a clock time or after a delay, stop after 30, 60, 120, 160, 240 or 360 minutes. The file ends at exactly that length.
- **Measure Levels**, **Set Levels**, **Revert levels**: the same as the buttons in the levels panel.

**File** has **Open capture folder** (the last capture's folder), **Open report**, **Verify capture** and **Recover**. The last three list every capture under your output folder, newest first, with Browse for anything elsewhere.

**Tools > Preferences** has the theme, keys, the stop confirmation, VBI, the desktop notification, and the crush and clip warning thresholds.

### Profiles

Settings are saved per device and per named profile, so plugging in the GV-USB2 brings back its input, standard, controls (including what Set Levels chose), audio setup, output folder template, chapter and contact sheet settings. **File > Save profile as** names one ("VCR S-video NTSC"); you can make it the device's default. Profiles live in `~/.config/pingdub/profiles/*.ini`, app settings in `~/.config/pingdub/pingdub.ini`. Export and import are file copies.

### Keys

F5 starts, Esc stops. Both can be changed in **Tools > Preferences**.

### Themes

Light, Dark, Kawaii Light, Kawaii Dark and Copper Deep-Sea, switched live in Preferences. The first run picks Light or Dark from your desktop. The preview surround is always neutral gray in every theme, and meter colors keep their meaning: green OK, amber warning, red clip or failure. To add your own, copy one of the files in `src/ui/themes/` to `~/.config/pingdub/themes/`, change the colors, and it shows up in Preferences. No code needed.

## Setting levels

Brightness, contrast and saturation are applied in the capture chip before the picture is digitized. That makes the time before a capture the only place a crushed black or a blown white can be avoided: once the chip has clipped it, no software afterward gets it back. So PingDub measures while the tape plays and sets the levels then. Nothing is written to disk, so no tape is used up except the minutes you play.

1. Play the tape in preview.
2. Click **Measure Levels**. PingDub starts collecting.
3. Let it run as long as you like. Fast-forward to other parts of the tape and play again if you want; the measurement adds up. Search and fast-forward frames usually have no signal lock, so they drop out on their own.
4. Click **Set Levels** while the tape is still playing. The dialog shows the current and suggested values. **Apply** sets them, measures 3 seconds of live video, and corrects up to twice. Keep the tape playing until it says it is on target.
5. **Keep** saves the values into the profile for that device and input. **Revert** (in the dialog or the panel) puts back what was there before.
6. Rewind, press play, start the capture.

What it measures, over the active picture only:

- **Black and white points**: the 0.1st and 99.9th percentile of luma, so a few noisy pixels don't decide the result. Targets 16 and 235.
- **Peak saturation**: the 99.9th percentile of chroma distance from neutral. The limit is 112 by default, the edge of the 16 to 240 chroma code range (75% color bars sit near 90). It can be changed in Preferences.
- **Left out**: the bottom 8 lines (head-switching noise), dark border columns at the sides (up to 1/24 of the width) and dark rows at the top, frames without signal lock, frames the driver flagged as damaged, and featureless frames such as a VCR's blue or black screen.

How it gets there: most chips apply brightness as an offset and contrast as a gain, but the step per control unit differs per chip, and some add the offset before the gain, some after. The solver reads each control's range from the driver, starts from a guess (the whole brightness range spans about the whole code range, contrast is a gain from its minimum), and fits every reading it gets to a model that covers both orders. The first step goes half the way, so a chip that steps harder than guessed lands past the target instead of crushing into its rail. By the third reading the model rests on measurements alone. On the simulated chips in the test suite (offsets before and after the gain, steps from half to twice the guess, 16-bit controls with coarse steps, a start already crushed) it lands within 2 codes of 16 and 3 codes of 235 in at most two corrections. Saturation only ever comes down, when the peak is past the limit; a pale tape stays pale.

Warnings the dialog gives:

- **Narrow range**: the sample never got near both black and white (a dark scene only). It says so and suggests measuring a section with both. The button becomes **Apply anyway**.
- **Already clipping**: if blacks are crushed at the current settings, the true black point is unknown. PingDub raises brightness one step and keeps measuring; click Set Levels again after a few more seconds. Clipped whites get the same treatment with contrast.
- **Out of range**: when a control hits the end of what the driver allows, it says the target may be out of reach on this device.

Levels are set in preview only. A capture never adjusts itself, and the levels panel is locked while one runs.

## During a capture

### Crush and clip warnings

More than 0.5% of the active picture below 16, above 235, or past the chroma limit, for 3 seconds or longer, is a warning. The Levels stat turns amber and the status bar says what and since when, for example "Blacks crushed since 0:12:04 (1.3% below 16)". Each episode becomes one event in the `.log.csv` (`levels_crush`, `levels_clip`, `levels_chroma`) with its start, end and worst value, and the JSON counts them. An episode ends after a second back under the threshold. Both numbers are in Preferences.

This is not a stop condition. The capture keeps going; the warning tells you a re-capture with new levels may be worth it.

### Audio checks

VHS hi-fi audio fails in ways the level meters don't show. PingDub watches the audio as it goes into the file and logs each fault as an event. The audio is never changed.

- **Hi-fi dropouts**: short gaps or bursts when tracking is marginal. A level step of 20 dB or more, down or up, lasting 5 to 200 ms with sharp edges on both sides. The level is a 4 ms RMS stepped every millisecond, so a natural decay (a drum, a reverb tail) is too slow to count as an edge. Gaps the sync engine filled for an xrun are already counted as xruns and are not counted again.
- **Dead channel**: one channel 30 dB or more below the other for 10 seconds. Usually a mono tape on a stereo deck, or a bad cable.
- **Phase problem**: left and right correlation below -0.5 for 10 seconds. One channel is inverted, and the sound cancels in mono.

Seconds that are near silence count for neither of the last two, so the gaps between songs don't split one fault into several. They show in the stats strip as the Audio count, in the log (`audio_dropout`, `audio_dead_channel`, `audio_phase`) and in the capture report. The thresholds are a first guess; they get tuned on real tapes.

### When a capture stops on its own

Captures run for hours unattended, so when one ends without you pressing Stop (a stop condition fired, the timer ran out, the device was lost or the disk filled), PingDub sends a desktop notification with why it stopped, the length, and the inserted, dropped and warning counts. It goes through the freedesktop notification service over D-Bus, which KDE shows as a normal notification. PingDub already links libdbus for realtime priority, so there is no new dependency. On by default; the checkbox is in Preferences.

## After a capture

### Capture report

The report answers "is this capture good, or do I re-run it" in seconds. It opens after each capture, and from **File > Open report** for any PingDub capture, reading its `.json` and `.log.csv` sidecars.

- **Verdict**: "Clean", or "Review 4 spots" with the totals beside it. Events within 5 seconds of each other count as one spot.
- **Timeline**: the whole capture left to right, one lane each for video (inserts, driver drops, buffer overflow, signal loss, damaged frames), levels (crush, clip, chroma) and audio (dropouts, dead channel, phase, xruns). Chapter marks run along the top.
- **Thumbnails**: a strip cut from the contact sheet, so you can see where you are.
- **Click** an event, a thumbnail or the timeline: that frame decoded from the file, its timecode, the event details. **Copy timecode** copies it.
- **Verify** and **Open folder** buttons.

Captures made before this update have no thumbnails and no level or audio events. The report shows what their sidecars hold.

### Chapters

Chapters go into the MKV as Matroska chapters. The file stays one untouched master, and any player can jump between them. Two sources, set separately in **Capture > Output**, usable together:

| Source | Setting | Chapter name |
|---|---|---|
| Fixed interval | Off, 5, 10, 15 or 30 minutes | Chapter 03 (00:20:00) |
| Recording boundaries | Off or On | Recording 2 (00:41:13) |

- **Recording boundary**: a sync loss or blank stretch of 0.5 seconds or more, followed by picture again. That's where someone pressed stop, then record. A fade to black that long counts too; the next rule cleans most of those up.
- **Minimum length**: a chapter shorter than 30 seconds merges into the one before it, so a glitchy section doesn't produce a run of tiny chapters.
- **Both on**: interval marks and boundary marks share one list, in time order. An interval mark within 10 seconds of a boundary is dropped. With boundaries on, the first chapter is "Recording 1".
- **Frame accurate**: each mark sits on a timeline slot.
- **AVI** has no chapters. PingDub writes them to a `.chapters.txt` sidecar in FFmpeg metadata format and says so in the log.
- **Segments**: each file gets the chapters inside it, timed from its own start. A chapter is decided when its file closes, so a boundary within 30 seconds after a split can't merge back into the file before.

FFmpeg's Matroska muxer writes chapters added after the header into the trailer, which is what PingDub relies on: the list is only complete at the end. After each file closes PingDub reads it back, and if a future FFmpeg ever drops them, the same list goes into a `.chapters.txt` with a warning. The test suite checks the files with `ffprobe -show_chapters`.

### Output folder templates

The folder in **Capture > Output** accepts tokens. With `~/Videos/PingDub/{title}/`, each tape's capture and sidecars land in a folder named after its tape label.

| Token | Becomes | Example |
|---|---|---|
| `{title}` | Tape label | Wedding 1994 |
| `{date}` | Capture start date | 2026-10-25 |
| `{year}` | Year of the start date | 2026 |
| `{month}` | Month of the start date | 10 |
| `{time}` | Start time | 174800 |
| `{device}` | Device name | gvusb2 |
| `{input}` | Input | S-Video |
| `{profile}` | Profile name | VCR S-video NTSC |

- **Resolved once**, when the capture starts (timed captures included), so a capture never splits across folders. Missing folders are created. The overflow folder takes the same tokens.
- **Empty tape label**: `{title}` becomes `Untitled`, and the preflight check warns before the capture starts.
- **Unsafe characters**: `/` and control characters in a value become `_`, so `AC/DC live` becomes `AC_DC live`. Leading and trailing spaces and dots are trimmed, and each value is capped at 100 characters. An empty device, input or profile becomes `Unknown`.
- **Unknown token**: Output settings shows the error and won't save the path.
- **Preview**: Output settings shows the resolved path for the current tape label as you type.
- **File names** are unchanged, `YYYY-MM-DD_HHMMSS_<label>.mkv`, inside the resolved folder, with sidecars next to the MKV. The time stamp keeps two captures with the same label from colliding.
- **Disk space**: the free-space check and Time left use the resolved folder, or its nearest existing parent before it's created.
- **Recorded**: each capture's resolved folder goes in its JSON sidecar (`output_folder`).
- **Finding captures**: **File > Open capture folder** opens the last capture's folder. Recover, Verify and Open report search the base folder (the part before the first token) and every folder below it.

### Checksums and Verify

Every capture gets checksums at two levels, and **File > Verify capture** checks a file against them years later, after a copy to another drive or a restore from backup.

- **Per frame**: an MD5 of each frame's raw picture before encoding, in a `.framemd5` sidecar in the same format as `ffmpeg -i file.mkv -map 0:v:0 -f framemd5 -`. The codecs are lossless, so decoding the file must reproduce every hash. Video only: the audio packet layout depends on the demuxer, so it is left out. Cost: MD5 of about 21 MB/s of NTSC video is a few percent of one core, on the writer thread.
- **Per file**: a SHA-256 of the finished file, in a `.sha256` sidecar that `sha256sum -c` checks. It is computed when the capture ends, after the trailer and the totals are written, so it covers the final bytes. That adds one full read of the file to Finishing. With the CPU's SHA instructions (every Ryzen, Intel from Ice Lake) it runs at about 1 GB/s, so the disk sets the pace: a 60 GB tape on a hard drive at 150 MB/s adds about 7 minutes, on an SSD under a minute. Without them it falls back to FFmpeg's C version at about 230 MB/s.
- **Quick verify** recomputes the SHA-256. It catches any change to the file.
- **Full verify** also decodes every frame and compares it with the `.framemd5`, with a progress bar. If the file was damaged it names the exact frames, with their timecodes, and it reports FFV1 slice CRC errors.

A capture split into segments has a pair of sidecars per file, and Verify checks them all.

To check by hand: `sha256sum -c name.sha256` in the folder, and `ffmpeg -i name.mkv -map 0:v:0 -f framemd5 - | diff - name.framemd5`. The `#software` line differs if your FFmpeg is a different version; the frame lines must not.

### Contact sheet

A grid of thumbnails saved next to the MKV shows what's on a tape without opening it.

- **Interval**: one thumbnail every 30 seconds by default, set in Output settings (Off, 10, 15, 30, 60 or 120 seconds).
- **Images**: 320 pixels wide with the aspect ratio corrected, timecode under each, 6 across and 10 down. A 2-hour tape fills four JPEG pages of 60 thumbnails, `<name>.contact01.jpg` and up.
- **Source**: taken from the frames the analysis thread already has in memory during the capture, so nothing gets decoded again afterward. Pages are written as they fill, so a crash keeps the finished ones.
- **Reused**: the capture report's thumbnail strip is cut from the same images.

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
- `<name>.framemd5`: per-frame MD5s, one line per timeline frame (not for MJPEG, which is stored as the device sent it).
- `<name>.sha256`: the SHA-256 of the finished file.
- `<name>.contactNN.jpg`: contact sheet pages.
- `<name>.chapters.txt`: chapters for AVI, in FFmpeg metadata format.

The JSON also holds the resolved output folder, the folder template, the segment list with each file's first frame, the chapters, the contact sheet layout, and counts of level and audio events.

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

Video master measures the A/V offset on the shared monotonic clock and steers a libsoxr variable-rate resampler with a slow loop (about 30 seconds), clamped to plus or minus 0.5%, so timestamp jitter never reaches the pitch. If the ratio hits the clamp, something is wrong (wrong audio device, broken source) and PingDub warns. Gaps in the driver's sequence numbers or timestamps get repeats of the last good frame. Timestamps are judged against the whole timeline, with the driver's timestamp jitter learned as it goes, so a driver that stamps frames in coarse steps (a USB transfer at a time) does not produce false gaps; when the driver stops delivering altogether (blank tape, signal loss), PingDub keeps filling at the nominal rate so audio stays aligned. Overruns on the audio side are filled with exactly as much silence as was lost.

The test suite runs six simulated hours at +100 and -100 ppm with timestamp jitter, driver drops, a signal loss and audio xruns. A tone placed one minute before the end lands within 0.1 ms of where it belongs, and every injected fault is counted exactly once. A separate case feeds ten minutes of frames stamped on a 32 ms USB transfer grid, like older GV-USB2 driver builds, and checks that only the frames actually lost are inserted.

## Reliability

A capture cannot be redone without wearing the tape again, so every failure ends in a playable file and a log entry.

- A 2 GB RAM buffer (256 MB to 16 GB) sits between capture and encoder to absorb disk stalls. If it ever fills, frames are dropped at the buffer, counted as "Buffer ovf", and the strip goes red.
- Matroska clusters are cut every second and synced to disk every second, so a crash or power cut loses at most the last second.
- **File > Recover** remuxes a truncated file to rebuild its index. Nothing is re-encoded.
- Disk full, device unplugged, driver crash: the capture stops, the file is finalized, the log says what happened, and the desktop gets a notification.
- Device busy: PingDub names the process holding `/dev/videoN` or the ALSA device.
- No permission: PingDub says which group is missing and gives the command.
- USB 1.1 link: warned before capture, since it cannot carry uncompressed SD.
- Video and audio threads ask rtkit for realtime priority. If that fails, PingDub says so once and carries on.

Before each capture a preflight checks estimated data rate times planned duration against free space, signal present, standard matching detection, audio delivering samples, the output folder template, and an empty tape label with `{title}` in the folder. These are warnings, not blocks; a folder template with an unknown token is the one thing that stops a capture from starting.

The analysis thread (scopes, Level Measure, warnings, boundaries, contact sheet, audio checks) reads the timeline through queues that drop instead of blocking. If it ever falls behind, the JSON counts the frames it skipped; the capture itself is never held up.

## Building from source

Prerequisites on Kubuntu 24.04 (I use apt, so in my case this is the line; it depends on your system):

```
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev libqt6opengl6-dev \
  libgl-dev libavcodec-dev libavformat-dev libavutil-dev libsoxr-dev libzvbi-dev \
  libasound2-dev libudev-dev libusb-1.0-0-dev libdbus-1-dev ffmpeg
```

1. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
2. `cmake --build build`
3. `cd build && ctest --output-on-failure`, since the sync test takes a couple of minutes (six simulated hours, twice) and the chapter test writes eleven minutes of video. The report test builds the real window on Qt's offscreen platform; ctest sets `QT_QPA_PLATFORM=offscreen` for it.
4. `./build/pingdub --self-test` runs a real 10-second capture through test-pattern devices and checks the file.
5. `./build/pingdub` to run it. `PINGDUB_FAKE_DEVICES=1 ./build/pingdub` adds a test-pattern device if you want to try the UI with no hardware.

To build the AppImage the same way CI does: `bash packaging/build-appimage.sh`. It downloads the latest Qt 6 with aqtinstall and builds the latest FFmpeg, libsoxr and zvbi from source. The FFmpeg build is trimmed to what PingDub uses; this update added the MJPEG encoder (contact sheets) and the framemd5 muxer (the reference for the checksum test). The latest CMake and aqtinstall go into a Python venv under `build-appimage/tools`, since current Qt needs a newer CMake than Ubuntu 22.04 ships, so you also need `python3-venv`. Add `USE_SYSTEM_QT=1` to use your distro's Qt instead.

### CI

`.github/workflows/build.yml` builds `PingDub-x86_64.AppImage` inside an `ubuntu:22.04` container on every push, runs the tests and a smoke test of the AppImage, uploads it with its `.zsync` as a workflow artifact, and publishes both to a Release on every `v*` tag.

Qt, CMake, FFmpeg, libsoxr, zvbi, linuxdeploy and appimagetool are not pinned: each build takes the latest release. A new upstream release can break a build; when it does, the fix is a code change and the workflow log shows the cause. The GitHub Actions themselves are referenced by major version, since there is no other way to name them.

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
| `test_analyzer` | Frame analyzer on synthetic frames (ramps, color bars, black borders, head-switching noise) in YUYV, UYVY, NV12 and GREY: exact percentiles and the right active area; clip detection; crush episode timing. Set Levels against simulated proc amps with different step sizes, the offset before or after the gain, and clipping rails: on target within 2 corrections. |
| `test_audio_checks` | Injected dropouts, a burst, a dead channel and an inverted channel each found exactly once; a 3 ms gap, a 400 ms gap and xrun silence not counted; 6 simulated hours of clean program give zero events. |
| `test_chapters` | Chapter rules, then `ffprobe -show_chapters` on MKVs with interval only, boundaries only, both, and split into segments. Every mark frame accurate. AVI writes the sidecar. |
| `test_templates` | Every token resolves; an empty label, a label with `/`, control characters, long values and unknown tokens; Recover finds captures in template subfolders. |
| `test_checksums` | SHA-256 against FFmpeg's at every length (both code paths: `PINGDUB_NO_SHA_NI=1` forces the C one). The `.framemd5` sidecar equals FFmpeg's framemd5 of the file for every codec and container (and `ffmpeg -f framemd5` when the program is installed); Verify catches a single flipped byte and names the frame; `sha256sum -c` agrees. |
| `test_report` | A capture made before this update opens without errors; a new capture through the real session gets its folder from `{title}`, a contact sheet, checksums and a boundary; spots, CSV parsing, frame decoding. |

`--self-test` now also checks the `.framemd5`, the `.sha256`, a full Verify, the contact sheet, zero audio faults on the clean test tone, and that the report reads the sidecars.

On the GV-USB2, still to do by hand: Level Measure on a real tape, a 2-hour capture with both chapter types into a `{title}` folder, and Verify after copying the file to another drive.

## Known limits

- The GV-USB2 captures on real hardware (S-Video, NTSC). The full acceptance tests (2-hour capture, HandBrake detection, unplug and kill -9 mid-capture) have not been run yet; CI has no capture hardware.
- The Somagic and DeckLink backends compile, pass their unit tests, and stay hidden when their hardware or library is absent, but nobody has run them on real hardware. They are marked experimental in the UI.
- The HM12 detiler follows the documented ivtv/cx18 tile layout; it has not been checked against a real cx18 card.
- Preview uses QOpenGLWidget rather than QRhiWidget. It runs on every Qt 6 release and needs no shader baking step in the build; the YUV to RGB work is still done in a shader.
- AVI cannot carry field order reliably; PingDub writes it to the sidecar instead and warns.
- Drivers that only deliver alternating single fields (V4L2_FIELD_ALTERNATE) are refused for now.
- Set Levels is tested against simulated chips, not yet against the GV-USB2's decoder. If a real chip steps more than about twice as hard as the guess, the first step can land in a rail; the solver then drops the bent reading and keeps going, but may need the full two corrections or say it ended short.
- The dropout, dead channel and phase thresholds come from the update spec, not from real tapes yet. Expect them to move.
- MJPEG dongles: no scopes, no Level Measure, no crush warnings, no contact sheet and no `.framemd5`, since the frames are compressed by the device. Recording boundaries come from signal lock alone. The `.sha256` still works.
- Notifications need a session bus. Without one (a bare X session, a container) PingDub carries on silently.
- Deferred until there's hardware or a need: raw composite capture with vhs-decode (cxadc card or MISRC), multi-pass stacking of damaged tapes, batch capture, a tape catalog and a device quirks file.

## License

GPL-3.0-or-later. The Somagic protocol code is ported from somagic-easycap (GPL-2.0-or-later). zvbi is GPL-2.0-or-later. The vendored DeckLink SDK headers in `third_party/decklink/` carry Blackmagic's permissive license, included in each file.

Now PingDub is installed, your device is picked up with its audio, levels are measured and set before the tape rolls, captures land in their own folders as lossless FFV1 with every frame accounted for, chapters, a contact sheet and checksums, the report tells you whether to re-run a tape, and Verify proves years later that nothing changed. Tinker with profiles, folder templates, thresholds and themes as you see fit.
