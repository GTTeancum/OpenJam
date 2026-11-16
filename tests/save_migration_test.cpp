#include "save_migration.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << data;
  Require(bool(out), "fixture write failed");
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(bool(in), "expected file is missing");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "Pass an empty fixture directory inside the repository");
    const fs::path root = fs::absolute(argv[1]);
    Require(!fs::exists(root), "Fixture directory already exists");
    fs::create_directories(root);
    const auto legacy = root / "Documents" / "nbajam_ofe";
    const auto local = root / "game" / "saves" / "base";
    const fs::path progress = "B13EBABEBABEBABE/584111EC/00000001/PROGRESS 20261030113223/PROGRESS 20261030113223";
    const fs::path header = "B13EBABEBABEBABE/584111EC/Headers/00000001/PROGRESS 20261030113223.header";
    const std::string payload(150123, '\x9f');
    Require(NbaImportLegacySaves(legacy, local) == NbaSaveMigration::kNoLegacySaves,
            "missing legacy directory should be harmless");
    Write(legacy / progress, payload);
    Write(legacy / header, "save header");
    Write(legacy / "584111EC/profile/Player1/profile", "profile");
    Write(legacy / "achievements/584111EC.toml", "achievement");
    Write(legacy / "cache/shaders/old.xsh", "cache must not migrate");
    Write(legacy / "B13EBABEBABEBABE/99999999/other", "another title");
    fs::create_directories(local);  // an earlier empty first launch
    Require(NbaImportLegacySaves(legacy, local) == NbaSaveMigration::kImported,
            "complete saves should import into an empty local directory");
    Require(Read(local / progress) == payload, "save bytes changed");
    Require(Read(local / header) == "save header", "save header was not imported");
    Require(Read(local / "584111EC/profile/Player1/profile") == "profile", "profile missing");
    Require(Read(local / "achievements/584111EC.toml") == "achievement", "achievements missing");
    Require(Read(legacy / progress) == payload, "legacy save must remain intact");
    Require(!fs::exists(local / "cache"), "shader cache was imported");
    Require(!fs::exists(local / "B13EBABEBABEBABE/99999999"), "another title was imported");
    Write(local / progress, "new local progress");
    Require(NbaImportLegacySaves(legacy, local) == NbaSaveMigration::kLocalSavesExist,
            "a later launch must preserve local progress");
    Require(Read(local / progress) == "new local progress", "local progress overwritten");
    const auto retry = root / "retry" / "base";
    Write(root / "retry/.base-import-0/partial", "interrupted import");
    Require(NbaImportLegacySaves(legacy, retry) == NbaSaveMigration::kImported,
            "interrupted imports must be retryable");
    Require(Read(retry / progress) == payload, "retried save differs");
    Require(!fs::exists(retry / "partial"), "partial import contaminated final saves");
    Require(Read(root / "retry/.base-import-0/partial") == "interrupted import",
            "recovery files should remain intact");
    Write(root / "blocked", "not a directory");
    bool failed = false;
    try { NbaImportLegacySaves(legacy, root / "blocked/base"); }
    catch (const fs::filesystem_error&) { failed = true; }
    Require(failed, "failed migrations must not report success");
    Require(Read(legacy / progress) == payload, "failed import damaged original");
    std::cout << "Save migration: import, headers, profiles, achievements, conflicts, retry and failure checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
