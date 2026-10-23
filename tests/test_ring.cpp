// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/BufferPool.h"
#include "engine/RingBuffer.h"
#include "testing.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace pd;
using namespace std::chrono_literals;

TEST_CASE("ring keeps order and byte count") {
    RingBuffer<int> r(100);
    CHECK(r.push(1, 30));
    CHECK(r.push(2, 30));
    CHECK(r.push(3, 30));
    CHECK_EQ(r.bytes(), size_t(90));
    CHECK(!r.push(4, 30));  // would exceed capacity: refused, never blocks
    CHECK_EQ(r.count(), size_t(3));
    int v = 0;
    CHECK(r.tryPop(v));
    CHECK_EQ(v, 1);
    CHECK(r.push(4, 30));
    for (int want : {2, 3, 4}) {
        CHECK(r.tryPop(v));
        CHECK_EQ(v, want);
    }
    CHECK(!r.tryPop(v));
    CHECK_EQ(r.bytes(), size_t(0));
    CHECK_EQ(r.peakBytes(), size_t(90));
}

TEST_CASE("an item larger than the ring still fits when empty") {
    RingBuffer<int> r(10);
    CHECK(r.push(1, 50));
    CHECK(!r.push(2, 1));
}

TEST_CASE("fill level and clear") {
    RingBuffer<int> r(200);
    r.push(1, 50);
    CHECK(r.fill() > 0.24 && r.fill() < 0.26);
    r.clear();
    CHECK_EQ(r.count(), size_t(0));
    CHECK(r.fill() == 0.0);
}

TEST_CASE("pop waits for a producer, close wakes it") {
    RingBuffer<int> r(1000);
    std::thread t([&] {
        std::this_thread::sleep_for(20ms);
        r.push(42, 1);
    });
    int v = 0;
    CHECK(r.pop(v, 2000ms));
    CHECK_EQ(v, 42);
    t.join();
    std::thread c([&] {
        std::this_thread::sleep_for(20ms);
        r.close();
    });
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!r.pop(v, 5000ms));
    CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
    c.join();
}

TEST_CASE("producer and consumer threads lose nothing") {
    RingBuffer<int> r(64 * 1024);
    std::atomic<int> refused{0};
    const int n = 200000;
    std::thread p([&] {
        for (int i = 0; i < n; ++i)
            while (!r.push(i, 16)) {
                ++refused;
                std::this_thread::yield();
            }
    });
    int expect = 0;
    bool inOrder = true;
    while (expect < n) {
        int v;
        if (r.pop(v, 1000ms)) {
            inOrder &= v == expect;
            ++expect;
        } else {
            break;
        }
    }
    p.join();
    CHECK_EQ(expect, n);
    CHECK(inOrder);
}

TEST_CASE("blocking queue applies back-pressure") {
    BlockingQueue<int> q(100);
    std::atomic<int> pushed{0};
    std::thread p([&] {
        for (int i = 0; i < 10; ++i) {
            q.push(i, 40);
            ++pushed;
        }
        q.close();
    });
    std::this_thread::sleep_for(50ms);
    CHECK(pushed.load() <= 3);  // capacity 100 bytes holds two or three items
    int v, got = 0;
    while (q.pop(v)) ++got;
    p.join();
    CHECK_EQ(got, 10);
}

TEST_CASE("buffer pool recycles buffers") {
    auto pool = BufferPool::create(4);
    const uint8_t* first;
    {
        BytesPtr a = pool->acquire(1000);
        first = a->data();
        CHECK(a->size() >= 1000);
    }
    BytesPtr b = pool->acquire(500);
    CHECK(b->data() == first);  // same storage came back
    BytesPtr c = pool->acquire(2000);
    CHECK(c->size() >= 2000);
}

TEST_CASE("buffers outlive their pool") {
    BytesPtr keep;
    {
        auto pool = BufferPool::create(4);
        keep = pool->acquire(64);
    }
    (*keep)[0] = 1;  // still valid, freed normally later
    CHECK(keep->size() >= 64);
}

TEST_MAIN()
