// NBA JAM: On Fire Edition - keeping a saved settings file readable
//
// See config_repair.cpp.

#pragma once

#include <filesystem>

// Rewrite folder paths in a settings file the runtime saved, so that the
// runtime can read that file back. Call before the runtime loads it.
void NbaRepairSavedConfig(const std::filesystem::path& config_path);
