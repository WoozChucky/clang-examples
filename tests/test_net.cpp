#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

#include "MpscRing.h"

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

int main() {
    test_mpsc_basic();
    test_mpsc_concurrent();
    if (g_Failures == 0) { std::printf("All net tests passed.\n"); return 0; }
    std::printf("%d net test(s) FAILED.\n", g_Failures);
    return 1;
}
