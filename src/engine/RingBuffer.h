// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded FIFO between a capture thread and the sync thread, limited by bytes
// rather than item count. The producer never blocks: if the ring is full the
// push fails and the caller counts a buffer overflow. That keeps the driver
// queue moving no matter how far behind the disk falls.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace pd {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityBytes = 0) : m_capacity(capacityBytes) {}

    void setCapacity(size_t bytes) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_capacity = bytes;
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_capacity;
    }

    // Returns false (and leaves the ring untouched) when the item does not fit.
    bool push(T item, size_t bytes) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_closed) return false;
            if (m_capacity && m_bytes + bytes > m_capacity && !m_items.empty()) return false;
            m_items.push_back(Entry{std::move(item), bytes});
            m_bytes += bytes;
            if (m_bytes > m_peak) m_peak = m_bytes;
        }
        m_cv.notify_one();
        return true;
    }

    // Waits up to timeout for an item. Returns false on timeout or close.
    template <typename Rep, typename Period>
    bool pop(T& out, std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_cv.wait_for(lk, timeout, [&] { return !m_items.empty() || m_closed; })) return false;
        if (m_items.empty()) return false;
        out = std::move(m_items.front().item);
        m_bytes -= m_items.front().bytes;
        m_items.pop_front();
        return true;
    }

    bool tryPop(T& out) { return pop(out, std::chrono::milliseconds(0)); }

    // Copies the head without removing it.
    bool peek(T& out) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_items.empty()) return false;
        out = m_items.front().item;
        return true;
    }

    // Waits until the ring has data or is closed, then reports whether it has data.
    template <typename Rep, typename Period>
    bool waitNonEmpty(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait_for(lk, timeout, [&] { return !m_items.empty() || m_closed; });
        return !m_items.empty();
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_items.clear();
        m_bytes = 0;
        m_peak = 0;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_closed = true;
        }
        m_cv.notify_all();
    }

    void reopen() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_closed = false;
    }

    size_t bytes() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_bytes;
    }

    size_t peakBytes() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_peak;
    }

    size_t count() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_items.size();
    }

    // Fill level 0..1.
    double fill() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_capacity ? double(m_bytes) / double(m_capacity) : 0.0;
    }

private:
    struct Entry {
        T item;
        size_t bytes;
    };
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Entry> m_items;
    size_t m_capacity = 0;
    size_t m_bytes = 0;
    size_t m_peak = 0;
    bool m_closed = false;
};

// Bounded queue whose producer waits when full. Used after the sync point,
// where back-pressure is wanted: a slow disk stalls the encoder, the encoder
// stalls the sync thread, and the RAM ring in front of it absorbs the rest.
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacityBytes) : m_capacity(capacityBytes) {}

    bool push(T item, size_t bytes) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_notFull.wait(lk, [&] { return m_closed || m_bytes == 0 || m_bytes + bytes <= m_capacity; });
        if (m_closed) return false;
        m_items.push_back(Entry{std::move(item), bytes});
        m_bytes += bytes;
        lk.unlock();
        m_notEmpty.notify_one();
        return true;
    }

    // Blocks until an item arrives or the queue is closed and empty.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_notEmpty.wait(lk, [&] { return !m_items.empty() || m_closed; });
        if (m_items.empty()) return false;
        out = std::move(m_items.front().item);
        m_bytes -= m_items.front().bytes;
        m_items.pop_front();
        lk.unlock();
        m_notFull.notify_one();
        return true;
    }

    // Producers stop; the consumer still drains what is queued.
    void close() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_closed = true;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    size_t bytes() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_bytes;
    }

private:
    struct Entry {
        T item;
        size_t bytes;
    };
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty, m_notFull;
    std::deque<Entry> m_items;
    size_t m_capacity;
    size_t m_bytes = 0;
    bool m_closed = false;
};

}  // namespace pd
