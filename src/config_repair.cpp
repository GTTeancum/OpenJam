// NBA JAM: On Fire Edition - keeping a saved settings file readable
//
// The settings overlay (F4) saves every changed setting to nbajam_ofe.toml,
// writing text settings as "..." strings without escaping them. A Windows
// folder path such as game_data_root = "D:\Games\Jam" is then not valid TOML,
// since a backslash inside "..." starts an escape, and on the next launch the
// runtime rejects the whole file: every saved setting, fullscreen included,
// is silently gone (part of issue #2). Nothing logs it, because the file is
// read before logging starts.
//
// The runtime does this in its own library, so the port mends the file on its
// way in instead: a saved string that holds a backslash becomes a '...'
// string, which TOML takes literally. Only files the runtime wrote are
// touched, recognised by the header it puts on them; a file someone wrote by
// hand is theirs to get right.

#include "config_repair.h"

#include <fstream>
#include <sstream>
#include <string>

#include <rex/logging.h>

namespace {

constexpr std::string_view kSavedHeader = "# Auto-generated cvar configuration";

// `name = "value"` with a backslash in the value and no ' to get in the way
// of quoting it the other way, rewritten as `name = 'value'`. Anything else
// comes back unchanged.
std::string RepairLine(const std::string& line) {
  const auto eq = line.find('=');
  if (eq == std::string::npos) {
    return line;
  }
  const auto open = line.find_first_not_of(" \t", eq + 1);
  const auto close = line.find_last_not_of(" \t\r");
  if (open == std::string::npos || close <= open || line[open] != '"' ||
      line[close] != '"') {
    return line;
  }
  const std::string value = line.substr(open + 1, close - open - 1);
  if (value.find('\\') == std::string::npos ||
      value.find('\'') != std::string::npos) {
    return line;
  }
  return line.substr(0, open) + "'" + value + "'" + line.substr(close + 1);
}

}  // namespace

void NbaRepairSavedConfig(const std::filesystem::path& config_path) {
  std::ifstream in(config_path, std::ios::binary);
  if (!in) {
    return;
  }
  std::stringstream text;
  text << in.rdbuf();
  in.close();

  const std::string original = text.str();
  if (original.rfind(kSavedHeader, 0) != 0) {
    return;
  }

  std::string repaired;
  std::istringstream lines(original);
  std::string line;
  int changed = 0;
  while (std::getline(lines, line)) {
    const std::string fixed = RepairLine(line);
    changed += fixed != line;
    repaired += fixed;
    repaired += '\n';
  }
  if (changed == 0) {
    return;
  }

  std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return;
  }
  out << repaired;
  // Logging may not be up yet; if it is, this is the only trace of the fix.
  REXLOG_INFO("settings: repaired {} folder path(s) in {} so it can be read",
              changed, config_path.filename().string());
}
