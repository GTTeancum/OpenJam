// NBA JAM: On Fire Edition - guest sampling profiler
//
// Recompiled guest functions are ordinary C++ functions named sub_<guest
// address>, so a plain instruction-pointer sampler resolved against our own
// PDB gives a profile in terms of XEX addresses. That is how you find a
// particular piece of guest code - a video decoder, an audio mixer - in an
// image with no symbols and no disassembler on hand.
//
// Off unless NBAJAM_PROFILE_SECONDS is set, so it costs a normal run nothing:
//
//   NBAJAM_PROFILE_SECONDS=6            sample for six seconds, then dump
//   NBAJAM_PROFILE_DELAY=2              start sampling two seconds in
//   NBAJAM_PROFILE_HZ=1000              samples per second (default 1000)
//   NBAJAM_PROFILE_OUT=<path>           default guest_profile.txt beside exe
//
// Each tick suspends every other thread in turn, reads RIP, and resumes it
// immediately. Nothing is allocated while a thread is suspended: a sample is
// just a raw address pushed into a preallocated buffer, and symbols are
// resolved once at the end. Suspending a thread that holds the CRT heap lock
// and then allocating on this thread would deadlock the process.

#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace {

// Refresh the thread list this often. Guest threads are created during boot,
// so a list taken once at startup would miss most of them.
constexpr int kRescanEveryTicks = 200;

int EnvInt(const char* name, int fallback) {
  char buf[64]{};
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) {
    return fallback;
  }
  int v = atoi(buf);
  return v > 0 ? v : fallback;
}

std::string OutputPath() {
  char buf[MAX_PATH]{};
  if (GetEnvironmentVariableA("NBAJAM_PROFILE_OUT", buf, MAX_PATH) > 0) {
    return buf;
  }
  char path[MAX_PATH]{};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string p(path);
  size_t slash = p.find_last_of("\\/");
  return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) +
         "guest_profile.txt";
}

std::vector<DWORD> ListThreads(DWORD self) {
  std::vector<DWORD> out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return out;
  }
  const DWORD pid = GetCurrentProcessId();
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) {
        out.push_back(te.th32ThreadID);
      }
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  return out;
}

void Dump(const std::vector<uint64_t>& samples) {
  FILE* f = nullptr;
  const std::string path = OutputPath();
  if (fopen_s(&f, path.c_str(), "w") != 0 || !f) {
    return;
  }

  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);

  // Resolve each distinct address once; many samples land in the same
  // function and SymFromAddr is far too slow to call per sample.
  std::unordered_map<uint64_t, std::string> addr_name;
  std::unordered_map<std::string, uint64_t> by_func;
  alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 512]{};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 511;

  for (uint64_t addr : samples) {
    auto it = addr_name.find(addr);
    if (it == addr_name.end()) {
      std::string name;
      DWORD64 disp = 0;
      if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
        name = sym->Name;
      } else {
        // Outside our module: the runtime DLL, a system DLL, or JIT-free
        // host code. Bucket by module so the report still accounts for it.
        HMODULE mod = nullptr;
        char modname[MAX_PATH]{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) &&
            GetModuleFileNameA(mod, modname, MAX_PATH)) {
          std::string m(modname);
          size_t slash = m.find_last_of("\\/");
          name = "[" + (slash == std::string::npos ? m : m.substr(slash + 1)) + "]";
        } else {
          name = "[unknown]";
        }
      }
      it = addr_name.emplace(addr, std::move(name)).first;
    }
    by_func[it->second]++;
  }

  std::vector<std::pair<std::string, uint64_t>> ranked(by_func.begin(), by_func.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    return a.second > b.second;
  });

  fprintf(f, "guest sampling profile\n");
  fprintf(f, "%llu samples, %zu distinct addresses, %zu distinct functions\n\n",
          static_cast<unsigned long long>(samples.size()), addr_name.size(), ranked.size());
  fprintf(f, "%8s  %6s  %s\n", "samples", "share", "function");
  for (const auto& [name, count] : ranked) {
    double pct = samples.empty() ? 0.0 : 100.0 * double(count) / double(samples.size());
    if (pct < 0.02) {
      break;
    }
    fprintf(f, "%8llu  %5.2f%%  %s\n", static_cast<unsigned long long>(count), pct, name.c_str());
  }
  fclose(f);
}

void SamplerThread() {
  const int seconds = EnvInt("NBAJAM_PROFILE_SECONDS", 0);
  if (seconds <= 0) {
    return;
  }
  const int delay = EnvInt("NBAJAM_PROFILE_DELAY", 0);
  const int hz = EnvInt("NBAJAM_PROFILE_HZ", 1000);
  const DWORD self = GetCurrentThreadId();

  if (delay > 0) {
    Sleep(DWORD(delay) * 1000);
  }

  std::vector<uint64_t> samples;
  samples.reserve(size_t(seconds) * size_t(hz) * 8);

  const int ticks = seconds * hz;
  const DWORD period_ms = DWORD(std::max(1, 1000 / std::max(1, hz)));
  std::vector<DWORD> tids;
  std::vector<uint64_t> tick_samples;

  for (int tick = 0; tick < ticks; ++tick) {
    if (tick % kRescanEveryTicks == 0) {
      tids = ListThreads(self);
    }
    tick_samples.clear();
    for (DWORD tid : tids) {
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
      if (!h) {
        continue;
      }
      if (SuspendThread(h) != DWORD(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        // Read and resume before touching anything that can allocate or take
        // a lock; the suspended thread may hold the heap lock.
        const bool ok = GetThreadContext(h, &c) != 0;
        ResumeThread(h);
        if (ok && c.Rip) {
          tick_samples.push_back(c.Rip);
        }
      }
      CloseHandle(h);
    }
    samples.insert(samples.end(), tick_samples.begin(), tick_samples.end());
    Sleep(period_ms);
  }

  Dump(samples);
}

struct Starter {
  Starter() {
    if (EnvInt("NBAJAM_PROFILE_SECONDS", 0) > 0) {
      std::thread(SamplerThread).detach();
    }
  }
};

Starter g_starter;

}  // namespace
