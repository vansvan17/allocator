// test_allocator.cpp — Functional and performance smoke tests.

#include "arena_allocator.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using arena::Allocator;

static int failures = 0;

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
    EXPECT(a.deallocate(nullptr); (void)0;);
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

static void benchmark() {
    std::fprintf(stderr, "\n[ BENCH ] alloc/free hot loop, fixed 64B, 2M iters\n");
    constexpr int N = 2'000'000;
    using clk = std::chrono::steady_clock;

    {
        Allocator a;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            void* p = a.allocate(64);
            a.deallocate(p);
        }
        auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      clk::now() - t0).count();
        std::fprintf(stderr, "  arena : %lld ns total, %.1f ns/op\n",
                     static_cast<long long>(dt), double(dt) / N);
    }
    {
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            void* p = std::malloc(64);
            std::free(p);
        }
        auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      clk::now() - t0).count();
        std::fprintf(stderr, "  malloc: %lld ns total, %.1f ns/op\n",
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

    std::fprintf(stderr, "\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");

    benchmark();
    return failures == 0 ? 0 : 1;
}
