#pragma once

#include <filesystem>

enum class NbaSaveMigration { kNoLegacySaves, kImported, kLocalSavesExist };

// Import this title's saves, headers, profile and achievements together. The
// source is read-only; existing local saves are never merged or overwritten.
NbaSaveMigration NbaImportLegacySaves(const std::filesystem::path& legacy,
                                     const std::filesystem::path& destination);
