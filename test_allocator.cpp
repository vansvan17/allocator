#include "arena_allocator.hpp"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using arena::Allocator;
using arena::ConcurrentAllocator;

static std::atomic<int> failures{0};

static inline void keep_pointer_observable(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

#define EXPECT(cond) do {                                                 \
    if (!(cond)) {                                                        \
        std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++failures;                                                       \
    }                                                                     \
} while (0)

static void test_basic_alloc_free() {
    std::fprintf(stderr, "[ RUN ] basic_alloc_free\n");
    Allocator a;
    void* p = a.allocate(64);
    EXPECT(p != nullptr);
    std::memset(p, 0xAB, 64);
    a.deallocate(p);
}

static void test_alignment() {
    std::fprintf(stderr, "[ RUN ] alignment\n");
    Allocator a;
    for (size_t n = 1; n <= 200; ++n) {
        void* p = a.allocate(n);
        EXPECT(p != nullptr);
        EXPECT(reinterpret_cast<uintptr_t>(p) % arena::kAlignment == 0);
        a.deallocate(p);
    }
}

static void test_many_allocs_no_corruption() {
    std::fprintf(stderr, "[ RUN ] many_allocs_no_corruption\n");
    Allocator a;
    std::vector<std::pair<char*, size_t>> live;
    for (int i = 0; i < 2000; ++i) {
        size_t n = 16 + (i * 31) % 512;
        auto* p = static_cast<char*>(a.allocate(n));
        EXPECT(p != nullptr);
        std::memset(p, static_cast<int>(i & 0xFF), n);
        live.push_back({p, n});
    }
    for (size_t i = 0; i < live.size(); ++i) {
        auto [p, n] = live[i];
        for (size_t j = 0; j < n; ++j) {
            EXPECT(static_cast<unsigned char>(p[j]) ==
                   static_cast<unsigned char>(i & 0xFF));
        }
    }
    for (auto [p, _] : live) a.deallocate(p);
}

static void test_coalescing() {
    std::fprintf(stderr, "[ RUN ] coalescing\n");
    Allocator a;
    void* p1 = a.allocate(256);
    void* p2 = a.allocate(256);
    void* p3 = a.allocate(256);
    a.deallocate(p2);
    a.deallocate(p3);
    a.deallocate(p1);
    void* big = a.allocate(700);
    EXPECT(big != nullptr);
    a.deallocate(big);
}

static void test_arena_growth() {
    std::fprintf(stderr, "[ RUN ] arena_growth\n");
    Allocator a(4096);
    std::vector<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        void* p = a.allocate(1024);
        EXPECT(p != nullptr);
        ptrs.push_back(p);
    }
    EXPECT(a.arena_count() > 1);
    for (auto* p : ptrs) a.deallocate(p);
}

static void test_realloc() {
    std::fprintf(stderr, "[ RUN ] realloc\n");
    Allocator a;
    void* p = a.allocate(32);
    EXPECT(p != nullptr);
    std::memcpy(p, "hello arena", 12);

    p = a.realloc(p, 1024);
    EXPECT(p != nullptr);
    EXPECT(std::memcmp(p, "hello arena", 12) == 0);

    void* z = a.realloc(nullptr, 64);
    EXPECT(z != nullptr);

    void* shrink = a.realloc(z, 8);
    EXPECT(shrink == z);

    a.deallocate(shrink);
    a.deallocate(nullptr);
}

static void test_usable_size() {
    std::fprintf(stderr, "[ RUN ] usable_size\n");
    Allocator a;
    void* p = a.allocate(100);
    EXPECT(p != nullptr);
    EXPECT(a.usable_size(p) >= 100);
    a.deallocate(p);
    EXPECT(a.usable_size(nullptr) == 0);
}

static void test_stats() {
    std::fprintf(stderr, "[ RUN ] stats\n");
    Allocator a;
    auto s0 = a.stats();
    EXPECT(s0.arena_count >= 1);
    EXPECT(s0.total_mapped > 0);

    void* p = a.allocate(128);
    EXPECT(p != nullptr);
    auto s1 = a.stats();
    EXPECT(s1.allocation_count >= 1);

    a.deallocate(p);
}

static void test_deallocate_all() {
    std::fprintf(stderr, "[ RUN ] deallocate_all\n");
    Allocator a;
    std::vector<void*> ptrs;
    for (int i = 0; i < 100; ++i) ptrs.push_back(a.allocate(64));
    a.deallocate_all();
    void* big = a.allocate(1024 * 512);
    EXPECT(big != nullptr);
    a.deallocate(big);
}

static void test_concurrent_cross_thread_free() {
    std::fprintf(stderr, "[ RUN ] concurrent_cross_thread_free\n");
    constexpr int kThreads = 16;
    constexpr int kItems = 512;
    ConcurrentAllocator allocator;
    std::vector<std::vector<void*>> blocks(kThreads);
    std::vector<std::vector<size_t>> sizes(kThreads);
    std::atomic<bool> contents_ok{true};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            blocks[t].reserve(kItems);
            sizes[t].reserve(kItems);
            for (int i = 0; i < kItems; ++i) {
                const size_t size = 16 + ((t * 37 + i * 19) % 240);
                void* p = allocator.allocate(size);
                EXPECT(p != nullptr);
                std::memset(p, t, size);
                blocks[t].push_back(p);
                sizes[t].push_back(size);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    threads.clear();

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            const int source = (t + 1) % kThreads;
            for (int i = 0; i < kItems; ++i) {
                auto* p = static_cast<unsigned char*>(blocks[source][i]);
                for (size_t j = 0; j < sizes[source][i]; ++j) {
                    if (p[j] != static_cast<unsigned char>(source)) {
                        contents_ok.store(false, std::memory_order_relaxed);
                        break;
                    }
                }
                allocator.deallocate(p);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT(contents_ok.load(std::memory_order_relaxed));
}

static void test_concurrent_realloc() {
    std::fprintf(stderr, "[ RUN ] concurrent_realloc\n");
    ConcurrentAllocator allocator;
    auto* original = static_cast<unsigned char*>(allocator.allocate(64));
    std::memset(original, 0x5a, 64);
    void* resized = nullptr;
    std::thread thread([&] { resized = allocator.realloc(original, 4096); });
    thread.join();
    EXPECT(resized != nullptr);
    auto* bytes = static_cast<unsigned char*>(resized);
    for (int i = 0; i < 64; ++i) EXPECT(bytes[i] == 0x5a);
    allocator.deallocate(resized);
}

static void benchmark_concurrent() {
    std::fprintf(stderr,
                 "\n[ BENCH ] 16-thread alloc/free, fixed 64B, 500k pairs/thread\n");
    constexpr int kThreads = 16;
    constexpr int kPairsPerThread = 500'000;
    ConcurrentAllocator allocator;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<std::uintptr_t> checksum{0};
    std::vector<std::thread> threads;
    std::vector<long long> thread_ns(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            void* warmup = allocator.allocate(64);
            allocator.deallocate(warmup);
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {}

            const auto thread_start = std::chrono::steady_clock::now();
            std::uintptr_t local = 0;
            for (int i = 0; i < kPairsPerThread; ++i) {
                void* p = allocator.allocate(64);
                keep_pointer_observable(p);
                static_cast<unsigned char*>(p)[0] =
                    static_cast<unsigned char>(i);
                local += reinterpret_cast<std::uintptr_t>(p) ^
                         static_cast<std::uintptr_t>(i);
                allocator.deallocate(p);
            }
            thread_ns[t] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - thread_start).count();
            checksum.fetch_add(local, std::memory_order_relaxed);
        });
    }

    while (ready.load(std::memory_order_acquire) != kThreads) {}
    const auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count();
    const long long total_pairs =
        static_cast<long long>(kThreads) * kPairsPerThread;
    long long summed_thread_ns = 0;
    for (long long duration : thread_ns) summed_thread_ns += duration;
    std::fprintf(stderr, "  aggregate throughput: %.1f ns/pair (%lld total pairs)\n",
                 static_cast<double>(elapsed) / total_pairs, total_pairs);
    std::fprintf(stderr, "  mean thread latency:  %.1f ns/pair\n",
                 static_cast<double>(summed_thread_ns) / total_pairs);
    std::fprintf(stderr, "  checksum: %zu\n",
                 static_cast<size_t>(checksum.load(std::memory_order_relaxed)));
}

static void benchmark() {
    std::fprintf(stderr, "\n[ BENCH ] alloc/free hot loop, fixed 64B, 2M iters\n");
    constexpr int N = 2'000'000;
    using clk = std::chrono::steady_clock;

    {
        Allocator a;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            void* p = a.allocate(64);
            keep_pointer_observable(p);
            a.deallocate(p);
        }
        auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      clk::now() - t0).count();
        std::fprintf(stderr, "  arena : %lld ns total, %.1f ns/op\n",
                     static_cast<long long>(dt), double(dt) / N);
    }
}

int main() {
    test_basic_alloc_free();
    test_alignment();
    test_many_allocs_no_corruption();
    test_coalescing();
    test_arena_growth();
    test_realloc();
    test_usable_size();
    test_stats();
    test_deallocate_all();
    test_concurrent_cross_thread_free();
    test_concurrent_realloc();

    const int failure_count = failures.load(std::memory_order_relaxed);
    std::fprintf(stderr, "\n%s\n",
                 failure_count == 0 ? "all tests passed" : "TESTS FAILED");

    benchmark();
    benchmark_concurrent();
    return failure_count == 0 ? 0 : 1;
}
