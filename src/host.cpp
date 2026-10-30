// NBA JAM: On Fire Edition - the few things the port asks of the host system
//
// See host.h.

#include "host.h"

#include <chrono>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#endif

std::filesystem::path NbaExePath() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return {};
  return std::filesystem::path(buf);
#else
  std::error_code ec;
  const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : p;
#endif
}

std::filesystem::path NbaExeDir() {
  const auto p = NbaExePath();
  return p.empty() ? p : p.parent_path();
}

std::filesystem::path NbaPathFromUtf8(const std::string& s) {
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()),
                                    nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
  return std::filesystem::path(w);
#else
  return std::filesystem::path(s);
#endif
}

std::vector<std::string> NbaCommandLine() {
  std::vector<std::string> out;
#ifdef _WIN32
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 0; i < argc; ++i) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0,
                                        nullptr, nullptr);
    std::string a(size_t(len > 0 ? len - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, a.data(), len, nullptr,
                        nullptr);
    out.push_back(a);
  }
  LocalFree(argv);
#else
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  std::string arg;
  while (std::getline(in, arg, '\0')) out.push_back(arg);
#endif
  return out;
}

std::string NbaEnv(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

bool NbaEnvOn(const char* name) {
  const std::string v = NbaEnv(name);
  return !v.empty() && v[0] != '0';
}

uint64_t NbaNowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool NbaReadable(const void* p, size_t n) {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD)) return false;
  const auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return reinterpret_cast<uintptr_t>(p) + n <= end;
#else
  // mincore fails with ENOMEM for any page in the range that is not mapped.
  // It says nothing about protection, so a page mapped with no access passes;
  // the guest arena's untouched pages are the case that matters, and those
  // are mapped readable.
  const long page = sysconf(_SC_PAGESIZE);
  const auto start = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(page - 1);
  const auto end = reinterpret_cast<uintptr_t>(p) + (n ? n : 1);
  std::vector<unsigned char> v((end - start + page - 1) / page);
  return mincore(reinterpret_cast<void*>(start), end - start, v.data()) == 0;
#endif
}
