// SPDX-License-Identifier: GPL-3.0-or-later
#include "AlsaBackend.h"

#include "engine/Clock.h"
#include "engine/SysInfo.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>

namespace pd {

AlsaBackend::AlsaBackend(const std::string& pcm) : m_pcm(pcm) {}

AlsaBackend::~AlsaBackend() { close(); }

static std::string realDir(const std::string& p) {
    char r[PATH_MAX];
    return realpath(p.c_str(), r) ? std::string(r) : std::string();
}

// alsa-lib prints configuration errors for every PCM it cannot resolve
// (no pipewire plugin, no default card). Probing is expected to fail
// sometimes, so keep that off the terminal.
static void quietAlsa(const char*, int, const char*, int, const char*, ...) {}

std::vector<AlsaDevice> AlsaBackend::enumerate() {
    std::vector<AlsaDevice> out;
    snd_lib_error_set_handler(quietAlsa);
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[32];
        std::snprintf(hw, sizeof hw, "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, hw, 0) < 0) continue;
        snd_ctl_card_info_t* info;
        snd_ctl_card_info_alloca(&info);
        if (snd_ctl_card_info(ctl, info) < 0) {
            snd_ctl_close(ctl);
            continue;
        }
        const std::string cardId = snd_ctl_card_info_get_id(info);
        const std::string cardName = snd_ctl_card_info_get_name(info);
        const std::string sysfs = realDir("/sys/class/sound/card" + std::to_string(card) + "/device");
        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
            snd_pcm_info_t* pi;
            snd_pcm_info_alloca(&pi);
            snd_pcm_info_set_device(pi, unsigned(dev));
            snd_pcm_info_set_subdevice(pi, 0);
            snd_pcm_info_set_stream(pi, SND_PCM_STREAM_CAPTURE);
            if (snd_ctl_pcm_info(ctl, pi) < 0) continue;
            AlsaDevice d;
            d.pcm = "hw:CARD=" + cardId + ",DEV=" + std::to_string(dev);
            d.name = cardName + ": " + snd_pcm_info_get_name(pi) + " (" + d.pcm + ")";
            d.cardId = cardId;
            d.cardIndex = card;
            d.sysfsDevice = sysfs;
            out.push_back(d);
        }
        snd_ctl_close(ctl);
    }
    // Sound server PCMs as fallbacks, for when hw: is busy.
    for (const char* name : {"pipewire", "default"}) {
        snd_pcm_t* h = nullptr;
        if (snd_pcm_open(&h, name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) == 0) {
            snd_pcm_close(h);
            AlsaDevice d;
            d.pcm = name;
            d.name = std::string(name) + " (sound server, may resample)";
            d.fallback = true;
            out.push_back(d);
        }
    }
    return out;
}

std::string AlsaBackend::pairedPcmFor(const std::string& videoNode) {
    const std::string name = videoNode.substr(videoNode.find_last_of('/') + 1);
    std::string vdev = realDir("/sys/class/video4linux/" + name + "/device");
    if (vdev.empty()) return {};
    // Walk up from the video interface to the USB device (or PCI function),
    // then look for a sound card that lives under it.
    std::vector<std::string> parents;
    for (std::string p = vdev; p.size() > 5; p = p.substr(0, p.find_last_of('/'))) {
        parents.push_back(p);
        if (fileExists(p + "/idVendor") || fileExists(p + "/vendor")) break;
    }
    const auto devices = enumerate();
    for (const std::string& parent : parents) {
        for (const AlsaDevice& d : devices) {
            if (d.fallback || d.sysfsDevice.empty()) continue;
            if (d.sysfsDevice == parent || d.sysfsDevice.rfind(parent + "/", 0) == 0) return d.pcm;
        }
    }
    // PCI cards often put audio on another function of the same slot
    // (bt878 video on .0, audio on .1).
    if (!parents.empty() && fileExists(parents.back() + "/vendor")) {
        const std::string fn = parents.back();
        const size_t dot = fn.find_last_of('.');
        if (dot != std::string::npos) {
            const std::string slot = fn.substr(0, dot + 1);
            for (const AlsaDevice& d : devices)
                if (!d.fallback && d.sysfsDevice.rfind(slot, 0) == 0) return d.pcm;
        }
    }
    return {};
}

static std::string selemName(snd_mixer_elem_t* e) { return snd_mixer_selem_get_name(e); }

std::string AlsaBackend::applyMixerQuirks(const std::string& videoDriver) {
    if (videoDriver != "stk1160") return {};
    // stk1160 audio is silent until its AC97 mixer selects Line as the source.
    int card = -1;
    std::string done;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char* id = nullptr;
        snd_card_get_name(card, &id);
        std::string cname = id ? id : "";
        free(id);
        char* lname = nullptr;
        snd_card_get_longname(card, &lname);
        std::string longname = lname ? lname : "";
        free(lname);
        std::string lower = cname + " " + longname;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("stk1160") == std::string::npos) continue;
        snd_mixer_t* m = nullptr;
        if (snd_mixer_open(&m, 0) < 0) continue;
        const std::string hw = "hw:" + std::to_string(card);
        if (snd_mixer_attach(m, hw.c_str()) < 0 || snd_mixer_selem_register(m, nullptr, nullptr) < 0 ||
            snd_mixer_load(m) < 0) {
            snd_mixer_close(m);
            continue;
        }
        for (snd_mixer_elem_t* e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
            if (!snd_mixer_selem_is_enumerated(e)) continue;
            const int n = snd_mixer_selem_get_enum_items(e);
            for (int i = 0; i < n; ++i) {
                char item[64];
                if (snd_mixer_selem_get_enum_item_name(e, unsigned(i), sizeof item, item) < 0) continue;
                if (std::strcmp(item, "Line") == 0) {
                    snd_mixer_selem_set_enum_item(e, SND_MIXER_SCHN_MONO, unsigned(i));
                    snd_mixer_selem_set_enum_item(e, SND_MIXER_SCHN_FRONT_RIGHT, unsigned(i));
                    done = "stk1160 mixer: '" + selemName(e) + "' set to Line on card " + cname;
                }
            }
        }
        // Make sure line capture is switched on and not at zero.
        for (snd_mixer_elem_t* e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
            if (snd_mixer_selem_has_capture_switch(e)) snd_mixer_selem_set_capture_switch_all(e, 1);
        }
        snd_mixer_close(m);
    }
    return done;
}

bool AlsaBackend::open(int preferRate, AudioFormat& out, std::string& err) {
    close();
    int r = snd_pcm_open(&m_h, m_pcm.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (r < 0) {
        err = m_pcm + ": " + snd_strerror(r);
        if (r == -EBUSY) {
            // Name who holds it: scan /proc for the capture node of this card.
            std::string card;
            const size_t c = m_pcm.find("CARD=");
            if (c != std::string::npos) card = m_pcm.substr(c + 5, m_pcm.find(',', c) - c - 5);
            const int idx = card.empty() ? -1 : snd_card_get_index(card.c_str());
            if (idx >= 0) {
                for (int dev = 0; dev < 8; ++dev) {
                    const std::string node = "/dev/snd/pcmC" + std::to_string(idx) + "D" + std::to_string(dev) + "c";
                    for (const auto& p : processesHolding(node))
                        err += ", held by " + p.name + " (pid " + std::to_string(p.pid) + ")";
                }
            }
            err += ". The 'pipewire' device works as a fallback, but PipeWire may resample.";
        } else if (r == -EACCES || r == -EPERM) {
            err += ". Add yourself to the audio group (sudo usermod -aG audio $USER, then log out and in).";
        }
        m_h = nullptr;
        return false;
    }
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(m_h, hw);
    snd_pcm_hw_params_set_rate_resample(m_h, hw, 0);  // never let alsa-lib resample
    if ((r = snd_pcm_hw_params_set_access(m_h, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        err = "interleaved access not supported: " + std::string(snd_strerror(r));
        close();
        return false;
    }
    // Deepest native format first.
    struct F {
        snd_pcm_format_t f;
        SampleFmt s;
        bool packed;
        bool low24;
    };
    const F order[] = {{SND_PCM_FORMAT_S32_LE, SampleFmt::S32, false, false},
                       {SND_PCM_FORMAT_S24_LE, SampleFmt::S24, false, true},
                       {SND_PCM_FORMAT_S24_3LE, SampleFmt::S24, true, false},
                       {SND_PCM_FORMAT_S16_LE, SampleFmt::S16, false, false}};
    bool fmtOk = false;
    for (const F& f : order) {
        if (snd_pcm_hw_params_test_format(m_h, hw, f.f) == 0 && snd_pcm_hw_params_set_format(m_h, hw, f.f) == 0) {
            m_fmt.fmt = f.s;
            m_packed24 = f.packed;
            m_low24 = f.low24;
            m_formatDesc = snd_pcm_format_name(f.f);
            fmtOk = true;
            break;
        }
    }
    if (!fmtOk) {
        err = "device offers no integer PCM format PingDub can store";
        close();
        return false;
    }
    // Rate: 48 kHz if offered, else the device's highest native rate.
    m_rates.clear();
    for (int rate : {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000})
        if (snd_pcm_hw_params_test_rate(m_h, hw, unsigned(rate), 0) == 0) m_rates.push_back(rate);
    unsigned rate = preferRate > 0 ? unsigned(preferRate) : 48000;
    if (snd_pcm_hw_params_test_rate(m_h, hw, rate, 0) != 0) {
        rate = m_rates.empty() ? 48000 : unsigned(m_rates.back());
    }
    int dir = 0;
    if ((r = snd_pcm_hw_params_set_rate_near(m_h, hw, &rate, &dir)) < 0) {
        err = "cannot set rate: " + std::string(snd_strerror(r));
        close();
        return false;
    }
    // Channels: what the device has. hw devices usually allow exactly one count.
    unsigned chMin = 1, chMax = 2;
    snd_pcm_hw_params_get_channels_min(hw, &chMin);
    snd_pcm_hw_params_get_channels_max(hw, &chMax);
    unsigned ch = std::min(chMax, 8u);
    if (ch < chMin) ch = chMin;
    if ((r = snd_pcm_hw_params_set_channels(m_h, hw, ch)) < 0) {
        err = "cannot set channels: " + std::string(snd_strerror(r));
        close();
        return false;
    }
    // ~10 ms periods, ~500 ms buffer, never fewer than three periods: with
    // two, one late wakeup overruns. Devices with a large minimum period
    // (the GV-USB2's is 32 ms) round the period up.
    unsigned periodsMin = 3;
    snd_pcm_hw_params_set_periods_min(m_h, hw, &periodsMin, &dir);
    dir = 0;
    snd_pcm_uframes_t period = rate / 100;
    snd_pcm_hw_params_set_period_size_near(m_h, hw, &period, &dir);
    snd_pcm_uframes_t buffer = rate / 2;
    snd_pcm_hw_params_set_buffer_size_near(m_h, hw, &buffer);
    if ((r = snd_pcm_hw_params(m_h, hw)) < 0) {
        err = "hw params: " + std::string(snd_strerror(r));
        close();
        return false;
    }
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    m_period = period;

    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(m_h, sw);
    snd_pcm_sw_params_set_tstamp_mode(m_h, sw, SND_PCM_TSTAMP_ENABLE);
    snd_pcm_sw_params_set_tstamp_type(m_h, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
    snd_pcm_sw_params_set_avail_min(m_h, sw, period);
    snd_pcm_sw_params_set_start_threshold(m_h, sw, 1);
    snd_pcm_sw_params(m_h, sw);

    m_fmt.rate = int(rate);
    m_fmt.channels = int(ch);
    out = m_fmt;
    return true;
}

bool AlsaBackend::start(std::string& err) {
    if (!m_h) {
        err = "not open";
        return false;
    }
    snd_pcm_prepare(m_h);
    const int r = snd_pcm_start(m_h);
    if (r < 0) {
        err = "snd_pcm_start: " + std::string(snd_strerror(r));
        return false;
    }
    return true;
}

void AlsaBackend::stop() {
    if (m_h) snd_pcm_drop(m_h);
}

void AlsaBackend::close() {
    if (m_h) {
        snd_pcm_drop(m_h);
        snd_pcm_close(m_h);
    }
    m_h = nullptr;
}

int AlsaBackend::read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) {
    xrun = false;
    if (!m_h) {
        err = "not open";
        return -1;
    }
    int w = snd_pcm_wait(m_h, timeoutMs);
    if (w == 0) return 0;
    if (w < 0) {
        if (w == -EPIPE || w == -ESTRPIPE) {
            m_pendingXrun = true;
            snd_pcm_prepare(m_h);
            snd_pcm_start(m_h);
            return 0;
        }
        err = std::string("snd_pcm_wait: ") + snd_strerror(w);
        return -1;
    }
    const int bpf = (m_packed24 ? 3 : (m_fmt.fmt == SampleFmt::S16 ? 2 : 4)) * m_fmt.channels;
    if (m_raw.size() < m_period * size_t(bpf)) m_raw.resize(m_period * size_t(bpf));
    snd_pcm_sframes_t n = snd_pcm_readi(m_h, m_raw.data(), m_period);
    if (n == -EPIPE) {
        // Overrun: recover now, report it with the next chunk so the gap gets measured.
        m_pendingXrun = true;
        snd_pcm_prepare(m_h);
        snd_pcm_start(m_h);
        return 0;
    }
    if (n == -ESTRPIPE) {
        while (snd_pcm_resume(m_h) == -EAGAIN) usleep(10000);
        snd_pcm_prepare(m_h);
        m_pendingXrun = true;
        return 0;
    }
    if (n == -EAGAIN) return 0;
    if (n < 0) {
        err = n == -ENODEV ? "audio device disconnected" : std::string("snd_pcm_readi: ") + snd_strerror(int(n));
        return -1;
    }
    if (n == 0) return 0;

    // Timestamp of the first sample: status time minus everything still
    // buffered and this chunk.
    snd_pcm_status_t* st;
    snd_pcm_status_alloca(&st);
    int64_t tsNs = 0;
    if (snd_pcm_status(m_h, st) == 0) {
        snd_htimestamp_t h;
        snd_pcm_status_get_htstamp(st, &h);
        const snd_pcm_uframes_t avail = snd_pcm_status_get_avail(st);
        const int64_t now = int64_t(h.tv_sec) * 1000000000LL + h.tv_nsec;
        if (now > 0) tsNs = now - int64_t(double(avail + snd_pcm_uframes_t(n)) * 1e9 / m_fmt.rate);
    }
    if (!tsNs) {
        const snd_pcm_sframes_t avail = std::max<snd_pcm_sframes_t>(0, snd_pcm_avail(m_h));
        tsNs = monotonicNs() - int64_t(double(avail + n) * 1e9 / m_fmt.rate);
    }

    const size_t total = size_t(n) * size_t(m_fmt.channels);
    out.samples = std::make_shared<std::vector<int32_t>>(total);
    int32_t* d = out.samples->data();
    const uint8_t* s = m_raw.data();
    if (m_fmt.fmt == SampleFmt::S16) {
        for (size_t i = 0; i < total; ++i) d[i] = int32_t(int16_t(s[2 * i] | (s[2 * i + 1] << 8))) * 65536;
    } else if (m_packed24) {
        for (size_t i = 0; i < total; ++i) d[i] = int32_t(uint32_t(s[3 * i]) << 8 | uint32_t(s[3 * i + 1]) << 16 | uint32_t(s[3 * i + 2]) << 24);
    } else if (m_low24) {
        for (size_t i = 0; i < total; ++i) {
            const uint32_t v = uint32_t(s[4 * i]) | uint32_t(s[4 * i + 1]) << 8 | uint32_t(s[4 * i + 2]) << 16;
            d[i] = int32_t(v << 8);
        }
    } else {
        std::memcpy(d, s, total * 4);
    }
    out.frames = uint32_t(n);
    out.tsNs = tsNs;
    out.flags = 0;
    xrun = m_pendingXrun;
    m_pendingXrun = false;
    return int(n);
}

std::string AlsaBackend::mixerDevice() const {
    const size_t c = m_pcm.find("CARD=");
    if (c == std::string::npos) return {};
    const std::string card = m_pcm.substr(c + 5, m_pcm.find(',', c) - c - 5);
    return "hw:CARD=" + card;
}

std::vector<MixerControl> AlsaBackend::mixer() {
    std::vector<MixerControl> out;
    const std::string dev = mixerDevice();
    if (dev.empty()) return out;
    snd_mixer_t* m = nullptr;
    if (snd_mixer_open(&m, 0) < 0) return out;
    if (snd_mixer_attach(m, dev.c_str()) < 0 || snd_mixer_selem_register(m, nullptr, nullptr) < 0 || snd_mixer_load(m) < 0) {
        snd_mixer_close(m);
        return out;
    }
    for (snd_mixer_elem_t* e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
        if (!snd_mixer_selem_is_active(e)) continue;
        const std::string name = selemName(e);
        const int idx = int(snd_mixer_selem_get_index(e));
        if (snd_mixer_selem_has_capture_volume(e)) {
            MixerControl c;
            c.name = name;
            c.index = idx;
            c.kind = MixerControl::Kind::Volume;
            snd_mixer_selem_get_capture_volume_range(e, &c.minimum, &c.maximum);
            snd_mixer_selem_get_capture_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &c.value);
            out.push_back(c);
        }
        if (snd_mixer_selem_has_capture_switch(e)) {
            MixerControl c;
            c.name = name;
            c.index = idx;
            c.kind = MixerControl::Kind::Switch;
            c.maximum = 1;
            int v = 0;
            snd_mixer_selem_get_capture_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &v);
            c.value = v;
            out.push_back(c);
        }
        if (snd_mixer_selem_is_enumerated(e) && snd_mixer_selem_is_enum_capture(e)) {
            MixerControl c;
            c.name = name;
            c.index = idx;
            c.kind = MixerControl::Kind::Enum;
            const int n = snd_mixer_selem_get_enum_items(e);
            for (int i = 0; i < n; ++i) {
                char item[64];
                if (snd_mixer_selem_get_enum_item_name(e, unsigned(i), sizeof item, item) == 0) c.items.push_back(item);
            }
            unsigned cur = 0;
            snd_mixer_selem_get_enum_item(e, SND_MIXER_SCHN_MONO, &cur);
            c.value = long(cur);
            c.maximum = long(c.items.size()) - 1;
            out.push_back(c);
        }
    }
    snd_mixer_close(m);
    return out;
}

bool AlsaBackend::setMixer(const std::string& name, int index, long value, std::string& err) {
    const std::string dev = mixerDevice();
    snd_mixer_t* m = nullptr;
    if (dev.empty() || snd_mixer_open(&m, 0) < 0) {
        err = "no mixer";
        return false;
    }
    bool ok = false;
    if (snd_mixer_attach(m, dev.c_str()) == 0 && snd_mixer_selem_register(m, nullptr, nullptr) == 0 && snd_mixer_load(m) == 0) {
        snd_mixer_selem_id_t* sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, name.c_str());
        snd_mixer_selem_id_set_index(sid, unsigned(index));
        if (snd_mixer_elem_t* e = snd_mixer_find_selem(m, sid)) {
            if (snd_mixer_selem_is_enumerated(e)) ok = snd_mixer_selem_set_enum_item(e, SND_MIXER_SCHN_MONO, unsigned(value)) == 0;
            else if (snd_mixer_selem_has_capture_volume(e)) ok = snd_mixer_selem_set_capture_volume_all(e, value) == 0;
            else if (snd_mixer_selem_has_capture_switch(e)) ok = snd_mixer_selem_set_capture_switch_all(e, int(value)) == 0;
        }
    }
    snd_mixer_close(m);
    if (!ok) err = "could not set " + name;
    return ok;
}

std::string AlsaBackend::diagnostics() {
    std::ostringstream o;
    o << "PCM " << m_pcm;
    if (m_h) o << ": " << m_formatDesc << ", " << m_fmt.rate << " Hz, " << m_fmt.channels << " ch, period " << m_period;
    o << "\n";
    for (const auto& c : mixer()) {
        o << "  mixer " << c.name << "[" << c.index << "] ";
        if (c.kind == MixerControl::Kind::Enum) {
            o << "= " << (c.value >= 0 && size_t(c.value) < c.items.size() ? c.items[size_t(c.value)] : "?") << " of {";
            for (size_t i = 0; i < c.items.size(); ++i) o << (i ? ", " : "") << c.items[i];
            o << "}";
        } else if (c.kind == MixerControl::Kind::Switch) {
            o << "capture " << (c.value ? "on" : "off");
        } else {
            o << "capture volume " << c.value << " (" << c.minimum << ".." << c.maximum << ")";
        }
        o << "\n";
    }
    return o.str();
}

}  // namespace pd
