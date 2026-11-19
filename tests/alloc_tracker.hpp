// Global allocation tracker for tests: counts allocations/frees made while tl_audio is true on the
// current thread. Include from exactly one source file per test executable.
#pragma once

#include <atomic>
#include <cstdlib>
#include <new>

// ---- allocation tracking: counts allocations made while tl_audio is set on the current thread ----
namespace {
thread_local bool tl_audio = false;
std::atomic<long> g_audioAllocs{0};
}  // namespace
namespace {
void* trackedAlloc(std::size_t n) {
  if (tl_audio) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void trackedFree(void* p) noexcept {
  if (tl_audio && p) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);  // frees count too
  std::free(p);
}
}  // namespace
void* operator new(std::size_t n) { return trackedAlloc(n); }
void* operator new[](std::size_t n) { return trackedAlloc(n); }
void operator delete(void* p) noexcept { trackedFree(p); }
void operator delete[](void* p) noexcept { trackedFree(p); }
void operator delete(void* p, std::size_t) noexcept { trackedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { trackedFree(p); }
