# First-Fit Free-List Arena Allocator

A custom heap allocator in C++ that requests memory directly from the
kernel via `mmap(2)`, manages it with an intrusive doubly-linked free list,
and uses boundary-tag coalescing to prevent fragmentation.

> **Status:** correctness validated under AddressSanitizer; ~17 ns per
> alloc/free cycle (64 B blocks, hot loop, single-threaded).

## What this demonstrates

- **Kernel-managed memory:** uses `mmap` with `MAP_PRIVATE | MAP_ANONYMOUS`
  to obtain page-aligned, zero-filled memory regions ("arenas") of 1 MiB
  by default. No dependency on `malloc`/`brk`/`sbrk`.
- **In-band metadata:** every allocated block carries a header (and a
  footer for boundary-tag walks) holding its size and flags. The user
  pointer returned by `allocate()` skips past the header; `deallocate()`
  recovers the header by subtracting the header size.
- **Bitwise alignment:** all sizes are stored with the low 3 bits reserved
  for flags. Because every block size is a multiple of 8, those bits are
  always zero in the "real size" and free for tagging. `bit 0 = is_free`.
- **Boundary-tag coalescing:** on `deallocate()`, we walk to the next block
  via pointer arithmetic and to the previous block via the previous
  footer. Adjacent free blocks merge in O(1), preventing fragmentation
  without a separate compaction pass.
- **Lazy arena growth:** when the free list has no block large enough, a
  new arena is `mmap`'d and laid down as one giant free block.
- **realloc support:** extends in place when the next block is free and
  large enough; otherwise falls back to allocate-copy-free.
- **Memory introspection:** `stats()` reports arena count, mapped/allocated/
  free bytes, and allocation count. `usable_size()` queries the usable
  payload size of any live allocation.
- **Bulk reset:** `deallocate_all()` reconstitutes each arena as a single
  free block, enabling fast reuse without individual frees.

## Block layout

```
 ┌──────────┬─────────────────────────┬──────────┐
 │  Header  │       Payload (n B)     │  Footer  │
 │  (size,  │  user pointer points    │  (size,  │
 │   flags) │  to start of payload    │   flags) │
 └──────────┴─────────────────────────┴──────────┘
```

When a block is free, its first 16 bytes of payload are reused as the
doubly-linked free-list pointers (prev/next). This is safe because no
user pointer can be live for a free block.

## Build & test

```bash
g++ -O2 -std=c++17 -Wall -Wextra test_allocator.cpp -o test_alloc
./test_alloc
```

ASan + UBSan run:

```bash
g++ -O1 -g -std=c++17 -fsanitize=address,undefined test_allocator.cpp -o test_alloc_asan
./test_alloc_asan
```

## Test results

```
[ RUN ] basic_alloc_free               OK
[ RUN ] alignment                      OK    (200 sizes, every payload 8-byte aligned)
[ RUN ] many_allocs_no_corruption      OK    (2,000 live allocs, byte-fill checksums)
[ RUN ] coalescing                     OK    (free middle+right+left, then allocate
                                              larger-than-any-single-block — succeeds)
[ RUN ] arena_growth                   OK    (100 × 1 KiB allocs in 4 KiB arenas →
                                              multiple arenas chained)
[ RUN ] realloc                        OK    (grow, shrink, null-pointer semantics)
[ RUN ] usable_size                    OK    (reports >= requested payload)
[ RUN ] stats                          OK    (arena count, mapped/allocated bytes)
[ RUN ] deallocate_all                 OK    (bulk reset, then large alloc succeeds)
```

ASan: clean across all tests.

## Benchmark

Single-threaded alloc/free of 64 B blocks, 2 M iterations, x86-64:

| Allocator | ns/op |
|---|---|
| arena (this project) | **17.2** |
| glibc `malloc`       | <1 (the hot loop optimizes away; not directly comparable) |

The glibc number is misleadingly fast: under `-O2`, the compiler+glibc
recognize the repeated same-size alloc/free pattern and serve from a
thread-local fast bin without ever touching the global heap. A more
honest comparison would interleave allocations of different sizes and
hold a working set live — left for future work.

## Known limitations

- **Not thread-safe.** A real production allocator (e.g. tcmalloc,
  jemalloc) maintains per-thread caches to avoid synchronization on the
  hot path. This project deliberately keeps that out of scope; the
  companion MPMC queue covers concurrency.
- **First-fit, not best-fit or segregated.** Best-fit reduces wasted space
  but costs more per allocation. Segregated free lists (one per size
  class) are what production allocators use. First-fit is the simplest
  correct strategy and a good starting point.
- **Whole arenas, no return-to-OS.** Once an arena is `mmap`'d, it's never
  `munmap`'d until destruction. A real allocator would `madvise(MADV_DONTNEED)`
  fully-free pages.
