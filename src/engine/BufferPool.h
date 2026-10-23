// SPDX-License-Identifier: GPL-3.0-or-later
// Recycles frame-sized buffers so the capture threads never hit malloc in
// steady state. Buffers come back to the pool when the last reference drops.
#pragma once

#include "Types.h"

#include <mutex>
#include <vector>

namespace pd {

class BufferPool : public std::enable_shared_from_this<BufferPool> {
public:
    static std::shared_ptr<BufferPool> create(size_t keepMax = 128) {
        return std::shared_ptr<BufferPool>(new BufferPool(keepMax));
    }

    BytesPtr acquire(size_t size) {
        Bytes* raw = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_free.empty()) {
                raw = m_free.back();
                m_free.pop_back();
            }
        }
        if (!raw) raw = new Bytes();
        if (raw->size() < size) raw->resize(size);
        std::weak_ptr<BufferPool> weak = shared_from_this();
        return BytesPtr(raw, [weak](Bytes* b) {
            if (auto pool = weak.lock()) pool->release(b);
            else delete b;
        });
    }

    ~BufferPool() {
        for (Bytes* b : m_free) delete b;
    }

private:
    explicit BufferPool(size_t keepMax) : m_keepMax(keepMax) {}

    void release(Bytes* b) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_free.size() < m_keepMax) m_free.push_back(b);
        else delete b;
    }

    std::mutex m_mutex;
    std::vector<Bytes*> m_free;
    size_t m_keepMax;
};

}  // namespace pd
