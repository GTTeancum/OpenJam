// NBA JAM: On Fire Edition - keeping one mod's save away from another's
//
// Every mod is the same executable and so the same title, and the game files
// saved content under the title and nothing else. Left alone, all of them
// share one save.
//
// That is not the harmless kind of sharing. The roster databases are
// structurally identical - 442,996 bytes, the same 34 tables, the same 509
// player and 67 team slots - because a mod replaces what is in the slots
// rather than adding to them. Player 137 exists in every one of them and is a
// different person in each. A save records unlocks, purchases, records and
// Road Trip progress by slot, so one carried across would unlock players
// nobody unlocked and show a campaign finished against teams never played.
//
// So the saves are split, and split by a rule rather than by a list. A game
// root built by tools/dlc.py carries a `mod.id` file naming the mod it holds;
// the save folder is then wherever saves would have gone, plus that name. It
// is done here, in OnConfigurePaths, because that runs after the defaults and
// the command line have both been read and before anything opens a save - so
// it holds however the game was started, including a relaunch from a mod
// swap, and needs nothing to be passed along.
//
// The two things that follow from the rule being a rule:
//
//   - a mod added tomorrow needs no change here and no entry anywhere. Its
//     root carries its id, and its saves follow.
//   - a game root that is not one of ours, the extracted game itself for
//     instance, has no id and keeps the plain save location. That is the
//     right answer rather than a missing case: there is no mod to separate.
//
// An explicit --user_data_root still decides where saves live; the mod's name
// is appended to it, so pointing the whole lot somewhere else keeps working
// and keeps the mods apart.

#include "mod_saves.h"

#include <cctype>
#include <fstream>

namespace {

// A mod id is written by us and is already a slug, but it lands in a path, so
// anything that has no business in a folder name is dropped rather than
// trusted.
std::string Sanitise(const std::string& raw) {
  std::string out;
  for (char c : raw) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '-' || c == '_' || c == '.') {
      out.push_back(c);
    }
  }
  while (!out.empty() && out.front() == '.') {
    out.erase(out.begin());       // no leading dots, no `..`
  }
  return out;
}

}  // namespace

std::string NbaModId(const std::filesystem::path& game_root) {
  if (game_root.empty()) {
    return {};
  }
  std::error_code ec;
  const std::filesystem::path marker = game_root / "mod.id";
  if (!std::filesystem::exists(marker, ec)) {
    return {};
  }
  std::ifstream in(marker);
  std::string line;
  std::getline(in, line);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
  return Sanitise(line);
}

std::filesystem::path NbaSaveRootFor(const std::filesystem::path& user_data_root,
                                     const std::filesystem::path& game_root) {
  const std::string id = NbaModId(game_root);
  if (id.empty()) {
    return user_data_root;
  }
  return user_data_root / id;
}
