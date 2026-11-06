/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#include "KAssert.h"
#include "Utils.hpp"

namespace kotlin::gc {

// Kore: the mark stack of the generational GC: pointers to the objects to trace, in chunks of `kChunkCapacity`. Unlike
// the intrusive list of the other collectors, it keeps nothing in the objects, so the mark state is a single bit (an
// object is pushed once, by whoever sets its mark bit).
//
// It has the subset of the `intrusive_forward_list` interface that `ParallelProcessor` uses: one stack is a batch
// there (a batch of `kChunkCapacity` elements is one chunk, so sharing a batch moves a pointer), and another is a
// worker's overflow list.
//
// Every chunk in the chain holds at least one element, except the top one when it is the only one (kept for reuse).
template <typename T, size_t kChunkCapacity = 512>
class ChunkedStack : private MoveOnly {
    struct Chunk {
        Chunk* next;
        size_t size;
        T* items[kChunkCapacity];
    };

public:
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using pointer = T*;

    // `splice_after` takes the source stack through these (the positions themselves are meaningless: a stack only
    // gives and takes elements at its top).
    struct position {
        ChunkedStack* owner;
    };

    ChunkedStack() noexcept = default;
    ChunkedStack(ChunkedStack&& other) noexcept : top_(other.top_) { other.top_ = nullptr; }
    ChunkedStack& operator=(ChunkedStack&& other) noexcept {
        if (this != &other) {
            clear();
            top_ = other.top_;
            other.top_ = nullptr;
        }
        return *this;
    }
    ~ChunkedStack() { clear(); }

    position before_begin() noexcept { return position{this}; }
    position end() noexcept { return position{nullptr}; }

    bool empty() const noexcept { return top_ == nullptr || top_->size == 0; }

    // Always succeeds: the caller decides whether to push (by setting the mark bit of the object first).
    ALWAYS_INLINE bool try_push_front(reference value) noexcept {
        Chunk* chunk = top_;
        if (__builtin_expect(chunk == nullptr || chunk->size == kChunkCapacity, false)) chunk = pushChunk();
        chunk->items[chunk->size++] = &value;
        return true;
    }

    ALWAYS_INLINE pointer try_pop_front() noexcept {
        Chunk* chunk = top_;
        if (chunk == nullptr || chunk->size == 0) return nullptr;
        pointer value = chunk->items[--chunk->size];
        if (__builtin_expect(chunk->size == 0 && chunk->next != nullptr, false)) {
            top_ = chunk->next;
            std::free(chunk);
        }
        return value;
    }

    // Moves up to `count` elements of `from.owner` to this stack (whole chunks where possible). Returns their number.
    size_type splice_after(position, position from, position, size_type count) noexcept {
        ChunkedStack& source = *from.owner;
        RuntimeAssert(&source != this, "Splicing a stack into itself");
        size_type moved = 0;
        while (moved < count && !source.empty()) {
            Chunk* chunk = source.top_;
            if (chunk->size <= count - moved) {
                source.top_ = chunk->next;
                moved += chunk->size;
                if (top_ != nullptr && top_->size == 0) {
                    RuntimeAssert(top_->next == nullptr, "Only a lone top chunk may be empty");
                    std::free(top_);
                    top_ = nullptr;
                }
                chunk->next = top_;
                top_ = chunk;
            } else {
                while (moved < count) {
                    try_push_front(*source.try_pop_front());
                    ++moved;
                }
            }
        }
        return moved;
    }

    void clear() noexcept {
        while (top_ != nullptr) {
            Chunk* next = top_->next;
            std::free(top_);
            top_ = next;
        }
    }

    // Testing.
    size_type size() const noexcept {
        size_type size = 0;
        for (Chunk* chunk = top_; chunk != nullptr; chunk = chunk->next) size += chunk->size;
        return size;
    }

private:
    NO_INLINE Chunk* pushChunk() noexcept {
        if (top_ != nullptr && top_->size == 0) return top_;
        auto* chunk = static_cast<Chunk*>(std::malloc(sizeof(Chunk)));
        RuntimeCheck(chunk != nullptr, "Out of memory for the mark stack");
        chunk->next = top_;
        chunk->size = 0;
        top_ = chunk;
        return chunk;
    }

    Chunk* top_ = nullptr;
};

} // namespace kotlin::gc
