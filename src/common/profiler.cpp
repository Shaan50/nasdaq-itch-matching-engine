#include "ironbook/common/profiler.hpp"

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#if defined(__linux__)
#include <ucontext.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <link.h>
#endif

namespace ib {

namespace {

struct Sample {
  std::uint32_t depth;
  std::uintptr_t pc[SamplingProfiler::kMaxDepth];
};

struct State {
  std::unique_ptr<Sample[]> samples;
  std::size_t capacity = 0;
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> dropped{0};
  std::string path;
  struct sigaction old_action {};
  bool running = false;
};

State g_state;

bool in_stack(std::uintptr_t p, std::uintptr_t lo, std::uintptr_t hi) noexcept {
  return p >= lo && p + 2 * sizeof(void*) <= hi && (p % sizeof(void*)) == 0;
}

void on_sigprof(int, siginfo_t*, void* ctx_v) {
  const std::size_t i = g_state.next.fetch_add(1, std::memory_order_relaxed);
  if (i >= g_state.capacity) {
    g_state.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Sample& s = g_state.samples[i];
  auto* ctx = static_cast<ucontext_t*>(ctx_v);
  std::uintptr_t pc = 0, fp = 0, lr = 0;
#if defined(__APPLE__) && defined(__aarch64__)
  pc = ctx->uc_mcontext->__ss.__pc;
  fp = ctx->uc_mcontext->__ss.__fp;
  lr = ctx->uc_mcontext->__ss.__lr;
#elif defined(__APPLE__) && defined(__x86_64__)
  pc = ctx->uc_mcontext->__ss.__rip;
  fp = ctx->uc_mcontext->__ss.__rbp;
#elif defined(__linux__) && defined(__aarch64__)
  pc = ctx->uc_mcontext.pc;
  fp = ctx->uc_mcontext.regs[29];
  lr = ctx->uc_mcontext.regs[30];
#elif defined(__linux__) && defined(__x86_64__)
  pc = static_cast<std::uintptr_t>(ctx->uc_mcontext.gregs[REG_RIP]);
  fp = static_cast<std::uintptr_t>(ctx->uc_mcontext.gregs[REG_RBP]);
#endif
  std::uint32_t d = 0;
  s.pc[d++] = pc;
  // Leaf functions on arm64 may not have pushed a frame yet; the link register then
  // holds the caller. It can duplicate the first frame-pointer entry, which the
  // symbolizer collapses.
  if (lr) s.pc[d++] = lr;

  std::uintptr_t lo = 0, hi = 0;
#if defined(__APPLE__)
  pthread_t self = pthread_self();
  hi = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
  lo = hi - pthread_get_stacksize_np(self);
#else
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* addr;
    std::size_t size;
    pthread_attr_getstack(&attr, &addr, &size);
    lo = reinterpret_cast<std::uintptr_t>(addr);
    hi = lo + size;
    pthread_attr_destroy(&attr);
  }
#endif
  while (d < SamplingProfiler::kMaxDepth && in_stack(fp, lo, hi)) {
    const auto* frame = reinterpret_cast<const std::uintptr_t*>(fp);
    const std::uintptr_t next_fp = frame[0];
    const std::uintptr_t ret = frame[1];
    if (ret == 0) break;
    s.pc[d++] = ret;
    if (next_fp <= fp) break;  // stacks grow down; the chain must move up
    fp = next_fp;
  }
  s.depth = d;
}

std::uintptr_t main_image_slide_base() {
#if defined(__APPLE__)
  // Load address of the main executable's Mach-O header (what `atos -l` expects).
  return reinterpret_cast<std::uintptr_t>(_dyld_get_image_header(0));
#elif defined(__linux__)
  std::uintptr_t base = 0;
  dl_iterate_phdr(
      [](dl_phdr_info* info, std::size_t, void* out) {
        *static_cast<std::uintptr_t*>(out) = info->dlpi_addr;
        return 1;  // first entry is the main program
      },
      &base);
  return base;
#else
  return 0;
#endif
}

}  // namespace

bool SamplingProfiler::start(const std::string& out_path, int hz, std::size_t max_samples) {
  if (g_state.running) return false;
  g_state.samples = std::make_unique<Sample[]>(max_samples);
  g_state.capacity = max_samples;
  g_state.next = 0;
  g_state.dropped = 0;
  g_state.path = out_path;

  struct sigaction sa {};
  sa.sa_sigaction = on_sigprof;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, &g_state.old_action) != 0) return false;

  itimerval tv{};
  tv.it_interval.tv_sec = 0;
  tv.it_interval.tv_usec = 1'000'000 / (hz > 0 ? hz : 1999);
  tv.it_value = tv.it_interval;
  if (setitimer(ITIMER_PROF, &tv, nullptr) != 0) return false;
  g_state.running = true;
  return true;
}

std::size_t SamplingProfiler::stop() {
  if (!g_state.running) return 0;
  itimerval off{};
  setitimer(ITIMER_PROF, &off, nullptr);
  sigaction(SIGPROF, &g_state.old_action, nullptr);
  g_state.running = false;

  const std::size_t n = std::min(g_state.next.load(), g_state.capacity);
  FILE* f = std::fopen(g_state.path.c_str(), "w");
  if (!f) return 0;
  // Format: header line, then one sample per line as hex addresses (leaf first).
  std::fprintf(f, "# ironbook-profile v1 load=0x%lx samples=%zu dropped=%zu\n",
               static_cast<unsigned long>(main_image_slide_base()), n, g_state.dropped.load());
  for (std::size_t i = 0; i < n; ++i) {
    const Sample& s = g_state.samples[i];
    for (std::uint32_t d = 0; d < s.depth; ++d)
      std::fprintf(f, d ? " %lx" : "%lx", static_cast<unsigned long>(s.pc[d]));
    std::fputc('\n', f);
  }
  std::fclose(f);
  std::fprintf(stderr, "[profiler] wrote %zu samples to %s\n", n, g_state.path.c_str());
  return n;
}

bool SamplingProfiler::start_from_env() {
  const char* path = std::getenv("IRONBOOK_PROFILE");
  if (!path || !*path) return false;
  const char* hz = std::getenv("IRONBOOK_PROFILE_HZ");
  return start(path, hz ? std::atoi(hz) : 1999);
}

}  // namespace ib
