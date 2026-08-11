// A minimal parallel-for, for the whole-image passes in the mask pipeline.
//
// std::thread rather than OFX::MultiThread because src/core must not include an
// OFX header -- that constraint is what lets the offline harness link the exact
// code Resolve runs, and it would be a poor trade to give it up for a thread
// pool. The plugin still threads the per-pixel render through the host; this
// covers the stages that run before it.
#pragma once

#include <algorithm>
#include <thread>
#include <vector>

namespace am {

/// Calls f(begin, end) on disjoint sub-ranges of [0,n).
///
/// Threads are spawned per call rather than pooled. At the sizes this is used
/// for -- a full-frame pass, milliseconds of work -- spawn cost is a rounding
/// error, and a pool would need a lifetime, a shutdown, and a reason to trust
/// it across a host that may already be running us on its own worker threads.
template <class F>
void ParallelFor(int n, F f, int minPerThread = 4096) {
  if (n <= 0) return;

  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 4;
  int threads = int(std::min<unsigned>(hw, unsigned(std::max(1, n / minPerThread))));
  if (threads <= 1) {
    f(0, n);
    return;
  }

  std::vector<std::thread> pool;
  pool.reserve(std::size_t(threads - 1));
  for (int t = 1; t < threads; ++t) {
    const int b = int(std::size_t(t) * n / threads);
    const int e = int(std::size_t(t + 1) * n / threads);
    if (b < e) pool.emplace_back([&f, b, e] { f(b, e); });
  }
  f(0, int(std::size_t(1) * n / threads));  // this thread takes the first band
  for (std::thread& th : pool) th.join();
}

}  // namespace am
