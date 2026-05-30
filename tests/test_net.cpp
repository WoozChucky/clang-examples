#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

#include "MpscRing.h"
#include "NetBufferPool.h"
#include "NetServices.h"
#include <cstring>

void platform_debug_break(const char*, const char*, int, const char*) {}

static int g_Failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_Failures; } } while (0)

static void test_mpsc_basic() {
    MpscRing<int, 4> r;
    int out = 0;
    CHECK(!r.Dequeue(out), "mpsc: empty dequeue false");
    CHECK(r.Enqueue(1), "mpsc: enqueue 1");
    CHECK(r.Enqueue(2), "mpsc: enqueue 2");
    CHECK(r.Enqueue(3), "mpsc: enqueue 3");
    CHECK(r.Enqueue(4), "mpsc: enqueue 4 (full ring of 4 holds 4)");
    CHECK(!r.Enqueue(5), "mpsc: enqueue 5 fails (full)");
    CHECK(r.Dequeue(out) && out == 1, "mpsc: dequeue 1");
    CHECK(r.Dequeue(out) && out == 2, "mpsc: dequeue 2");
}

static void test_mpsc_concurrent() {
    constexpr int K = 8, M = 10000;
    MpscRing<int, 1024> r;
    std::atomic<int> producedTotal{0};
    std::vector<std::thread> producers;
    std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) {
        producers.emplace_back([&]{
            while (!go.load()) {}
            for (int i = 0; i < M; ++i) {
                while (!r.Enqueue(1)) { std::this_thread::yield(); }
                producedTotal.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true);
    int received = 0;
    int out = 0;
    while (received < K * M) {
        if (r.Dequeue(out)) received += out;
        else std::this_thread::yield();
    }
    for (auto& t : producers) t.join();
    CHECK(received == K * M, "mpsc: concurrent received exactly K*M");
    CHECK(!r.Dequeue(out), "mpsc: empty after draining all");
}

static void test_pool_basic() {
    NetBufferPool pool(64, 4);   // 4 blocks of 64 bytes
    uint32_t i0 = 0, i1 = 0;
    std::byte* b0 = pool.Acquire(i0);
    CHECK(b0 != nullptr, "pool: acquire 0 non-null");
    std::byte* b1 = pool.Acquire(i1);
    CHECK(b1 != nullptr && i1 != i0, "pool: acquire 1 distinct index");
    const char* msg = "hi";
    std::memcpy(b0, msg, 3);
    CHECK(std::memcmp(pool.Block(i0), "hi", 3) == 0, "pool: Block(i) round-trips bytes");
    pool.Release(i0);
    pool.Release(i1);
    uint32_t i2;
    CHECK(pool.Acquire(i2) != nullptr, "pool: reacquire after release");
    pool.Release(i2);
}

static void test_pool_exhaustion() {
    NetBufferPool pool(32, 2);
    uint32_t a, b, c;
    CHECK(pool.Acquire(a) != nullptr, "pool: acq 1");
    CHECK(pool.Acquire(b) != nullptr, "pool: acq 2");
    CHECK(pool.Acquire(c) == nullptr, "pool: acq 3 exhausted -> null");
    pool.Release(a); pool.Release(b);
}

static void test_pool_concurrent() {
    NetBufferPool pool(64, 256);
    constexpr int K = 8, iters = 20000;
    std::vector<std::thread> ts;
    std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) ts.emplace_back([&]{
        while (!go.load()) {}
        for (int i = 0; i < iters; ++i) {
            uint32_t idx;
            std::byte* p = pool.Acquire(idx);
            if (p) { p[0] = std::byte{1}; pool.Release(idx); }
        }
    });
    go.store(true);
    for (auto& t : ts) t.join();
    std::vector<uint32_t> held;
    for (int i = 0; i < 256; ++i) { uint32_t idx; if (pool.Acquire(idx)) held.push_back(idx); }
    CHECK(held.size() == 256, "pool: all blocks free after concurrent churn (no leak/double-free)");
    for (uint32_t idx : held) pool.Release(idx);
}

static void test_net_types() {
    netlib::Endpoint ep{ "127.0.0.1", 9000 };
    NetServerConfig sc{}; sc.bind = ep;
    CHECK(sc.bind.port == 9000, "types: NetServerConfig.bind");
    CHECK(sc.gameResident == false, "types: gameResident defaults false");

    NetEvent ev{};
    ev.kind = NetEventKind::Message;
    ev.opcode = 7;
    CHECK(ev.kind == NetEventKind::Message && ev.opcode == 7, "types: NetEvent fields");
    CHECK(NetHandle::Invalid == NetHandle{0}, "types: NetHandle::Invalid is 0");
}

int main() {
    test_net_types();
    test_mpsc_basic();
    test_mpsc_concurrent();
    test_pool_basic();
    test_pool_exhaustion();
    test_pool_concurrent();
    if (g_Failures == 0) { std::printf("All net tests passed.\n"); return 0; }
    std::printf("%d net test(s) FAILED.\n", g_Failures);
    return 1;
}
