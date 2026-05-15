// arena_allocator.hpp — First-fit free-list arena allocator.
//
// Design summary:
//   - Memory is requested directly from the kernel via mmap(2) in large
//     "arenas" (default 1 MiB each). This bypasses libc malloc entirely and
//     gives us full control over the heap layout.
//
//   - Each arena is carved into variable-sized blocks. Every block carries
//     an in-band header (and footer, for boundary-tag coalescing) holding
//     the block's size and a "free" flag.
//
//   - Free blocks live on a doubly linked free list. Allocation walks the
//     list and returns the first block whose size >= requested (first-fit).
//
//   - All sizes are stored with the low 3 bits reserved for flags. This is
//     safe because all allocations are 8-byte aligned, so the low 3 bits of
//     any block size are always zero. Bit 0 = "is_free".
//
//   - On free(), we check the immediately-preceding and immediately-following
//     blocks via boundary-tag walks: if either is free, we coalesce. This
//     prevents fragmentation without a separate compaction pass.
//
// What this allocator is NOT:
//   - thread-safe (single-threaded by design; the lock-free MPMC queue
//     handles concurrency in the companion project)
//   - drop-in for new/delete (deliberately a learning project; no realloc)

#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace arena {

// ─── Alignment ──────────────────────────────────────────────────────────
// Every block payload is aligned to 8 bytes. This is enough for any
// fundamental type on x86-64 (long double aside, which we ignore).
// Hardware loads at unaligned addresses are either slow or undefined,
// so we pad sizes up at allocation time.
constexpr size_t kAlignment = 8;

// We reserve the bottom 3 bits of each size field for flags. Bit 0 = free.
// Bits 1-2 are spare for future use. The mask below extracts the real size.
constexpr size_t kFlagFree = 0x1;
constexpr size_t kSizeMask = ~(kAlignment - 1);   // == 0xFFF...FFF8

inline constexpr size_t align_up(size_t n) {
    return (n + kAlignment - 1) & kSizeMask;
}

// ─── Block layout ───────────────────────────────────────────────────────
//
//  ┌──────────┬─────────────────────────┬──────────┐
//  │  Header  │       Payload (n B)     │  Footer  │
//  │  (size,  │  user pointer points    │  (size,  │
//  │   flags) │  to start of payload    │   flags) │
//  └──────────┴─────────────────────────┴──────────┘
//
// Both header and footer hold (size | flags). The footer is what makes
// O(1) "find my previous neighbour" possible: from a block's start, look
// 8 bytes back to read the previous block's footer, which tells us its
// size and free state.
//
// When a block is free, its payload's first 16 bytes are reused as the
// free-list links (prev/next pointers). This is safe because no user
// pointer can be referencing a free block.

struct BlockHeader {
    size_t size_and_flags;          // size in high bits, flags in low 3 bits

    size_t size() const { return size_and_flags & kSizeMask; }
    bool   is_free() const { return size_and_flags & kFlagFree; }
    void   set_size(size_t s) {
        size_and_flags = (s & kSizeMask) | (size_and_flags & ~kSizeMask);
    }
    void   mark_free()  { size_and_flags |= kFlagFree; }
    void   mark_used()  { size_and_flags &= ~kFlagFree; }
};

// Free-list links overlay the payload of a free block.
struct FreeNode {
    BlockHeader header;
    FreeNode*   prev;
    FreeNode*   next;
};

// Footer mirrors the header. Sits at the very end of the block's payload.
struct BlockFooter {
    size_t size_and_flags;
};

constexpr size_t kHeaderSize = sizeof(BlockHeader);
constexpr size_t kFooterSize = sizeof(BlockFooter);
constexpr size_t kOverhead   = kHeaderSize + kFooterSize;

// Minimum block size needs to fit the free-list links when freed.
constexpr size_t kMinBlockSize = align_up(sizeof(FreeNode) + kFooterSize);

// ─── Pointer arithmetic helpers ─────────────────────────────────────────
inline BlockFooter* footer_of(BlockHeader* h) {
    auto* base = reinterpret_cast<char*>(h);
    return reinterpret_cast<BlockFooter*>(base + h->size() - kFooterSize);
}

inline BlockHeader* next_block(BlockHeader* h) {
    auto* base = reinterpret_cast<char*>(h);
    return reinterpret_cast<BlockHeader*>(base + h->size());
}

// Look back at the previous block's footer to find its header.
// Returns nullptr if `h` is at the start of an arena.
inline BlockHeader* prev_block(BlockHeader* h, void* arena_start) {
    if (reinterpret_cast<void*>(h) == arena_start) return nullptr;
    auto* footer_ptr = reinterpret_cast<BlockFooter*>(
        reinterpret_cast<char*>(h) - kFooterSize);
    size_t prev_size = footer_ptr->size_and_flags & kSizeMask;
    return reinterpret_cast<BlockHeader*>(
        reinterpret_cast<char*>(h) - prev_size);
}

inline void* payload_of(BlockHeader* h) {
    return reinterpret_cast<char*>(h) + kHeaderSize;
}

inline BlockHeader* header_of(void* p) {
    return reinterpret_cast<BlockHeader*>(
        reinterpret_cast<char*>(p) - kHeaderSize);
}

// ─── Allocator ──────────────────────────────────────────────────────────
class Allocator {
public:
    static constexpr size_t kDefaultArenaSize = 1 << 20;   // 1 MiB

    explicit Allocator(size_t arena_size = kDefaultArenaSize)
        : arena_size_(align_up(arena_size)) {
        grow_arena();
    }

    ~Allocator() {
        Arena* a = arenas_;
        while (a) {
            Arena* next = a->next;
            ::munmap(a->base, a->size);
            delete a;
            a = next;
        }
    }

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    void* allocate(size_t n) {
        if (n == 0) return nullptr;

        // Round up requested size to alignment + overhead.
        size_t need = align_up(n) + kOverhead;
        if (need < kMinBlockSize) need = kMinBlockSize;

        // First-fit search through the free list.
        FreeNode* node = free_head_;
        while (node) {
            if (node->header.size() >= need) {
                return carve(node, need);
            }
            node = node->next;
        }

        // No block big enough; grow.
        grow_arena();
        return allocate(n);   // tail-call into the search again
    }

    void deallocate(void* p) {
        if (!p) return;

        BlockHeader* h = header_of(p);
        h->mark_free();

        // Mirror flags into footer for boundary-tag walks.
        footer_of(h)->size_and_flags = h->size_and_flags;

        // Find which arena this block lives in (for prev-bound check).
        void* arena_start = arena_containing(h);

        // Coalesce with NEXT neighbour if free.
        BlockHeader* n = next_block(h);
        if (in_arena(n, arena_start) && n->is_free()) {
            remove_from_free_list(reinterpret_cast<FreeNode*>(n));
            h->set_size(h->size() + n->size());
            footer_of(h)->size_and_flags = h->size_and_flags;
        }

        // Coalesce with PREV neighbour if free.
        BlockHeader* p_blk = prev_block(h, arena_start);
        if (p_blk && p_blk->is_free()) {
            remove_from_free_list(reinterpret_cast<FreeNode*>(p_blk));
            p_blk->set_size(p_blk->size() + h->size());
            footer_of(p_blk)->size_and_flags = p_blk->size_and_flags;
            h = p_blk;
        }

        // Insert (possibly enlarged) free block at head of free list.
        insert_free_list_head(reinterpret_cast<FreeNode*>(h));
    }

    // For debugging / benchmarks.
    size_t arena_count() const {
        size_t c = 0;
        for (Arena* a = arenas_; a; a = a->next) ++c;
        return c;
    }

private:
    struct Arena {
        char*  base;
        size_t size;
        Arena* next;
    };

    // Carve a `need`-sized block from the front of `node`. Insert any
    // remainder back onto the free list as a new free block.
    void* carve(FreeNode* node, size_t need) {
        BlockHeader* h = &node->header;
        size_t total = h->size();
        size_t leftover = total - need;

        remove_from_free_list(node);

        if (leftover >= kMinBlockSize) {
            // Split: shrink current block to `need`, create a free block
            // of `leftover` bytes immediately after.
            h->set_size(need);
            h->mark_used();
            footer_of(h)->size_and_flags = h->size_and_flags;

            BlockHeader* tail = next_block(h);
            tail->size_and_flags = leftover | kFlagFree;
            footer_of(tail)->size_and_flags = tail->size_and_flags;
            insert_free_list_head(reinterpret_cast<FreeNode*>(tail));
        } else {
            // Take the whole block — leftover too small to be useful.
            h->mark_used();
            footer_of(h)->size_and_flags = h->size_and_flags;
        }

        return payload_of(h);
    }

    void grow_arena() {
        // mmap MAP_ANONYMOUS gives us zero-filled pages straight from the
        // kernel, no file backing. MAP_PRIVATE means writes are private to
        // this process. PROT_READ|PROT_WRITE is the obvious choice.
        void* mem = ::mmap(nullptr, arena_size_,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) throw std::bad_alloc();

        Arena* a = new Arena{static_cast<char*>(mem), arena_size_, arenas_};
        arenas_ = a;

        // Lay down one giant free block spanning the entire arena.
        auto* h = reinterpret_cast<BlockHeader*>(a->base);
        h->size_and_flags = arena_size_ | kFlagFree;
        footer_of(h)->size_and_flags = h->size_and_flags;
        insert_free_list_head(reinterpret_cast<FreeNode*>(h));
    }

    void insert_free_list_head(FreeNode* n) {
        n->prev = nullptr;
        n->next = free_head_;
        if (free_head_) free_head_->prev = n;
        free_head_ = n;
    }

    void remove_from_free_list(FreeNode* n) {
        if (n->prev) n->prev->next = n->next;
        else         free_head_ = n->next;
        if (n->next) n->next->prev = n->prev;
    }

    void* arena_containing(BlockHeader* h) const {
        for (Arena* a = arenas_; a; a = a->next) {
            char* end = a->base + a->size;
            if (reinterpret_cast<char*>(h) >= a->base &&
                reinterpret_cast<char*>(h) <  end) {
                return a->base;
            }
        }
        return nullptr;
    }

    bool in_arena(BlockHeader* h, void* arena_start) const {
        for (Arena* a = arenas_; a; a = a->next) {
            if (a->base == arena_start) {
                char* end = a->base + a->size;
                return reinterpret_cast<char*>(h) <  end &&
                       reinterpret_cast<char*>(h) >= a->base;
            }
        }
        return false;
    }

    size_t    arena_size_;
    Arena*    arenas_    = nullptr;
    FreeNode* free_head_ = nullptr;
};

} // namespace arena
