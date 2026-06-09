// arena_allocator.hpp — First-fit free-list arena allocator.
//
// Design: mmap'd arenas, boundary-tag coalescing, in-band flags in low 3 bits.
// Single-threaded. No libc malloc dependency.

#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace arena {

// Every block payload is 8-byte aligned. Low 3 bits of size fields = flags.
constexpr size_t kAlignment  = 8;
constexpr size_t kFlagFree   = 0x1;
constexpr size_t kSizeMask   = ~(kAlignment - 1);

inline constexpr size_t align_up(size_t n) {
    return (n + kAlignment - 1) & kSizeMask;
}

// Each block has a header (size|flags) and footer (size|flags) for O(1)
// coalescing. Free blocks reuse payload space for free-list links.
struct BlockHeader {
    size_t size_and_flags;

    size_t size()    const { return size_and_flags & kSizeMask; }
    bool   is_free() const { return size_and_flags & kFlagFree; }
    void   set_size(size_t s) { size_and_flags = (s & kSizeMask) | (size_and_flags & ~kSizeMask); }
    void   mark_free()        { size_and_flags |= kFlagFree; }
    void   mark_used()        { size_and_flags &= ~kFlagFree; }
};

struct FreeNode {
    BlockHeader header;
    FreeNode*   prev;
    FreeNode*   next;
};

struct BlockFooter {
    size_t size_and_flags;
};

constexpr size_t kHeaderSize  = sizeof(BlockHeader);
constexpr size_t kFooterSize  = sizeof(BlockFooter);
constexpr size_t kOverhead    = kHeaderSize + kFooterSize;
constexpr size_t kMinBlockSize = align_up(sizeof(FreeNode) + kFooterSize);

// ─── Pointer helpers ────────────────────────────────────────────────────
inline BlockFooter* footer_of(BlockHeader* h) {
    return reinterpret_cast<BlockFooter*>(reinterpret_cast<char*>(h) + h->size() - kFooterSize);
}

inline BlockHeader* next_block(BlockHeader* h) {
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(h) + h->size());
}

inline BlockHeader* prev_block(BlockHeader* h, void* arena_start) {
    if (reinterpret_cast<void*>(h) == arena_start) return nullptr;
    auto* fp = reinterpret_cast<BlockFooter*>(reinterpret_cast<char*>(h) - kFooterSize);
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(h) - (fp->size_and_flags & kSizeMask));
}

inline void* payload_of(BlockHeader* h) { return reinterpret_cast<char*>(h) + kHeaderSize; }
inline BlockHeader* header_of(void* p)  { return reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(p) - kHeaderSize); }

struct AllocatorStats {
    size_t arena_count     = 0;
    size_t total_mapped    = 0;
    size_t total_allocated = 0;
    size_t total_free      = 0;
    size_t allocation_count = 0;
};

class Allocator {
public:
    static constexpr size_t kDefaultArenaSize = 1 << 20;

    explicit Allocator(size_t arena_size = kDefaultArenaSize)
        : arena_size_(align_up(arena_size)) { grow_arena(); }

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
        size_t need = align_up(n) + kOverhead;
        if (need < kMinBlockSize) need = kMinBlockSize;

        FreeNode* node = free_head_;
        while (node) {
            if (node->header.size() >= need) {
                ++alloc_count_;
                return carve(node, need);
            }
            node = node->next;
        }

        grow_arena();
        return allocate(n);
    }

    void deallocate(void* p) {
        if (!p) return;

        BlockHeader* h = header_of(p);
        h->mark_free();
        footer_of(h)->size_and_flags = h->size_and_flags;
        void* arena_start = arena_containing(h);

        BlockHeader* n = next_block(h);
        if (in_arena(n, arena_start) && n->is_free()) {
            remove_from_free_list(reinterpret_cast<FreeNode*>(n));
            h->set_size(h->size() + n->size());
            footer_of(h)->size_and_flags = h->size_and_flags;
        }

        BlockHeader* p_blk = prev_block(h, arena_start);
        if (p_blk && p_blk->is_free()) {
            remove_from_free_list(reinterpret_cast<FreeNode*>(p_blk));
            p_blk->set_size(p_blk->size() + h->size());
            footer_of(p_blk)->size_and_flags = p_blk->size_and_flags;
            h = p_blk;
        }

        insert_free_list_head(reinterpret_cast<FreeNode*>(h));
    }

    void* realloc(void* p, size_t n) {
        if (!p) return allocate(n);
        if (n == 0) { deallocate(p); return nullptr; }

        BlockHeader* h = header_of(p);
        size_t old_payload = h->size() - kOverhead;
        if (align_up(n) <= old_payload) return p;

        size_t need = align_up(n) + kOverhead;
        if (need < kMinBlockSize) need = kMinBlockSize;

        size_t available = h->size();
        void* arena_start = arena_containing(h);

        BlockHeader* nxt = next_block(h);
        if (in_arena(nxt, arena_start) && nxt->is_free()) {
            size_t combined = available + nxt->size();
            if (combined >= need) {
                remove_from_free_list(reinterpret_cast<FreeNode*>(nxt));
                h->set_size(combined);
                footer_of(h)->size_and_flags = h->size_and_flags;

                size_t leftover = combined - need;
                if (leftover >= kMinBlockSize) {
                    h->set_size(need);
                    footer_of(h)->size_and_flags = h->size_and_flags;
                    BlockHeader* tail = next_block(h);
                    tail->size_and_flags = leftover | kFlagFree;
                    footer_of(tail)->size_and_flags = tail->size_and_flags;
                    insert_free_list_head(reinterpret_cast<FreeNode*>(tail));
                }
                return p;
            }
        }

        void* new_p = allocate(n);
        if (new_p) std::memcpy(new_p, p, old_payload);
        deallocate(p);
        return new_p;
    }

    size_t usable_size(void* p) const {
        if (!p) return 0;
        return header_of(p)->size() - kOverhead;
    }

    void deallocate_all() {
        free_head_ = nullptr;
        alloc_count_ = 0;
        for (Arena* a = arenas_; a; a = a->next) {
            auto* h = reinterpret_cast<BlockHeader*>(a->base);
            h->size_and_flags = arena_size_ | kFlagFree;
            footer_of(h)->size_and_flags = h->size_and_flags;
            insert_free_list_head(reinterpret_cast<FreeNode*>(h));
        }
    }

    AllocatorStats stats() const {
        AllocatorStats s;
        size_t used = 0;
        for (Arena* a = arenas_; a; a = a->next) {
            ++s.arena_count;
            s.total_mapped += a->size;
        }
        for (FreeNode* node = free_head_; node; node = node->next) {
            s.total_free += node->header.size();
        }
        s.total_allocated = s.total_mapped - s.total_free;
        s.allocation_count = alloc_count_;
        return s;
    }

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

    void* carve(FreeNode* node, size_t need) {
        BlockHeader* h = &node->header;
        size_t total = h->size();
        size_t leftover = total - need;
        remove_from_free_list(node);

        if (leftover >= kMinBlockSize) {
            h->set_size(need);
            h->mark_used();
            footer_of(h)->size_and_flags = h->size_and_flags;
            BlockHeader* tail = next_block(h);
            tail->size_and_flags = leftover | kFlagFree;
            footer_of(tail)->size_and_flags = tail->size_and_flags;
            insert_free_list_head(reinterpret_cast<FreeNode*>(tail));
        } else {
            h->mark_used();
            footer_of(h)->size_and_flags = h->size_and_flags;
        }
        return payload_of(h);
    }

    void grow_arena() {
        void* mem = ::mmap(nullptr, arena_size_, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) throw std::bad_alloc();
        Arena* a = new Arena{static_cast<char*>(mem), arena_size_, arenas_};
        arenas_ = a;
        total_mapped_ += arena_size_;
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
            if (reinterpret_cast<char*>(h) >= a->base &&
                reinterpret_cast<char*>(h) <  a->base + a->size) {
                return a->base;
            }
        }
        return nullptr;
    }

    bool in_arena(BlockHeader* h, void* arena_start) const {
        for (Arena* a = arenas_; a; a = a->next) {
            if (a->base == arena_start) {
                return reinterpret_cast<char*>(h) >= a->base &&
                       reinterpret_cast<char*>(h) <  a->base + a->size;
            }
        }
        return false;
    }

    size_t    arena_size_    = 0;
    Arena*    arenas_        = nullptr;
    FreeNode* free_head_     = nullptr;
    size_t    alloc_count_   = 0;
    size_t    total_mapped_  = 0;
};

} // namespace arena
