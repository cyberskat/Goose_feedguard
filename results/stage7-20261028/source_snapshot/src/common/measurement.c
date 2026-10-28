/* Shared measurement utilities only: no reader, order-book, checksum or JSON. */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <psapi.h>
#include "../../include/measurement.h"

uint64_t fg_clock_frequency(void) {
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 0;
    return (uint64_t)frequency.QuadPart;
}
uint64_t fg_clock_ns(void) {
    /* Benchmarks are single-threaded; initialize once before timed loops. */
    static uint64_t frequency = 0;
    LARGE_INTEGER counter;
    if (!frequency) frequency = fg_clock_frequency();
    if (!frequency || frequency > UINT64_MAX / 1000000000ull ||
        !QueryPerformanceCounter(&counter) || counter.QuadPart < 0) return 0;
    const uint64_t ticks = (uint64_t)counter.QuadPart;
    if (ticks / frequency > UINT64_MAX / 1000000000ull) return 0;
    const uint64_t whole = (ticks / frequency) * 1000000000ull;
    const uint64_t fraction = ((ticks % frequency) * 1000000000ull) / frequency;
    if (whole > UINT64_MAX - fraction) return 0;
    return whole + fraction;
}
uint64_t fg_peak_working_set_bytes(void) {
    PROCESS_MEMORY_COUNTERS counters;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0;
    return (uint64_t)counters.PeakWorkingSetSize;
}
