#pragma once
// GTA IV Enhanced - always-on lightweight CPU zone profiler for the Remix runtime.
// Every ScopedCpuProfileZone / ScopedGpuProfileZone site accumulates call count and inclusive time.
// A background thread writes the top zones (ms per frame) to rtx-remix/gta4e_profile.txt every 2 s,
// together with the gta4e counters (e.g. BLAS bucket statistics). No Tracy GUI needed.
// Each site also keeps exclusive time (minus nested zones) per thread, so the file shows which
// thread does the work (e.g. the dxvk CS thread) and what fills it ("T"/"E" lines at the end).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <windows.h>

namespace gta4e {

  // threads beyond this share the last slot ("other")
  constexpr uint32_t kMaxThreads = 24;

  struct ZoneSite {
    const char* name;
    std::atomic<uint64_t> ticks { 0 };
    std::atomic<uint64_t> calls { 0 };
    uint64_t lastTicks = 0, lastCalls = 0;
    std::atomic<uint64_t> exclTicks[kMaxThreads] {};
    std::atomic<uint64_t> exclCalls[kMaxThreads] {};
    uint64_t lastExclTicks[kMaxThreads] {}, lastExclCalls[kMaxThreads] {};
    explicit ZoneSite(const char* n);
  };

  struct Counter {
    const char* name;
    std::atomic<int64_t> value { 0 };
    int64_t last = 0;
    explicit Counter(const char* n);
  };

  struct Registry {
    std::mutex mtx;
    std::vector<ZoneSite*> sites;
    std::vector<Counter*> counters;
    std::atomic<bool> started { false };
    std::atomic<uint32_t> nextThread { 0 };
    std::atomic<DWORD> threadIds[kMaxThreads] {};
    static Registry& get() { static Registry r; return r; }
  };

  // per-thread nesting state for exclusive time: each open zone collects its children's inclusive time
  struct ThreadState {
    uint32_t slot = UINT32_MAX;
    uint32_t depth = 0;
    uint64_t childTicks[64] {};
  };

  inline ThreadState& threadState() {
    thread_local ThreadState t;
    if (t.slot == UINT32_MAX) {
      auto& r = Registry::get();
      t.slot = (std::min)(r.nextThread.fetch_add(1, std::memory_order_relaxed), kMaxThreads - 1);
      if (t.slot < kMaxThreads - 1) r.threadIds[t.slot].store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    return t;
  }

  inline void writerThread();

  inline void ensureStarted() {
    auto& r = Registry::get();
    if (r.started.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (r.started.compare_exchange_strong(expected, true)) {
      std::thread(writerThread).detach();
    }
  }

  inline ZoneSite::ZoneSite(const char* n) : name(n) {
    auto& r = Registry::get();
    std::lock_guard<std::mutex> lock(r.mtx);
    r.sites.push_back(this);
  }

  inline Counter::Counter(const char* n) : name(n) {
    auto& r = Registry::get();
    std::lock_guard<std::mutex> lock(r.mtx);
    r.counters.push_back(this);
  }

  struct ZoneTimer {
    ZoneSite& site;
    ThreadState& ts;
    uint64_t start;
    explicit ZoneTimer(ZoneSite& s) : site(s), ts(threadState()), start(__rdtsc()) {
      if (ts.depth < 64) ts.childTicks[ts.depth] = 0;
      ++ts.depth;
    }
    ~ZoneTimer() {
      const uint64_t elapsed = __rdtsc() - start;
      site.ticks.fetch_add(elapsed, std::memory_order_relaxed);
      site.calls.fetch_add(1, std::memory_order_relaxed);
      const uint32_t d = --ts.depth;
      const uint64_t child = d < 64 ? ts.childTicks[d] : 0;
      if (d > 0 && d - 1 < 64) ts.childTicks[d - 1] += elapsed;
      site.exclTicks[ts.slot].fetch_add(elapsed > child ? elapsed - child : 0, std::memory_order_relaxed);
      site.exclCalls[ts.slot].fetch_add(1, std::memory_order_relaxed);
    }
  };

  // thread name set by the runtime (dxvk names its threads, e.g. the CS thread), "" if unavailable
  inline std::string threadName(DWORD tid) {
    using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const Fn getDesc = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription"));
    std::string out;
    if (!getDesc || !tid) return out;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return out;  // thread has exited
    PWSTR w = nullptr;
    if (SUCCEEDED(getDesc(h, &w)) && w) {
      for (const wchar_t* p = w; *p && out.size() < 40; ++p) out.push_back(*p < 128 && *p > ' ' ? char(*p) : '_');
      LocalFree(w);
    }
    CloseHandle(h);
    return out;
  }

  inline double ticksPerMs() {
    static double tpm = [] {
      LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&a); const uint64_t t0 = __rdtsc();
      Sleep(50);
      QueryPerformanceCounter(&b); const uint64_t t1 = __rdtsc();
      const double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
      return double(t1 - t0) / ms;
    }();
    return tpm;
  }

  inline void writerThread() {
    const double tpm = ticksPerMs();
    ZoneSite* frameSite = nullptr;
    for (;;) {
      Sleep(2000);
      auto& r = Registry::get();
      std::vector<ZoneSite*> sites; std::vector<Counter*> counters;
      { std::lock_guard<std::mutex> lock(r.mtx); sites = r.sites; counters = r.counters; }

      struct Row { const char* name; double ms; uint64_t calls; };
      std::vector<Row> rows;
      double frames = 0.0;
      for (auto* s : sites) {
        const uint64_t t = s->ticks.load(), c = s->calls.load();
        const uint64_t dt = t - s->lastTicks, dc = c - s->lastCalls;
        s->lastTicks = t; s->lastCalls = c;
        if (!frameSite && std::strstr(s->name, "D3D9SwapChainEx::Present") && !std::strstr(s->name, "PresentImage")) frameSite = s;
        if (s == frameSite) frames = double(dc);
        if (dc) rows.push_back({ s->name, double(dt) / tpm, dc });
      }
      if (frames < 1.0) frames = 1.0;
      std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });

      FILE* f = nullptr;
      if (fopen_s(&f, "rtx-remix\\gta4e_profile.tmp", "w") != 0 || !f) continue;
      fprintf(f, "frames_in_window %.0f\n", frames);
      fprintf(f, "# counters (per frame)\n");
      for (auto* c : counters) {
        const int64_t v = c->value.load(); const int64_t d = v - c->last; c->last = v;
        fprintf(f, "C %-48s %12.1f\n", c->name, double(d) / frames);
      }
      fprintf(f, "# zones: inclusive ms per frame, calls per frame\n");
      for (size_t i = 0; i < rows.size() && i < 120; ++i)
        fprintf(f, "Z %-60s %9.3f %10.1f\n", rows[i].name, rows[i].ms / frames, double(rows[i].calls) / frames);

      // per thread: exclusive zone time (ms per frame), busiest threads first, top zones of each
      struct Excl { const char* name; double ms; uint64_t calls; };
      std::vector<Excl> perThread[kMaxThreads];
      double threadMs[kMaxThreads] {};
      for (auto* s : sites) {
        for (uint32_t t = 0; t < kMaxThreads; ++t) {
          const uint64_t et = s->exclTicks[t].load(), ec = s->exclCalls[t].load();
          const uint64_t dt = et - s->lastExclTicks[t], dc = ec - s->lastExclCalls[t];
          s->lastExclTicks[t] = et; s->lastExclCalls[t] = ec;
          if (!dc) continue;
          const double ms = double(dt) / tpm / frames;
          perThread[t].push_back({ s->name, ms, dc });
          threadMs[t] += ms;
        }
      }
      uint32_t order[kMaxThreads];
      for (uint32_t t = 0; t < kMaxThreads; ++t) order[t] = t;
      std::sort(order, order + kMaxThreads, [&](uint32_t a, uint32_t b) { return threadMs[a] > threadMs[b]; });
      fprintf(f, "# threads: exclusive zone ms per frame (T slot tid name total), then that thread's top zones\n");
      for (uint32_t i = 0; i < kMaxThreads; ++i) {
        const uint32_t t = order[i];
        if (threadMs[t] < 0.05) break;
        const DWORD tid = t < kMaxThreads - 1 ? r.threadIds[t].load() : 0;
        static std::string names[kMaxThreads];  // cached: the name stays readable after the thread exits
        if (names[t].empty()) names[t] = t < kMaxThreads - 1 ? threadName(tid) : std::string("other");
        const std::string& tn = names[t];
        fprintf(f, "T %2u %6lu %-24s %9.3f\n", t, static_cast<unsigned long>(tid), tn.empty() ? "?" : tn.c_str(), threadMs[t]);
        auto& v = perThread[t];
        std::sort(v.begin(), v.end(), [](const Excl& a, const Excl& b) { return a.ms > b.ms; });
        for (size_t k = 0; k < v.size() && k < 15; ++k)
          fprintf(f, "E %-60s %9.3f %10.1f\n", v[k].name, v[k].ms, double(v[k].calls) / frames);
      }
      fclose(f);
      MoveFileExA("rtx-remix\\gta4e_profile.tmp", "rtx-remix\\gta4e_profile.txt", MOVEFILE_REPLACE_EXISTING);
    }
  }
}

#define GTA4E_CONCAT2(a, b) a##b
#define GTA4E_CONCAT(a, b) GTA4E_CONCAT2(a, b)

#define GTA4E_ZONE(nameLiteral) \
  static ::gta4e::ZoneSite GTA4E_CONCAT(__gta4e_site_, __LINE__)(nameLiteral); \
  ::gta4e::ensureStarted(); \
  ::gta4e::ZoneTimer GTA4E_CONCAT(__gta4e_timer_, __LINE__)(GTA4E_CONCAT(__gta4e_site_, __LINE__))

// usage: GTA4E_COUNT("buckets.dirty", n);
#define GTA4E_COUNT(nameLiteral, val) \
  do { static ::gta4e::Counter __gta4e_ctr(nameLiteral); __gta4e_ctr.value.fetch_add(int64_t(val), std::memory_order_relaxed); } while (0)
