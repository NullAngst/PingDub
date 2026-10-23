// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeckLinkBackend.h"

#include "V4l2Logic.h"
#include "engine/Clock.h"
#include "engine/PixelConvert.h"

#include <DeckLinkAPI.h>
#include <linux/videodev2.h>

#include <chrono>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace pd {

namespace {
// Standards offered, keyed by V4L2 ids so profiles look the same for every backend.
// NTSC-J is NTSC without the 7.5 IRE setup.
constexpr uint64_t kNtscM = V4L2_STD_NTSC_M;
constexpr uint64_t kNtscJ = V4L2_STD_NTSC_M_JP;
constexpr uint64_t kPal = V4L2_STD_PAL_BG | V4L2_STD_PAL_DK | V4L2_STD_PAL_I;

std::string takeString(const char* s) {
    std::string r = s ? s : "";
    if (s) std::free(const_cast<char*>(s));  // Linux DeckLink API strings belong to the caller
    return r;
}
}  // namespace

class DeckLinkCallback : public IDeckLinkInputCallback {
public:
    explicit DeckLinkCallback(DeckLinkBackend* b) : m_b(b) {}
    HRESULT QueryInterface(REFIID, LPVOID* ppv) override {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++m_ref; }
    ULONG Release() override {
        const ULONG r = --m_ref;
        if (!r) delete this;
        return r;
    }
    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*,
                                    BMDDetectedVideoInputFormatFlags) override {
        return S_OK;
    }
    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* v, IDeckLinkAudioInputPacket* a) override {
        const uint8_t* vb = nullptr;
        size_t vsize = 0;
        int64_t vts = -1;
        bool noSignal = false;
        if (v) {
            void* p = nullptr;
            v->GetBytes(&p);
            vb = static_cast<const uint8_t*>(p);
            vsize = size_t(v->GetRowBytes()) * size_t(v->GetHeight());
            BMDTimeValue t = 0, d = 0;
            if (v->GetStreamTime(&t, &d, 1000000000) == S_OK) vts = t;
            noSignal = v->GetFlags() & bmdFrameHasNoInputSource;
        }
        const int32_t* ab = nullptr;
        size_t aframes = 0;
        int64_t ats = -1;
        if (a) {
            void* p = nullptr;
            a->GetBytes(&p);
            ab = static_cast<const int32_t*>(p);
            aframes = size_t(a->GetSampleFrameCount());
            BMDTimeValue t = 0;
            if (a->GetPacketTime(&t, 1000000000) == S_OK) ats = t;
        }
        m_b->onFrame(vb, vsize, vts, noSignal, ab, aframes, ats);
        return S_OK;
    }

private:
    DeckLinkBackend* m_b;
    std::atomic<ULONG> m_ref{1};
};

// Checked before touching the SDK dispatch, which prints to stderr when
// Desktop Video is not installed.
static bool libraryPresent() {
    static const bool present = [] {
        void* h = dlopen("libDeckLinkAPI.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) return false;
        dlclose(h);
        return true;
    }();
    return present;
}

bool DeckLinkBackend::available() {
    if (!libraryPresent()) return false;
    IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
    if (!it) return false;
    it->Release();
    return true;
}

std::vector<DeckLinkCardInfo> DeckLinkBackend::enumerate() {
    std::vector<DeckLinkCardInfo> out;
    if (!libraryPresent()) return out;
    IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
    if (!it) return out;
    IDeckLink* card = nullptr;
    int index = 0;
    while (it->Next(&card) == S_OK) {
        DeckLinkCardInfo info;
        info.index = index++;
        const char* name = nullptr;
        if (card->GetDisplayName(&name) == S_OK) info.name = takeString(name);
        IDeckLinkProfileAttributes* attr = nullptr;
        if (card->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&attr)) == S_OK) {
            int64_t conns = 0;
            if (attr->GetInt(BMDDeckLinkVideoInputConnections, &conns) == S_OK) {
                info.composite = conns & bmdVideoConnectionComposite;
                info.svideo = conns & bmdVideoConnectionSVideo;
            }
            attr->Release();
        }
        if (info.composite || info.svideo) out.push_back(info);
        card->Release();
    }
    it->Release();
    return out;
}

DeckLinkBackend::DeckLinkBackend() = default;

DeckLinkBackend::~DeckLinkBackend() {
    stop();
    if (m_cb) m_cb->Release();
    if (m_config) m_config->Release();
    if (m_input_) m_input_->Release();
    if (m_card) m_card->Release();
}

bool DeckLinkBackend::open(int index, std::string& err) {
    IDeckLinkIterator* it = libraryPresent() ? CreateDeckLinkIteratorInstance() : nullptr;
    if (!it) {
        err = "Blackmagic Desktop Video is not installed (libDeckLinkAPI.so not found)";
        return false;
    }
    IDeckLink* card = nullptr;
    int i = 0;
    while (it->Next(&card) == S_OK) {
        if (i++ == index) break;
        card->Release();
        card = nullptr;
    }
    it->Release();
    if (!card) {
        err = "DeckLink card not found";
        return false;
    }
    m_card = card;
    m_index = index;
    const char* name = nullptr;
    if (card->GetDisplayName(&name) == S_OK) m_name = takeString(name);
    if (card->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&m_input_)) != S_OK) {
        err = "card has no capture input";
        return false;
    }
    card->QueryInterface(IID_IDeckLinkConfiguration, reinterpret_cast<void**>(&m_config));
    IDeckLinkProfileAttributes* attr = nullptr;
    if (card->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&attr)) == S_OK) {
        int64_t conns = 0;
        if (attr->GetInt(BMDDeckLinkVideoInputConnections, &conns) == S_OK) {
            m_hasComposite = conns & bmdVideoConnectionComposite;
            m_hasSvideo = conns & bmdVideoConnectionSVideo;
        }
        attr->Release();
    }
    m_cb = new DeckLinkCallback(this);
    m_std = kNtscM;
    m_input = m_hasComposite ? 0 : 1;
    std::string e;
    setInput(m_input, e);
    return true;
}

std::vector<VideoInputInfo> DeckLinkBackend::inputs() {
    std::vector<VideoInputInfo> v;
    if (m_hasComposite) v.push_back({0, "Composite"});
    if (m_hasSvideo) v.push_back({1, "S-Video"});
    return v;
}

bool DeckLinkBackend::setInput(int index, std::string& err) {
    m_input = index;
    if (!m_config) return true;
    const int64_t conn = index == 1 ? bmdVideoConnectionSVideo : bmdVideoConnectionComposite;
    if (m_config->SetInt(bmdDeckLinkConfigVideoInputConnection, conn) != S_OK) {
        err = "card refused the input";
        return false;
    }
    // Analog sources come with analog audio.
    m_config->SetInt(bmdDeckLinkConfigAudioInputConnection, bmdAudioConnectionAnalog);
    return true;
}

std::vector<VideoStandardInfo> DeckLinkBackend::standards() {
    return {{kNtscM, "NTSC-M", {30000, 1001}, true}, {kNtscJ, "NTSC-J", {30000, 1001}, true}, {kPal, "PAL", {25, 1}, false}};
}

bool DeckLinkBackend::setStandard(uint64_t id, std::string& err) {
    m_std = id;
    if (m_config) {
        int64_t flags = 0;
        m_config->GetInt(bmdDeckLinkConfigAnalogVideoInputFlags, &flags);
        if (id == kNtscJ) flags &= ~int64_t(bmdAnalogVideoFlagCompositeSetup75);
        else if (id == kNtscM) flags |= bmdAnalogVideoFlagCompositeSetup75;
        m_config->SetInt(bmdDeckLinkConfigAnalogVideoInputFlags, flags);
    }
    return true;
}

std::vector<VideoFormatOption> DeckLinkBackend::formats() {
    std::vector<VideoFormatOption> v;
    const BMDDisplayMode mode = (m_std & V4L2_STD_525_60) ? bmdModeNTSC : bmdModePAL;
    bool ok10 = false, ok8 = false;
    if (m_input_) {
        const BMDVideoConnection conn = m_input == 1 ? bmdVideoConnectionSVideo : bmdVideoConnectionComposite;
        m_input_->DoesSupportVideoMode(conn, mode, bmdFormat10BitYUV, bmdNoVideoInputConversion, bmdSupportedVideoModeDefault,
                                       nullptr, &ok10);
        m_input_->DoesSupportVideoMode(conn, mode, bmdFormat8BitYUV, bmdNoVideoInputConversion, bmdSupportedVideoModeDefault,
                                       nullptr, &ok8);
    }
    if (ok10) v.push_back({PixFmt::V210, 0, "10-bit 4:2:2 (v210)", true});
    if (ok8 || !ok10) v.push_back({PixFmt::UYVY, 0, "8-bit 4:2:2 (UYVY)", true});
    return v;
}

bool DeckLinkBackend::configure(PixFmt pixfmt, VideoFormat& out, std::string& err) {
    if (pixfmt != PixFmt::V210 && pixfmt != PixFmt::UYVY) {
        err = "DeckLink delivers v210 or UYVY";
        return false;
    }
    const bool l525 = (m_std & V4L2_STD_525_60) != 0;
    IDeckLinkDisplayMode* dm = nullptr;
    FieldOrder field = l525 ? FieldOrder::BFF : FieldOrder::TFF;
    int w = 720, h = l525 ? 486 : 576;
    Rational fps = l525 ? Rational{30000, 1001} : Rational{25, 1};
    if (m_input_ && m_input_->GetDisplayMode(l525 ? bmdModeNTSC : bmdModePAL, &dm) == S_OK && dm) {
        w = int(dm->GetWidth());
        h = int(dm->GetHeight());
        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        if (dm->GetFrameRate(&dur, &scale) == S_OK && dur && scale) fps = Rational{int(scale), int(dur)};
        const BMDFieldDominance fd = dm->GetFieldDominance();
        if (fd == bmdLowerFieldFirst) field = FieldOrder::BFF;
        else if (fd == bmdUpperFieldFirst) field = FieldOrder::TFF;
        dm->Release();
    }
    m_pixfmt = pixfmt;
    m_fmt = VideoFormat{};
    m_fmt.pixfmt = pixfmt;
    m_fmt.width = w;
    m_fmt.height = h;
    m_fmt.bytesPerLine = pixfmt == PixFmt::V210 ? v210Stride(w) : w * 2;
    m_fmt.frameBytes = size_t(m_fmt.bytesPerLine) * size_t(h);
    m_fmt.fps = fps;
    m_fmt.field = field;
    m_fmt.lines525 = l525;
    m_fmt.sar = sarFor(w, l525);
    m_fmt.standardName = m_std == kNtscJ ? "NTSC-J" : (l525 ? "NTSC-M" : "PAL");
    out = m_fmt;
    return true;
}

SignalStatus DeckLinkBackend::signalStatus() {
    SignalStatus s;
    s.known = m_running;
    s.noSignal = m_noSignal.load();
    return s;
}

bool DeckLinkBackend::start(int, std::string& err) {
    if (!m_input_) {
        err = "not open";
        return false;
    }
    if (m_running) return true;
    const BMDDisplayMode mode = m_fmt.lines525 ? bmdModeNTSC : bmdModePAL;
    const BMDPixelFormat pf = m_pixfmt == PixFmt::V210 ? bmdFormat10BitYUV : bmdFormat8BitYUV;
    if (m_input_->EnableVideoInput(mode, pf, bmdVideoInputFlagDefault) != S_OK) {
        err = "EnableVideoInput failed";
        return false;
    }
    if (m_input_->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2) != S_OK) {
        m_input_->DisableVideoInput();
        err = "EnableAudioInput failed";
        return false;
    }
    m_input_->SetCallback(m_cb);
    m_haveOffset = false;
    if (m_input_->StartStreams() != S_OK) {
        err = "StartStreams failed";
        return false;
    }
    m_running = true;
    return true;
}

void DeckLinkBackend::stop() {
    if (!m_running || !m_input_) return;
    m_input_->StopStreams();
    m_input_->SetCallback(nullptr);
    m_input_->DisableAudioInput();
    m_input_->DisableVideoInput();
    m_running = false;
}

void DeckLinkBackend::onFrame(const uint8_t* data, size_t size, int64_t vts, bool noSignal, const int32_t* audio,
                              size_t audioFrames, int64_t ats) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_haveOffset && (vts >= 0 || ats >= 0)) {
        // Card stream time is on the card's clock; shift it once into the
        // monotonic domain. Video and audio share that clock, so they never drift apart.
        m_clockOffset = monotonicNs() - (vts >= 0 ? vts : ats);
        m_haveOffset = true;
    }
    m_noSignal = noSignal;
    if (data && size) {
        QFrame q;
        q.data.assign(data, data + size);
        q.ts = (vts >= 0 ? vts : 0) + m_clockOffset;
        q.seq = m_seq++;
        m_frames.push_back(std::move(q));
        while (m_frames.size() > 16) m_frames.pop_front();
        m_cvV.notify_one();
    } else {
        ++m_seq;  // the card skipped a frame
    }
    if (audio && audioFrames) {
        AudioChunk c;
        c.samples = std::make_shared<std::vector<int32_t>>(audio, audio + audioFrames * 2);
        c.frames = uint32_t(audioFrames);
        c.tsNs = (ats >= 0 ? ats : 0) + m_clockOffset;
        m_audioQ.push_back(std::move(c));
        while (m_audioQ.size() > 500) m_audioQ.pop_front();
        m_cvA.notify_one();
    }
}

int DeckLinkBackend::dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cvV.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !m_frames.empty(); });
    if (m_frames.empty()) return 0;
    QFrame q = std::move(m_frames.front());
    m_frames.pop_front();
    lk.unlock();
    out.data = pool.acquire(q.data.size());
    std::memcpy(out.data->data(), q.data.data(), q.data.size());
    out.size = q.data.size();
    out.tsNs = q.ts;
    out.seq = q.seq;
    out.flags = 0;
    return 1;
}

int DeckLinkBackend::readAudio(AudioChunk& out, int timeoutMs, std::string&) {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cvA.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !m_audioQ.empty(); });
    if (m_audioQ.empty()) return 0;
    out = std::move(m_audioQ.front());
    m_audioQ.pop_front();
    return int(out.frames);
}

bool DeckLinkAudio::open(int, AudioFormat& out, std::string&) {
    out.rate = 48000;
    out.channels = 2;
    out.fmt = SampleFmt::S32;
    return true;
}

int DeckLinkAudio::read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) {
    xrun = false;
    return m_dev->readAudio(out, timeoutMs, err);
}

std::string DeckLinkBackend::diagnostics() {
    std::ostringstream o;
    o << "DeckLink card " << m_index << ": " << m_name << " (EXPERIMENTAL, untested on real hardware)\n";
    o << "  analog inputs:" << (m_hasComposite ? " composite" : "") << (m_hasSvideo ? " s-video" : "") << "\n";
    o << "  formats:";
    for (const auto& f : formats()) o << " " << f.description << ";";
    o << "\n";
    return o.str();
}

}  // namespace pd
