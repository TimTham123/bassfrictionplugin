// Friction - counts heap allocations made by one thread (real-time safety tests)
#pragma once

namespace ft
{

/**
    The test executables replace the global operator new / delete (AllocCounter.cpp). While a thread
    is inside begin() ... end(), every operator new call made by that thread is counted. Other
    threads (JUCE timers, the Advisor worker) are not counted.
*/
struct AllocCounter
{
    static void begin() noexcept;
    /** Stops counting on this thread; returns the number of allocations since begin(). */
    static long long end() noexcept;
};

} // namespace ft
