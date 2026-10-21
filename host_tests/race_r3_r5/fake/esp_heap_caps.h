#pragma once
// HOST STUB (not production): the ESP-IDF heap-capability queries used by
// race.cpp's heap diagnostics. Values are set by the test (defined in
// stubs.cpp); g_fakeHeapCapsSeen collects every capability mask queried.
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
extern size_t g_fakeHeapFree, g_fakeHeapMinFree, g_fakeHeapLargest;
extern uint32_t g_fakeHeapCapsSeen;
extern int g_fakeHeapQueries;
inline size_t heap_caps_get_free_size(uint32_t caps) { g_fakeHeapCapsSeen |= caps; g_fakeHeapQueries++; return g_fakeHeapFree; }
inline size_t heap_caps_get_minimum_free_size(uint32_t caps) { g_fakeHeapCapsSeen |= caps; g_fakeHeapQueries++; return g_fakeHeapMinFree; }
inline size_t heap_caps_get_largest_free_block(uint32_t caps) { g_fakeHeapCapsSeen |= caps; g_fakeHeapQueries++; return g_fakeHeapLargest; }
