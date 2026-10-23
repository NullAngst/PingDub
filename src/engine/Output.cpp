// SPDX-License-Identifier: GPL-3.0-or-later
#include "Output.h"

#include "SysInfo.h"

#include <cmath>
#include <cstring>

namespace pd {

OutputWriter::OutputWriter(EventLog* log) : m_log(log) {}

OutputWriter::~OutputWriter() {
    if (m_open) close({});
}

void OutputWriter::setError(const std::string& e) {
    std::lock_guard<std::mutex> lk(m_errMutex);
    if (m_error.empty()) m_error = e;
    m_failed = true;
}

bool OutputWriter::failed() const { return m_failed.load() || m_muxer.failed(); }

std::string OutputWriter::error() const {
    {
        std::lock_guard<std::mutex> lk(m_errMutex);
        if (!m_error.empty()) return m_error;
    }
    return m_muxer.error();
}

std::string OutputWriter::currentFolder() const {
    return m_onOverflow.load() ? m_plan.out.overflowFolder : m_plan.folder;
}

std::string OutputWriter::segmentPath(int index, bool overflow) const {
    const std::string dir = overflow ? m_plan.out.overflowFolder : m_plan.folder;
    std::string name = m_plan.stem;
    if (m_segmenting || index > 1) {
        char suf[16];
        std::snprintf(suf, sizeof suf, "_part%03d", index);
        name += suf;
    }
    return dir + "/" + name + extension();
}

bool OutputWriter::open(const OutputPlan& plan, std::string& err) {
    m_plan = plan;
    m_segmenting = plan.out.segmentBytes > 0 || plan.out.segmentSeconds > 0;
    if (!makeDirs(plan.folder)) {
        err = "cannot create folder " + plan.folder;
        return false;
    }
    if (!plan.out.overflowFolder.empty()) makeDirs(plan.out.overflowFolder);

    // Open the encoders once up front so a bad codec/format pair fails
    // before the tape starts, not after.
    {
        VideoEncoder probe;
        if (!probe.open(plan.vfmt, plan.out.vcodec, plan.out.container, plan.out.threads, err)) return false;
        m_vdesc = probe.description();
        if (plan.haveAudio) {
            AudioEncoder ap;
            if (!ap.open(plan.afmt, plan.out.acodec, plan.out.container, err)) return false;
            m_adesc = ap.description();
        }
    }

    const size_t frameBytes = std::max<size_t>(plan.vfmt.frameBytes, 64 * 1024);
    m_queue = std::make_unique<BlockingQueue<Item>>(std::max<size_t>(frameBytes * 24, 32u << 20));
    m_muxer.start(256u << 20);
    if (!openSegment(0, 0)) {
        err = error();
        m_muxer.stop();
        return false;
    }
    m_open = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

bool OutputWriter::openSegment(int64_t startSlot, uint64_t startSample) {
    std::string err;
    m_venc = std::make_unique<VideoEncoder>();
    if (!m_venc->open(m_plan.vfmt, m_plan.out.vcodec, m_plan.out.container, m_plan.out.threads, err)) {
        setError(err);
        return false;
    }
    if (m_plan.haveAudio) {
        m_aenc = std::make_unique<AudioEncoder>();
        if (!m_aenc->open(m_plan.afmt, m_plan.out.acodec, m_plan.out.container, err)) {
            setError(err);
            return false;
        }
    }
    const int index = m_segIndex.load() + 1;
    SegmentSpec spec;
    spec.path = segmentPath(index, m_onOverflow.load());
    spec.container = m_plan.out.container;
    MuxStream vs;
    vs.par = copyParams(m_venc->params());
    vs.encoderTb = m_venc->timeBase();
    vs.title = "Video";
    spec.streams.push_back(vs);
    if (m_aenc) {
        MuxStream as;
        as.par = copyParams(m_aenc->params());
        as.encoderTb = m_aenc->timeBase();
        as.title = "Audio";
        spec.streams.push_back(as);
    }
    spec.tags = m_plan.tags;
    if (m_plan.out.container == Container::MKV) {
        for (const auto& k : m_plan.deferredTags) spec.tags[k] = tagPlaceholder(k);
        spec.tags["PINGDUB_SEGMENT"] = std::to_string(index);
        char tc[32];
        const double sec = double(startSlot) * m_plan.vfmt.fps.den / m_plan.vfmt.fps.num;
        std::snprintf(tc, sizeof tc, "%.6f", sec);
        spec.tags["PINGDUB_SEGMENT_START"] = tc;
    } else {
        // AVI's INFO list has room for a comment and little else; totals live in the sidecars.
        std::string c = "PingDub capture";
        auto it = m_plan.tags.find("PINGDUB_FIELD_ORDER");
        if (it != m_plan.tags.end()) c += ", field order " + it->second;
        spec.tags = {{"comment", c}, {"encoder", m_plan.tags.count("ENCODER") ? m_plan.tags.at("ENCODER") : "PingDub"}};
        if (m_plan.tags.count("TITLE")) spec.tags["title"] = m_plan.tags.at("TITLE");
    }
    m_muxer.beginSegment(spec);
    m_segIndex = index;
    m_segStartSlot = startSlot;
    m_segStartSample = startSample;
    if (m_log && index > 1)
        m_log->log("segment_start", startSlot, uint64_t(index), spec.path);
    return true;
}

void OutputWriter::closeSegment() {
    if (m_venc) {
        m_pkts.clear();
        m_venc->flush(m_pkts);
        for (AVPacket* p : m_pkts) m_muxer.writePacket(p, 0);
    }
    if (m_aenc) {
        m_pkts.clear();
        m_aenc->flush(m_pkts);
        for (AVPacket* p : m_pkts) m_muxer.writePacket(p, 1);
    }
    m_pkts.clear();
    m_muxer.endSegment();
    m_venc.reset();
    m_aenc.reset();
}

bool OutputWriter::needRoll(int64_t slot) {
    const int64_t inSeg = slot - m_segStartSlot;
    if (inSeg <= 0) return false;
    if (m_plan.out.segmentSeconds > 0 &&
        double(inSeg) >= m_plan.out.segmentSeconds * m_plan.vfmt.fps.toDouble())
        return true;
    if (m_plan.out.segmentBytes > 0 && m_muxer.segmentBytes() >= m_plan.out.segmentBytes)
        return true;
    // Spill: check free space about once a second.
    if (!m_onOverflow.load() && !m_plan.out.overflowFolder.empty() && slot - m_lastSpaceCheckSlot >= 30) {
        m_lastSpaceCheckSlot = slot;
        const uint64_t freeB = freeBytes(m_plan.folder);
        if (freeB && freeB < m_plan.out.overflowFreeBytes) {
            m_onOverflow = true;
            if (m_log)
                m_log->log("overflow_switch", slot, freeB, "free space low, continuing in " + m_plan.out.overflowFolder);
            return true;
        }
    }
    return false;
}

void OutputWriter::encodeVideo(const Item& it) {
    m_pkts.clear();
    std::string err;
    if (!m_venc->encode(it.frame, it.slot - m_segStartSlot, m_pkts, err)) {
        setError(err);
        return;
    }
    for (AVPacket* p : m_pkts) m_muxer.writePacket(p, 0);
    m_pkts.clear();
    ++m_framesEncoded;
}

void OutputWriter::encodeAudio(const int32_t* s, size_t frames) {
    if (!m_aenc || !frames) return;
    m_pkts.clear();
    m_aenc->push(s, frames, m_pkts);
    for (AVPacket* p : m_pkts) m_muxer.writePacket(p, 1);
    m_pkts.clear();
}

void OutputWriter::run() {
    Item it;
    const size_t ch = size_t(std::max(1, m_plan.afmt.channels));
    auto tryCompleteRoll = [&](bool force) {
        if (!m_pendingRoll) return;
        if (!force && m_plan.haveAudio && m_audioCount < m_rollSample) return;
        closeSegment();
        if (!openSegment(m_rollSlot, m_rollSample)) return;
        m_pendingRoll = false;
        std::vector<Item> held;
        held.swap(m_heldVideo);
        for (const Item& v : held) encodeVideo(v);
        std::vector<int32_t> ha;
        ha.swap(m_heldAudio);
        if (!ha.empty()) encodeAudio(ha.data(), ha.size() / ch);
    };

    while (m_queue->pop(it)) {
        if (it.type == Item::End) break;
        if (m_failed) continue;  // keep draining so the sync thread never blocks
        if (it.type == Item::Video) {
            if (!m_pendingRoll && needRoll(it.slot)) {
                m_pendingRoll = true;
                m_rollSlot = it.slot;
                m_rollSample = uint64_t(std::llround(double(it.slot) * m_plan.afmt.rate * m_plan.vfmt.fps.den /
                                                     double(m_plan.vfmt.fps.num)));
            }
            if (m_pendingRoll) m_heldVideo.push_back(it);
            else encodeVideo(it);
            tryCompleteRoll(false);
        } else {
            const int32_t* s = it.audio.data();
            size_t n = it.frames;
            if (m_pendingRoll) {
                // Samples before the split point finish the old segment.
                if (m_audioCount < m_rollSample) {
                    const size_t before = size_t(std::min<uint64_t>(n, m_rollSample - m_audioCount));
                    encodeAudio(s, before);
                    s += before * ch;
                    n -= before;
                    m_audioCount += before;
                }
                m_heldAudio.insert(m_heldAudio.end(), s, s + n * ch);
                m_audioCount += n;
            } else {
                encodeAudio(s, n);
                m_audioCount += n;
            }
            tryCompleteRoll(false);
        }
        it = Item{};
    }
    tryCompleteRoll(true);
    if (!m_pendingRoll) closeSegment();
}

void OutputWriter::syncVideo(const VideoFrame& frame, int64_t slot, bool inserted) {
    if (!m_queue) return;
    Item it;
    it.type = Item::Video;
    it.frame = frame;
    it.slot = slot;
    (void)inserted;
    const size_t bytes = frame.data ? frame.size : 64;
    // Inserted repeats share the buffer of the frame they repeat.
    m_queue->push(std::move(it), inserted ? 64 : bytes);
}

void OutputWriter::syncAudio(const int32_t* samples, size_t frames) {
    if (!m_queue || !m_plan.haveAudio || !frames) return;
    Item it;
    it.type = Item::Audio;
    it.audio.assign(samples, samples + frames * size_t(m_plan.afmt.channels));
    it.frames = frames;
    const size_t bytes = it.audio.size() * sizeof(int32_t);
    m_queue->push(std::move(it), bytes);
}

void OutputWriter::syncEvent(const SyncEvent& ev) {
    if (m_log) m_log->log(syncEventName(ev.type), ev.slot, ev.count, ev.detail);
}

void OutputWriter::close(const std::map<std::string, std::string>& finalTags) {
    if (!m_open) return;
    Item end;
    end.type = Item::End;
    m_queue->push(std::move(end), 1);
    if (m_thread.joinable()) m_thread.join();
    m_queue->close();
    m_muxer.stop();
    m_open = false;
    if (m_plan.out.container == Container::MKV && !finalTags.empty()) {
        for (const std::string& f : m_muxer.files()) {
            std::string err;
            if (!patchMatroskaTags(f, finalTags, err) && m_log)
                m_log->log("warning", -1, 0, "could not write totals into tags of " + f + ": " + err);
        }
    }
}

}  // namespace pd
