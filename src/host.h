// NBA JAM: On Fire Edition - the few things the port asks of the host system
//
// The port began Windows-only and called Windows directly wherever it needed
// the executable's folder, an environment variable or the time. These are
// those calls, once, for Windows and for Linux, so that the rest of the port
// does not have to say which it is running on. See host.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The running executable, and the folder it is in - which in a staged game is
// the game folder itself.
std::filesystem::path NbaExePath();
std::filesystem::path NbaExeDir();

// A path from UTF-8 text, which is what the command line and mods.list hold.
std::filesystem::path NbaPathFromUtf8(const std::string& s);

// The command line the process was started with, program name first.
std::vector<std::string> NbaCommandLine();

// An environment variable, or empty when it is not set.
std::string NbaEnv(const char* name);

// Whether an environment variable is set to something other than 0 - the
// form every NBAJAM_* switch takes.
bool NbaEnvOn(const char* name);

// Milliseconds since some fixed point; only differences mean anything.
uint64_t NbaNowMs();

// Whether [p, p + n) is mapped and can be read, without touching it.
bool NbaReadable(const void* p, size_t n);
