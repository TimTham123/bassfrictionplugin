// Friction - lock-free primitives used between the audio thread and other threads
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace fr
{

/**
    Single-producer / single-consumer FIFO of trivially copyable items.
    push() and pop() are wait-free; storage is allocated once in prepare().
*/
template <typename T>
class SpscFifo
{
public:
    void prepare (std::size_t capacityPow2)
    {
        std::size_t cap = 1;
        while (cap < capacityPow2)
            cap <<= 1;
        buffer.assign (cap, T());
        mask = cap - 1;
        head.store (0);
        tail.store (0);
    }

    /** Producer side. Returns false (and drops the item) when full. */
    bool push (const T& item) noexcept
    {
        const std::size_t h = head.load (std::memory_order_relaxed);
        const std::size_t t = tail.load (std::memory_order_acquire);
        if (h - t > mask)
            return false;
        buffer[h & mask] = item;
        head.store (h + 1, std::memory_order_release);
        return true;
    }

    /** Consumer side. */
    bool pop (T& out) noexcept
    {
        const std::size_t t = tail.load (std::memory_order_relaxed);
        const std::size_t h = head.load (std::memory_order_acquire);
        if (t == h)
            return false;
        out = buffer[t & mask];
        tail.store (t + 1, std::memory_order_release);
        return true;
    }

    std::size_t size() const noexcept
    {
        return head.load (std::memory_order_acquire) - tail.load (std::memory_order_acquire);
    }

    /** Consumer side: discard everything. */
    void clear() noexcept { tail.store (head.load (std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<T> buffer;
    std::size_t mask = 0;
    alignas (64) std::atomic<std::size_t> head { 0 };
    alignas (64) std::atomic<std::size_t> tail { 0 };
};

/**
    Triple buffer: one writer publishes the latest value, one reader always gets the most recent
    complete value. Never blocks; the writer never waits for the reader.
*/
template <typename T>
class TripleBuffer
{
public:
    TripleBuffer() = default;

    /** Writer: obtain the back buffer, fill it, then call publish(). */
    T& back() noexcept { return slots[backIdx]; }

    void publish() noexcept
    {
        // swap back with middle, mark dirty
        const int prev = middle.exchange (backIdx | kDirty, std::memory_order_acq_rel);
        backIdx = prev & kIndexMask;
    }

    /** Reader: returns true if a new value was taken; read it via front(). */
    bool fetch() noexcept
    {
        if ((middle.load (std::memory_order_relaxed) & kDirty) == 0)
            return false;
        const int prev = middle.exchange (frontIdx, std::memory_order_acq_rel);
        frontIdx = prev & kIndexMask;
        return true;
    }

    const T& front() const noexcept { return slots[frontIdx]; }

private:
    static constexpr int kDirty = 4;
    static constexpr int kIndexMask = 3;
    T slots[3] {};
    int backIdx = 0;
    int frontIdx = 1;
    std::atomic<int> middle { 2 };
};

} // namespace fr
