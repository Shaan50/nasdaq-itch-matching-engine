#pragma once

#include <pthread.h>

#if defined(__linux__)
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace ib {

// Pin the calling thread to `cpu` (Linux). macOS has no hard affinity API on Apple
// silicon, so there we request the USER_INTERACTIVE QoS class, which keeps the thread
// on performance cores. Returns true if a placement request was applied.
inline bool pin_current_thread(int cpu) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#elif defined(__APPLE__)
  (void)cpu;
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
  (void)cpu;
  return false;
#endif
}

inline void set_thread_name(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

}  // namespace ib
