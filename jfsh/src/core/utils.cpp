#include <jfsh/core/stack.h>

#include <pthread.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

namespace {

// Room kept free below the deepest checked frame: enough for the frames
// between two checks (and a signal handler), but at most a quarter of a
// small stack, so a `ulimit -s 512` shell still expands `$(…)`.
constexpr s::uintptr_t STACK_MARGIN = 512 * 1024;

struct StackBounds {
  s::uintptr_t floor = 0;   // lowest usable address; 0 when unknown
  s::uintptr_t size  = 0;
};

StackBounds stack_bounds() noexcept {
#if defined(__APPLE__)
  pthread_t self = pthread_self();
  auto top = reinterpret_cast<s::uintptr_t>(pthread_get_stackaddr_np(self));
  s::uintptr_t size = pthread_get_stacksize_np(self);
  return {top - size, size};
#elif defined(__linux__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) return {};
  void* low = nullptr;
  s::size_t size = 0;
  int rc = pthread_attr_getstack(&attr, &low, &size);
  pthread_attr_destroy(&attr);
  if (rc != 0) return {};
  return {reinterpret_cast<s::uintptr_t>(low), size};
#else
  return {};
#endif
}

}  // namespace

bool stack_low() noexcept {
  thread_local StackBounds bounds = stack_bounds();
  thread_local s::uintptr_t margin = s::min(STACK_MARGIN, bounds.size / 4);
  if (bounds.floor == 0) return false;
  auto at = reinterpret_cast<s::uintptr_t>(__builtin_frame_address(0));
  return at < bounds.floor + margin;
}

}  // namespace jfsh
