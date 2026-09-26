// NBA JAM: On Fire Edition - getting the game out of an Xbox Live download
//
// What a player owns is one file with no extension and a long hexadecimal
// name, sitting on their console's drive. Everything the game is - the
// executable, the archives, the audio - is inside it, in the format Xbox
// Live Arcade titles are delivered in.
//
// Until now this port could not open one, and the instructions had to send
// people off to find a separate tool before they could play. The runtime has
// always had the reader; nothing was calling it. This calls it.
//
// It is deliberately something a player does once rather than something the
// game does on every launch. Unpacked, the game is a folder: mods can be
// linked into it, saves sit beside it, and one file can be replaced without
// rewriting 850 MB. That is what the rest of this port is built on.

#include "unpack_container.h"

#include <windows.h>
#include <shellapi.h>

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <system_error>
#include <vector>

// X_STATUS_SUCCESS casts to rex's X_STATUS, so the name has to be in scope.
using rex::X_STATUS;

namespace fs = std::filesystem;

namespace {

// Big enough that most files are one read, small enough not to matter.
constexpr size_t kChunk = 8u * 1024u * 1024u;

// Smaller than any Arcade title, larger than any readme or icon that might
// be sitting in the same folder.
constexpr uintmax_t kSmallestGame = 40ull * 1024ull * 1024ull;

// The name Xbox Live gives a downloaded title: hexadecimal, no extension.
bool NamedLikeAContainer(const fs::path& file) {
  if (file.has_extension()) return false;
  const std::string name = file.filename().string();
  if (name.size() < 16) return false;
  for (const char c : name) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

// Print to a console this process borrowed, if it managed to borrow one.
void Say(bool console, const std::string& line) {
  if (console) {
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
  }
}

}  // namespace

bool NbaIsLiveContainer(const fs::path& file) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) return false;
  // The header says what it is. A name that looks right over a header that
  // does not is somebody's notes, not a game.
  return rex::filesystem::StfsContainerDevice::ReadPackageHeader(file) !=
         nullptr;
}

fs::path NbaFindLiveContainer(const fs::path& folder) {
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return {};
  // The ones that look the part are checked first, so a folder full of other
  // things does not cost a header read each.
  std::vector<fs::path> likely, rest;
  for (const auto& e : fs::directory_iterator(folder, ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec)) continue;
    if (e.file_size(ec) < kSmallestGame) continue;
    (NamedLikeAContainer(e.path()) ? likely : rest).push_back(e.path());
  }
  for (const auto& p : likely) {
    if (NbaIsLiveContainer(p)) return p;
  }
  for (const auto& p : rest) {
    if (NbaIsLiveContainer(p)) return p;
  }
  return {};
}

int NbaUnpackLiveContainer(
    const fs::path& container, const fs::path& into,
    const std::function<void(const std::string&, int, int)>& say) {
  rex::filesystem::StfsContainerDevice device("", container);
  if (!device.Initialize()) {
    REXLOG_ERROR("unpack: {} would not open as an Xbox Live container",
                 container.string());
    return -1;
  }
  rex::filesystem::Entry* root = device.ResolvePath("/");
  if (!root) {
    REXLOG_ERROR("unpack: {} has nothing in it", container.string());
    return -1;
  }

  // Breadth first, so a directory exists before the files that go in it.
  // Counted first, so there is something honest to show progress against.
  int total = 0;
  {
    std::queue<rex::filesystem::Entry*> counting;
    counting.push(root);
    while (!counting.empty()) {
      rex::filesystem::Entry* e = counting.front();
      counting.pop();
      for (const auto& child : e->children()) counting.push(child.get());
      if (!(e->attributes() & rex::filesystem::kFileAttributeDirectory)) {
        ++total;
      }
    }
  }

  std::error_code ec;
  fs::create_directories(into, ec);

  std::vector<uint8_t> buffer(kChunk);
  std::queue<rex::filesystem::Entry*> queue;
  queue.push(root);
  int written = 0;
  while (!queue.empty()) {
    rex::filesystem::Entry* entry = queue.front();
    queue.pop();
    for (const auto& child : entry->children()) queue.push(child.get());

    const fs::path out = into / fs::path(entry->path());
    if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
      fs::create_directories(out, ec);
      continue;
    }
    fs::create_directories(out.parent_path(), ec);

    rex::filesystem::File* in = nullptr;
    if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in) !=
        X_STATUS_SUCCESS) {
      REXLOG_WARN("unpack: could not read {} out of the container",
                  entry->path());
      continue;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, out.string().c_str(), "wb") != 0 || !f) {
      REXLOG_ERROR("unpack: could not write {}", out.string());
      in->Destroy();
      return -1;
    }
    // In pieces: some of these are hundreds of megabytes and there is no
    // reason to hold one in memory to copy it.
    size_t at = 0;
    const size_t size = entry->size();
    bool ok = true;
    while (at < size) {
      const size_t want = (std::min)(kChunk, size - at);
      size_t got = 0;
      if (in->ReadSync(std::span<uint8_t>(buffer.data(), want), at, &got) !=
              X_STATUS_SUCCESS ||
          got == 0) {
        ok = false;
        break;
      }
      if (std::fwrite(buffer.data(), 1, got, f) != got) {
        ok = false;
        break;
      }
      at += got;
    }
    std::fclose(f);
    in->Destroy();
    if (!ok) {
      REXLOG_ERROR("unpack: {} stopped short at {} of {} bytes", entry->path(),
                   at, size);
      return -1;
    }
    ++written;
    if (say) say(entry->path(), written, total);
  }
  REXLOG_INFO("unpack: {} file(s) out of {} and into {}", written,
              container.filename().string(), into.string());
  return written;
}

void NbaUnpackIfAsked() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return;
  fs::path container, into;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--unpack" && i + 1 < argc) {
      container = argv[++i];
    } else if (a == L"--into" && i + 1 < argc) {
      into = argv[++i];
    }
  }
  LocalFree(argv);
  if (container.empty()) return;
  if (into.empty()) into = container.parent_path();

  // This executable has no console of its own. Borrowing whoever started it
  // means progress shows up for a person running it from a prompt, and costs
  // nothing when something else started it.
  const bool console = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
  if (console) {
    FILE* attached = nullptr;
    freopen_s(&attached, "CONOUT$", "w", stdout);
  }

  std::error_code ec;
  fs::create_directories(into, ec);
  // A line on disk as well, for whatever is drawing a progress bar rather
  // than reading a console.
  const fs::path note = into / "unpacking.txt";

  char line[1024];
  std::snprintf(line, sizeof(line), "unpacking %s\n",
                container.filename().string().c_str());
  Say(console, line);

  int last = -1;
  const int n = NbaUnpackLiveContainer(
      container, into,
      [&](const std::string& name, int done, int total) {
        const int pct = total ? (done * 100 / total) : 0;
        if (pct == last) return;
        last = pct;
        char progress[1024];
        std::snprintf(progress, sizeof(progress), "%3d%%  %s\n", pct,
                      name.c_str());
        Say(console, progress);
        FILE* f = nullptr;
        if (fopen_s(&f, note.string().c_str(), "w") == 0 && f) {
          std::fprintf(f, "%d %d %s\n", done, total, name.c_str());
          std::fclose(f);
        }
      });

  fs::remove(note, ec);
  if (n < 0) {
    Say(console, "that file is not an Xbox Live container this can read\n");
    ExitProcess(1);
  }
  std::snprintf(line, sizeof(line), "done: %d file(s) in %s\n", n,
                into.string().c_str());
  Say(console, line);
  ExitProcess(0);
}
