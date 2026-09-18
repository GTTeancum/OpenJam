// NBA JAM: On Fire Edition - guest fault diagnostics
//
// The runtime reports an unhandled guest access violation as a single line:
//
//   Unhandled guest access violation: read of guest 0x68EC0000 ... on thread ...
//
// which says what was touched but not what touched it. There is no cdb or
// windbg on this machine and the LLVM lldb build here is missing python311.dll,
// so this installs a vectored exception handler that resolves the native call
// stack against our own PDB instead. Recompiled guest functions are ordinary
// C++ functions named sub_<guest address>, so the resulting stack reads as a
// guest call stack and maps straight back to addresses in the XEX.
//
// Deliberately limited:
//   - Only EXCEPTION_ACCESS_VIOLATION is reported.
//   - Each distinct faulting address is reported once. The runtime takes
//     legitimate access violations as part of physical-memory callbacks, so
//     logging every one would bury the interesting fault in noise.
//   - The handler returns EXCEPTION_CONTINUE_SEARCH, so it observes and never
//     changes behaviour. Removing this file cannot fix or break anything.
//
// Output: crash_stack.txt beside the executable.
//
// Build with REXPORT_CRASH_REPORT=OFF to leave it out.

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::mutex g_mutex;
std::set<uintptr_t> g_seen;
std::atomic<bool> g_symbols_ready{false};
std::atomic<int> g_reports{0};

constexpr int kMaxReports = 12;
constexpr int kMaxFrames = 48;

// The runtime maps the guest arena at a fixed host base; the log line prints
// both, e.g. "guest 0x68EC0000 (host 0x0000000168EC0000)".
constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

FILE* OpenReport() {
  char path[MAX_PATH]{};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string p(path);
  size_t slash = p.find_last_of("\\/");
  p = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "crash_stack.txt";
  FILE* f = nullptr;
  fopen_s(&f, p.c_str(), "a");
  return f;
}

void EnsureSymbols() {
  bool expected = false;
  if (!g_symbols_ready.compare_exchange_strong(expected, true)) {
    return;
  }
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

void DescribeFrame(FILE* f, int index, void* addr) {
  HANDLE proc = GetCurrentProcess();

  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = MAX_SYM_NAME;

  DWORD64 disp = 0;
  if (SymFromAddr(proc, reinterpret_cast<DWORD64>(addr), &disp, sym)) {
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_disp = 0;
    if (SymGetLineFromAddr64(proc, reinterpret_cast<DWORD64>(addr), &line_disp, &line)) {
      const char* file = line.FileName ? line.FileName : "?";
      size_t slash = std::string(file).find_last_of("\\/");
      if (slash != std::string::npos) {
        file += slash + 1;
      }
      fprintf(f, "  [%2d] %s + 0x%llX   (%s:%lu)\n", index, sym->Name,
              static_cast<unsigned long long>(disp), file, line.LineNumber);
    } else {
      fprintf(f, "  [%2d] %s + 0x%llX\n", index, sym->Name,
              static_cast<unsigned long long>(disp));
    }
  } else {
    fprintf(f, "  [%2d] 0x%p  <no symbol>\n", index, addr);
  }
}

// Guest memory is a flat arena at a fixed host base, so a guest address is just
// an offset into it. Reads are guarded: the whole point is that some of this
// memory is not mapped.
bool ReadGuest(uint32_t guest, void* out, size_t len) {
  const void* src = reinterpret_cast<const void*>(kGuestVirtualBase + guest);
  __try {
    memcpy(out, src, len);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

uint32_t Bswap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

void DumpGuestWord(FILE* f, uint32_t guest, const char* what) {
  uint32_t raw = 0;
  if (!ReadGuest(guest, &raw, sizeof(raw))) {
    fprintf(f, "guest [0x%08X] = <unmapped>   (%s)\n", guest, what);
    return;
  }
  // The guest is big-endian; the arena holds its bytes verbatim.
  fprintf(f, "guest [0x%08X] = 0x%08X   (%s)\n", guest, Bswap32(raw), what);
}

void DumpGuestWindow(FILE* f, uint32_t guest, uint32_t len, const char* what) {
  fprintf(f, "\n%s  (guest 0x%08X..0x%08X, big-endian words)\n", what, guest, guest + len);
  for (uint32_t off = 0; off < len; off += 16) {
    uint32_t words[4]{};
    if (!ReadGuest(guest + off, words, sizeof(words))) {
      fprintf(f, "  0x%08X: <unmapped>\n", guest + off);
      continue;
    }
    fprintf(f, "  0x%08X: %08X %08X %08X %08X\n", guest + off, Bswap32(words[0]),
            Bswap32(words[1]), Bswap32(words[2]), Bswap32(words[3]));
  }
}

void DumpGuestString(FILE* f, uint32_t guest, const char* what) {
  char s[512]{};
  size_t n = 0;
  for (; n < sizeof(s) - 1; ++n) {
    char ch = 0;
    if (!ReadGuest(guest + static_cast<uint32_t>(n), &ch, 1) || ch == '\0') {
      break;
    }
    s[n] = ch;
  }
  s[n] = '\0';
  fprintf(f, "guest string [0x%08X] = \"%s\"   (%s)\n", guest, s, what);
}

// Is the faulting page reserved-but-uncommitted, never allocated, or committed
// with the wrong protection? These mean very different things.
void DumpPageState(FILE* f, uintptr_t host_addr) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(host_addr), &mbi, sizeof(mbi))) {
    fprintf(f, "\nVirtualQuery failed for 0x%016llX\n",
            static_cast<unsigned long long>(host_addr));
    return;
  }
  const char* state = mbi.State == MEM_COMMIT  ? "COMMIT"
                      : mbi.State == MEM_RESERVE ? "RESERVE"
                      : mbi.State == MEM_FREE    ? "FREE"
                                                 : "?";
  fprintf(f,
          "\nfaulting page: state=%s protect=0x%lX base=0x%016llX size=0x%llX\n"
          "  (RESERVE means the arena is mapped but this page was never committed;\n"
          "   FREE means nothing ever claimed it)\n",
          state, mbi.Protect, static_cast<unsigned long long>(
              reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
          static_cast<unsigned long long>(mbi.RegionSize));
}

// Walk the committed guest regions and look for a marker from memory.cfg. If
// the file's text is nowhere in guest memory, the game never read it.
void ScanGuestFor(FILE* f, const char* needle) {
  const size_t len = strlen(needle);
  const uintptr_t lo = kGuestVirtualBase;
  const uintptr_t hi = kGuestVirtualBase + 0x100000000ull;

  fprintf(f, "\ncommitted guest regions, and search for \"%s\":\n", needle);
  int regions = 0, hits = 0;
  uintptr_t addr = lo;
  while (addr < hi && regions < 4096) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
      break;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const size_t size = mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) &&
        !(mbi.Protect & PAGE_GUARD)) {
      ++regions;
      if (regions <= 24) {
        fprintf(f, "  guest 0x%08X .. 0x%08X  protect=0x%lX\n",
                static_cast<unsigned>(base - lo),
                static_cast<unsigned>(base + size - lo), mbi.Protect);
      }
      const char* p = reinterpret_cast<const char*>(base);
      const size_t limit = size > len ? size - len : 0;
      __try {
        for (size_t i = 0; i < limit; ++i) {
          if (p[i] == needle[0] && memcmp(p + i, needle, len) == 0) {
            if (hits < 6) {
              fprintf(f, "  HIT \"%s\" at guest 0x%08X\n", needle,
                      static_cast<unsigned>(base + i - lo));
            }
            ++hits;
            i += len;
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    if (size == 0) {
      break;
    }
    addr = base + size;
  }
  fprintf(f, "  committed regions: %d (first 24 listed), hits: %d\n", regions, hits);
}

void DumpHostRegisters(FILE* f, const CONTEXT* c) {
  if (!c) {
    return;
  }
  fprintf(f, "\nhost registers at fault:\n");
  const struct {
    const char* name;
    DWORD64 value;
  } regs[] = {
      {"rax", c->Rax}, {"rbx", c->Rbx}, {"rcx", c->Rcx}, {"rdx", c->Rdx},
      {"rsi", c->Rsi}, {"rdi", c->Rdi}, {"rbp", c->Rbp}, {"rsp", c->Rsp},
      {"r8 ", c->R8},  {"r9 ", c->R9},  {"r10", c->R10}, {"r11", c->R11},
      {"r12", c->R12}, {"r13", c->R13}, {"r14", c->R14}, {"r15", c->R15},
  };
  for (const auto& r : regs) {
    fprintf(f, "  %s = 0x%016llX", r.name, static_cast<unsigned long long>(r.value));
    // Flag anything that looks like a pointer into the guest arena: one of
    // these is the recompiled function's `base`, and PPCContext sits near it.
    if (r.value >= kGuestVirtualBase && r.value < kGuestVirtualBase + 0x100000000ull) {
      fprintf(f, "   -> guest 0x%08X",
              static_cast<unsigned>(r.value - kGuestVirtualBase));
    }
    fprintf(f, "\n");
  }
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (rec->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const uintptr_t fault = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
  const bool is_write = rec->ExceptionInformation[0] != 0;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_reports.load() >= kMaxReports) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!g_seen.insert(fault).second) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    g_reports.fetch_add(1);

    FILE* f = OpenReport();
    if (!f) {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    EnsureSymbols();

    fprintf(f, "\n=== access violation: %s of host 0x%016llX", is_write ? "write" : "read",
            static_cast<unsigned long long>(fault));
    if (fault >= kGuestVirtualBase && fault < kGuestVirtualBase + 0x100000000ull) {
      fprintf(f, "  (guest 0x%08X)", static_cast<unsigned>(fault - kGuestVirtualBase));
    }
    fprintf(f, " ===\n");
    fprintf(f, "thread %lu, faulting pc 0x%p\n", GetCurrentThreadId(), rec->ExceptionAddress);

    void* frames[kMaxFrames]{};
    USHORT n = RtlCaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    DescribeFrame(f, -1, rec->ExceptionAddress);
    for (USHORT i = 0; i < n; ++i) {
      DescribeFrame(f, i, frames[i]);
    }

    DumpHostRegisters(f, info->ContextRecord);
    DumpGuestWord(f, 0x82006798, "printf's default-for-null string pointer");

    // sub_822453A0 formats this and then executes `twi 31,r0,22`, so this is an
    // assertion message and the fault is secondary to whatever assertion fired.
    DumpGuestString(f, 0x8200477C, "assert format string passed to sub_82245878");
    DumpPageState(f, fault);
    ScanGuestFor(f, "AddCategoryAllocator");

    fflush(f);
    fclose(f);
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

struct Installer {
  Installer() {
    // First (1) so it runs before the runtime's own handlers. It only reads.
    AddVectoredExceptionHandler(1, &OnException);
  }
};

Installer g_installer;

}  // namespace
