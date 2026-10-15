#pragma once
// GTA IV Enhanced - always-on lightweight CPU zone profiler for the Remix runtime.
// Every ScopedCpuProfileZone / ScopedGpuProfileZone site accumulates call count and inclusive time.
// A background thread writes the top zones (ms per frame) to rtx-remix/gta4e_profile.txt every 2 s,
// together with the gta4e counters (e.g. BLAS bucket statistics). No Tracy GUI needed.
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

  struct ZoneSite {
    const char* name;
    std::atomic<uint64_t> ticks { 0 };
    std::atomic<uint64_t> calls { 0 };
    uint64_t lastTicks = 0, lastCalls = 0;
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
    static Registry& get() { static Registry r; return r; }
  };

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
    uint64_t start;
    explicit ZoneTimer(ZoneSite& s) : site(s), start(__rdtsc()) {}
    ~ZoneTimer() {
      site.ticks.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
      site.calls.fetch_add(1, std::memory_order_relaxed);
    }
  };

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
