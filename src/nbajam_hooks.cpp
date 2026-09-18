// NBA JAM: On Fire Edition - mid-ASM diagnostic hooks (implementation)
//
// Writes to hooks.txt beside the executable rather than the runtime log, so the
// output stays readable and is not interleaved with kernel tracing.
//
// Every record opens, appends and closes the file. An earlier version held a
// buffered FILE* for the process lifetime and produced a zero-byte file: the
// process dies shortly after these hooks fire, and nothing that was still in
// the stdio buffer survived. Opening per record is slow and completely
// reliable, which is the right trade for a few dozen diagnostic lines.

#include "nbajam_hooks.h"

#include <windows.h>

#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::mutex g_mutex;
int g_events = 0;
constexpr int kMaxEvents = 40;

// The runtime maps the guest arena at a fixed host base.
constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

const char* ReportPath() {
  static char path[MAX_PATH]{};
  if (!path[0]) {
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p(exe);
    size_t slash = p.find_last_of("\\/");
    p = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "hooks.txt";
    strncpy_s(path, p.c_str(), _TRUNCATE);
  }
  return path;
}

void Emit(const char* fmt, ...) {
  FILE* f = nullptr;
  if (fopen_s(&f, ReportPath(), "a") != 0 || !f) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fflush(f);
  fclose(f);
}

bool ReadGuest(uint32_t guest, void* out, size_t len) {
  __try {
    memcpy(out, reinterpret_cast<const void*>(kGuestVirtualBase + guest), len);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A guest pointer can look plausible and still be backed by nothing.
const char* Backing(uint32_t guest) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(kGuestVirtualBase + guest), &mbi, sizeof(mbi))) {
    return "query-failed";
  }
  if (mbi.State == MEM_COMMIT) {
    return "committed";
  }
  if (mbi.State == MEM_RESERVE) {
    return "RESERVED-NOT-COMMITTED";
  }
  return "FREE";
}

// Recompiled guest functions are ordinary C++ functions named sub_<address>,
// so a native stack walk reads as a guest call stack. DbgHelp takes the loader
// lock from a guest thread that may hold the runtime's global critical region,
// so this is best-effort and never allowed to take the process with it.
void EmitStack() {
  __try {
    static bool inited = false;
    if (!inited) {
      SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
      SymInitialize(GetCurrentProcess(), nullptr, TRUE);
      inited = true;
    }
    void* frames[32]{};
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (USHORT i = 0; i < n && i < 18; ++i) {
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = MAX_SYM_NAME;
      DWORD64 disp = 0;
      if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]), &disp, sym)) {
        Emit("    [%2d] %s\n", i, sym->Name);
      } else {
        Emit("    [%2d] 0x%p\n", i, frames[i]);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Emit("    <stack walk failed>\n");
  }
}

void EmitGuestString(const char* label, uint32_t guest) {
  char s[256]{};
  size_t n = 0;
  for (; n < sizeof(s) - 1; ++n) {
    char ch = 0;
    if (!ReadGuest(guest + static_cast<uint32_t>(n), &ch, 1) || ch == '\0') {
      break;
    }
    s[n] = ch;
  }
  Emit("  %s = 0x%08X \"%s\"\n", label, guest, s);
}

}  // namespace

void NbaTraceFileReadEntry(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                           PPCRegister& r6) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (++g_events > kMaxEvents) {
    return;
  }
  Emit("\n--- sub_82552730(fileObj=0x%08X, buffer=0x%08X [%s], len=0x%X, offset=0x%llX)\n",
       r3.u32, r4.u32, Backing(r4.u32), r5.u32,
       static_cast<unsigned long long>(r6.u64));
  EmitStack();
}

// Called from the overridden REX_CALL_INDIRECT_FUNC in
// templates/codegen/pch_h.inja, for the two kernel thunks the memory.cfg load
// goes through. The game reaches them through the function table rather than a
// direct `bl`, so mid-ASM hooks on the recompiled call sites never fired.
void NbaTraceIndirect(uint32_t target, PPCContext& ctx) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (++g_events > kMaxEvents) {
    return;
  }
  const char* which = target == 0x82B1884Cu ? "NtCreateFile" : "NtReadFile";
  Emit("\n=== indirect -> %s (0x%08X) ===\n", which, target);
  Emit("  r1(stack)=0x%08X [%s]\n", ctx.r1.u32, Backing(ctx.r1.u32));
  Emit("  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
       ctx.r6.u32);
  Emit("  r7=0x%08X [%s]\n", ctx.r7.u32, Backing(ctx.r7.u32));
  Emit("  r8=0x%08X [%s]\n", ctx.r8.u32, Backing(ctx.r8.u32));
  Emit("  r9=0x%08X r10=0x%08X\n", ctx.r9.u32, ctx.r10.u32);
  if (target == 0x82B1891Cu) {
    EmitGuestString("buf as text", ctx.r8.u32);
  }
  EmitStack();
}

void NbaCreateSite1(PPCRegister& r3, PPCRegister& r4) { (void)r3; (void)r4; }
void NbaCreateSite2(PPCRegister& r3, PPCRegister& r4) { (void)r3; (void)r4; }
void NbaCreateSite3(PPCRegister& r3, PPCRegister& r4) { (void)r3; (void)r4; }
void NbaCreateSite4(PPCRegister& r3, PPCRegister& r4) { (void)r3; (void)r4; }
void NbaCreateSite5(PPCRegister& r3, PPCRegister& r4) { (void)r3; (void)r4; }

void NbaReadSite1(PPCRegister& r3, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9,
                  PPCRegister& r10) {
  (void)r3; (void)r7; (void)r8; (void)r9; (void)r10;
}
void NbaReadSite2(PPCRegister& r3, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9,
                  PPCRegister& r10) {
  (void)r3; (void)r7; (void)r8; (void)r9; (void)r10;
}
void NbaReadSite3(PPCRegister& r3, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9,
                  PPCRegister& r10) {
  (void)r3; (void)r7; (void)r8; (void)r9; (void)r10;
}

// Reports the guest stack pointer on entry to a function. A healthy r1 is
// 8-byte aligned and sits inside the thread's stack allocation; the failure
// here is an r1 of 0xBEBE431E, which is neither.
void NbaTraceR1(PPCRegister& r1) {
  const uint32_t sp = r1.u32;
  const bool aligned = (sp & 0xF) == 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  // Only one healthy reading, as a baseline, then the first few bad ones.
  // Printing every probe buried the answer under hundreds of healthy lines from
  // constructors that run repeatedly, and interleaved badly across threads.
  static int healthy_shown = 0;
  static int bad_shown = 0;
  if (aligned) {
    if (healthy_shown++ > 0) {
      return;
    }
    Emit("\nbaseline r1 = 0x%08X  aligned  [%s]\n", sp, Backing(sp));
    EmitStack();
    return;
  }
  if (bad_shown++ >= 3) {
    return;
  }
  Emit("\n*** FIRST MISALIGNED r1 = 0x%08X  [%s] ***\n", sp, Backing(sp));
  EmitStack();
}

// Reports the first indirect call that returns with a broken stack pointer.
// r1 is correct on entry to xstart and broken by the time xstart calls
// sub_8225AB88; the call that loses it is inside sub_82246B10, a CRT
// _initterm that runs static initializers through function pointers. Comparing
// r1 across every indirect call names the exact initializer.
void NbaCheckR1(uint32_t target, uint32_t r1_before, PPCContext& ctx) {
  const uint32_t after = ctx.r1.u32;
  if (after == r1_before) {
    return;
  }
  const bool was_ok = (r1_before & 0xF) == 0;
  const bool now_bad = (after & 0xF) != 0;
  if (!was_ok || !now_bad) {
    return;  // A callee legitimately adjusting r1 is not interesting.
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  static int reported = 0;
  if (++reported > 3) {
    return;
  }
  Emit("\n*** r1 BROKEN by indirect call to 0x%08X: 0x%08X -> 0x%08X ***\n", target, r1_before,
       after);
  EmitStack();
}

// See the header: r31 is the frame pointer sub_82558718 restores r1 from.
void NbaTraceR31(PPCRegister& r1, PPCRegister& r31) {
  const uint32_t fp = r31.u32;
  // 0xBEBExxxx means it came from stack memory that was never written.
  const bool poisoned = (fp & 0xFFFF0000u) == 0xBEBE0000u;
  std::lock_guard<std::mutex> lock(g_mutex);
  static int healthy = 0;
  static int bad = 0;
  if (!poisoned) {
    if (healthy++ > 0) {
      return;
    }
    Emit("\nbaseline r31 = 0x%08X (r1 = 0x%08X)\n", fp, r1.u32);
    EmitStack();
    return;
  }
  if (bad++ >= 3) {
    return;
  }
  Emit("\n*** r31 POISONED = 0x%08X (r1 = 0x%08X) ***\n", fp, r1.u32);
  EmitStack();
}

// Shared implementation for the per-site probes generated by
// tools/probe_sites.py. Reports every reading where r31 has picked up the
// 0xBExxxxxx fill pattern, plus the first healthy one as a baseline, with the
// site label so the reading is attributable.
void NbaProbeSite(const char* label, PPCRegister& r1, PPCRegister& r31) {
  const uint32_t sp = r1.u32;
  const uint32_t fp = r31.u32;
  const bool poisoned = (fp & 0xFF000000u) == 0xBE000000u;
  std::lock_guard<std::mutex> lock(g_mutex);
  static int healthy = 0;
  static int bad = 0;
  if (!poisoned) {
    if (healthy++ > 2) {
      return;
    }
    Emit("\n[ok]   %s\n         r1=0x%08X r31=0x%08X\n", label, sp, fp);
    return;
  }
  if (bad++ >= 6) {
    return;
  }
  Emit("\n[BAD]  %s\n         r1=0x%08X r31=0x%08X  <-- r31 poisoned\n", label, sp, fp);
  EmitStack();
}
