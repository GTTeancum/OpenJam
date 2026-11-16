#include "save_migration.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr const char* kTitle = "584111EC";

bool IsPlayerDirectory(const std::string& name) {
  return name.size() == 16 &&
         name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

void CopyVerified(const fs::path& source, const fs::path& target) {
  const auto status = fs::symlink_status(source);
  if (fs::is_symlink(status)) {
    throw std::runtime_error("Save migration does not follow symbolic links: " + source.string());
  }
  if (fs::is_directory(status)) {
    fs::create_directories(target);
    for (const auto& entry : fs::directory_iterator(source)) {
      CopyVerified(entry.path(), target / entry.path().filename());
    }
    return;
  }
  if (!fs::is_regular_file(status)) {
    throw std::runtime_error("Unsupported save file: " + source.string());
  }
  fs::create_directories(target.parent_path());
  fs::copy_file(source, target);  // No overwrite, including in staging.
  if (fs::file_size(source) != fs::file_size(target)) {
    throw std::runtime_error("Save copy has the wrong size: " + target.string());
  }
  std::ifstream original(source, std::ios::binary), copied(target, std::ios::binary);
  if (!original || !copied) throw std::runtime_error("Cannot verify save copy: " + target.string());
  std::array<char, 65536> a, b;
  do {
    original.read(a.data(), a.size());
    copied.read(b.data(), b.size());
    if (original.gcount() != copied.gcount() ||
        !std::equal(a.begin(), a.begin() + original.gcount(), b.begin()) ||
        original.bad() || copied.bad()) {
      throw std::runtime_error("Save copy verification failed: " + target.string());
    }
  } while (!original.eof());
  fs::last_write_time(target, fs::last_write_time(source));
}
}  // namespace

NbaSaveMigration NbaImportLegacySaves(const fs::path& legacy,
                                     const fs::path& destination) {
  if (!fs::exists(legacy)) return NbaSaveMigration::kNoLegacySaves;
  if (fs::exists(destination) && !fs::is_empty(destination)) {
    return NbaSaveMigration::kLocalSavesExist;
  }
  std::vector<fs::path> relative_paths;
  for (const auto& entry : fs::directory_iterator(legacy)) {
    const auto name = entry.path().filename().string();
    if (IsPlayerDirectory(name) && fs::exists(entry.path() / kTitle)) {
      if (fs::is_symlink(entry.symlink_status())) {
        throw std::runtime_error("Save migration does not follow player-directory links");
      }
      relative_paths.push_back(fs::path(name) / kTitle);
    }
  }
  for (const fs::path& relative :
       {fs::path(kTitle), fs::path("achievements") / (std::string(kTitle) + ".toml")}) {
    if (fs::exists(legacy / relative)) relative_paths.push_back(relative);
  }
  if (relative_paths.empty()) return NbaSaveMigration::kNoLegacySaves;

  fs::create_directories(destination.parent_path());
  fs::path staging;
  // Keep an interrupted import for recovery and use a fresh staging folder.
  // Never expose a partially copied save package to the game.
  for (unsigned i = 0; ; ++i) {
    staging = destination.parent_path() /
              ("." + destination.filename().string() + "-import-" + std::to_string(i));
    if (fs::create_directory(staging)) break;
  }
  for (const auto& relative : relative_paths) {
    fs::path check = legacy;
    for (const auto& component : relative) {
      check /= component;
      if (fs::is_symlink(fs::symlink_status(check))) {
        throw std::runtime_error("Save migration does not follow symbolic links: " + check.string());
      }
    }
    CopyVerified(legacy / relative, staging / relative);
  }
  if (fs::exists(destination)) {
    // remove() only succeeds for an empty directory. A concurrent launch
    // writing saves makes this fail instead of replacing its progress.
    if (!fs::is_directory(destination) || !fs::is_empty(destination) ||
        !fs::remove(destination)) {
      throw std::runtime_error("Local saves changed during migration; original files retained");
    }
  }
  // Windows scanners can briefly hold a newly verified file without delete
  // sharing, preventing its parent directory from being renamed. Retry only
  // transient sharing/access errors; never replace an existing destination.
  std::error_code publish_error;
  for (unsigned attempt = 0; ; ++attempt) {
    fs::rename(staging, destination, publish_error);
    if (!publish_error) break;
#ifdef _WIN32
    // Win32 ERROR_ACCESS_DENIED / SHARING_VIOLATION / LOCK_VIOLATION.
    const int code = publish_error.value();
    if ((code == 5 || code == 32 || code == 33) && attempt < 20 &&
        !fs::exists(destination)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
#endif
    throw fs::filesystem_error("Cannot activate imported saves; originals retained",
                               staging, destination, publish_error);
  }
  return NbaSaveMigration::kImported;
}
