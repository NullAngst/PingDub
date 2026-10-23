// SPDX-License-Identifier: GPL-3.0-or-later
#include "Cea608.h"

#include <libzvbi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pd {

uint8_t withOddParity(uint8_t b7) {
    b7 &= 0x7f;
    const int ones = __builtin_popcount(b7);
    return (ones & 1) ? b7 : uint8_t(b7 | 0x80);
}

bool oddParityOk(uint8_t b) { return __builtin_popcount(b) & 1; }

// --- SCC --------------------------------------------------------------------

void SccWriter::add(int64_t slot, uint8_t b1, uint8_t b2) {
    m_pairs.push_back({slot, uint16_t((b1 << 8) | b2)});
}

std::string SccWriter::timecode(int64_t frame, const Rational& fps) {
    char buf[64];
    if (fps.num == 30000 && fps.den == 1001) {
        // SMPTE drop-frame: skip frame numbers 0 and 1 every minute except each tenth.
        const int64_t perTen = 17982, perMin = 1798;
        const int64_t d = frame / perTen, m = frame % perTen;
        int64_t f = frame + 18 * d + (m > 1 ? 2 * ((m - 2) / perMin) : 0);
        std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld;%02lld", (long long)(f / 108000),
                      (long long)((f / 1800) % 60), (long long)((f / 30) % 60), (long long)(f % 30));
    } else {
        const int r = std::max(1, int(std::lround(fps.toDouble())));
        std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld:%02lld", (long long)(frame / (r * 3600)),
                      (long long)((frame / (r * 60)) % 60), (long long)((frame / r) % 60), (long long)(frame % r));
    }
    return buf;
}

std::string SccWriter::text() const {
    std::string out = "Scenarist_SCC V1.0\n";
    int64_t prev = -10;
    bool open = false;
    for (const auto& [slot, word] : m_pairs) {
        if (word == 0x8080) {  // null padding ends a run
            if (open) out += "\n";
            open = false;
            continue;
        }
        if (!open || slot != prev + 1) {
            if (open) out += "\n";
            out += "\n" + timecode(slot, m_fps) + "\t";
            open = true;
        } else {
            out += " ";
        }
        char w[8];
        std::snprintf(w, sizeof w, "%04x", word);
        out += w;
        prev = slot;
    }
    if (open) out += "\n";
    return out;
}

// --- zvbi caption decoding ---------------------------------------------------

static void eventThunk(vbi_event* ev, void* user) { static_cast<CaptionTracker*>(user)->onEvent(ev); }

CaptionTracker::CaptionTracker() {
    m_dec = vbi_decoder_new();
    if (m_dec) vbi_event_handler_register(m_dec, VBI_EVENT_CAPTION | VBI_EVENT_PROG_INFO, eventThunk, this);
}

CaptionTracker::~CaptionTracker() {
    if (m_dec) vbi_decoder_delete(m_dec);
}

static void appendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

std::string CaptionTracker::pageText(int pgno) {
    vbi_page pg;
    if (!vbi_fetch_cc_page(m_dec, &pg, vbi_pgno(pgno), TRUE)) return {};
    std::string text;
    for (int r = 0; r < pg.rows; ++r) {
        std::string row;
        for (int c = 0; c < pg.columns; ++c) {
            const vbi_char& ch = pg.text[r * pg.columns + c];
            if (ch.opacity == VBI_TRANSPARENT_SPACE || ch.unicode == 0) row += ' ';
            else appendUtf8(row, ch.unicode);
        }
        const size_t a = row.find_first_not_of(' ');
        if (a == std::string::npos) continue;
        const size_t b = row.find_last_not_of(' ');
        if (!text.empty()) text += "\n";
        text += row.substr(a, b - a + 1);
    }
    vbi_unref_page(&pg);
    return text;
}

void CaptionTracker::onEvent(const vbi_event* ev) {
    if (ev->type == VBI_EVENT_PROG_INFO && ev->ev.prog_info) {
        const vbi_program_info* pi = ev->ev.prog_info;
        std::lock_guard<std::mutex> lk(m_mutex);
        if (pi->title[0]) m_xds.title = reinterpret_cast<const char*>(pi->title);
        if (pi->rating_auth != VBI_RATING_AUTH_NONE) {
            const char* r = vbi_rating_string(pi->rating_auth, pi->rating_id);
            if (r) m_xds.rating = r;
        }
        return;
    }
    if (ev->type != VBI_EVENT_CAPTION) return;
    const int ch = ev->ev.caption.pgno;
    if (ch < 1 || ch > 4) return;  // text services T1-T4 are not captions
    const std::string now = pageText(ch);
    std::string& old = m_screen[ch];
    if (now == old) return;
    // Roll-up and paint-on captions grow a character at a time; keep one cue
    // while the new screen only adds to the old one.
    const bool grows = !old.empty() && now.size() > old.size() && now.compare(0, old.size(), old) == 0;
    if (!grows) {
        if (!old.empty()) m_cues[ch].push_back({m_since[ch], m_now, old});
        if (!now.empty()) m_since[ch] = m_now;
    }
    old = now;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_live[ch] = now;
}

void CaptionTracker::feed(double timeSec, const std::vector<std::pair<int, std::pair<uint8_t, uint8_t>>>& lines) {
    if (!m_dec || lines.empty()) return;
    m_now = timeSec;
    vbi_sliced sl[4];
    int n = 0;
    for (const auto& [field, bytes] : lines) {
        if (n >= 4) break;
        sl[n].id = field == 2 ? VBI_SLICED_CAPTION_525_F2 : VBI_SLICED_CAPTION_525_F1;
        sl[n].line = field == 2 ? 284 : 21;
        sl[n].data[0] = bytes.first;
        sl[n].data[1] = bytes.second;
        ++n;
    }
    vbi_decode(m_dec, sl, n, timeSec);
}

void CaptionTracker::finish(double timeSec) {
    for (auto& [ch, text] : m_screen) {
        if (!text.empty()) m_cues[ch].push_back({m_since[ch], timeSec, text});
        text.clear();
    }
}

static std::string srtTime(double t) {
    if (t < 0) t = 0;
    const long long ms = (long long)std::llround(t * 1000.0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60,
                  ms % 1000);
    return buf;
}

std::string CaptionTracker::srt(const std::vector<SrtCue>& cues) {
    std::string out;
    int n = 0;
    for (const SrtCue& c : cues) {
        if (c.end <= c.start) continue;
        out += std::to_string(++n) + "\n" + srtTime(c.start) + " --> " + srtTime(c.end) + "\n" + c.text + "\n\n";
    }
    return out;
}

std::string CaptionTracker::liveText(int channel) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_live.find(channel);
    return it == m_live.end() ? std::string() : it->second;
}

XdsInfo CaptionTracker::xds() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_xds;
}

}  // namespace pd
