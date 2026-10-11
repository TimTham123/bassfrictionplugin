// Friction - global operator new / delete replacement that counts allocations per thread
#include "AllocCounter.h"

#include <cstdlib>
#include <new>

namespace
{
    thread_local bool tCounting = false;
    thread_local long long tCount = 0;

    void* allocate (std::size_t n)
    {
        if (tCounting)
            ++tCount;
        if (void* p = std::malloc (n == 0 ? 1 : n))
            return p;
        throw std::bad_alloc();
    }
}

void ft::AllocCounter::begin() noexcept
{
    tCount = 0;
    tCounting = true;
}

long long ft::AllocCounter::end() noexcept
{
    tCounting = false;
    return tCount;
}

// Plain (non-aligned) forms. The nothrow forms of the standard library call these. Over-aligned
// allocations keep the library defaults; the code under test has no over-aligned types.
void* operator new (std::size_t n) { return allocate (n); }
void* operator new[] (std::size_t n) { return allocate (n); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept
{
    try { return allocate (n); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept
{
    try { return allocate (n); } catch (...) { return nullptr; }
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }
