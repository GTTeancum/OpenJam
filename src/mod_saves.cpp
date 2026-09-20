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
// So each game root says where its own saves go: tools/dlc.py leaves a
// `saves.path` file in it naming a folder under the game's own `saves`
// directory, one per mod. Written relative to the root where it can be, so
// moving the whole game folder keeps the saves attached to it.
//
// This is read in OnConfigurePaths, which runs after the defaults and the
// command line have both been read and before anything opens a save - so it
// holds however the game was started, a relaunch from a mod swap included,
// and nothing has to be passed along.
//
// Two things follow from a root carrying its own answer:
//
//   - a mod added tomorrow needs no change here and no entry anywhere. Its
//     root is built with the file in it, and its saves follow.
//   - a game root that is not one of ours, the extracted game itself for
//     instance, says nothing and keeps the plain save location. That is the
//     right answer rather than a missing case: there is no mod to separate.

#include "mod_saves.h"

#include <fstream>
#include <string>

std::filesystem::path NbaSavePath(const std::filesystem::path& game_root) {
  if (game_root.empty()) {
    return {};
  }
  std::error_code ec;
  const std::filesystem::path marker = game_root / "saves.path";
  if (!std::filesystem::exists(marker, ec)) {
    return {};
  }
  std::ifstream in(marker);
  std::string line;
  std::getline(in, line);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
  if (line.empty()) {
    return {};
  }
  // Relative to the root it was found in, which is what keeps a moved game
  // folder pointing at its own saves rather than at where they used to be.
  std::filesystem::path want(line);
  if (want.is_relative()) {
    want = game_root / want;
  }
  const std::filesystem::path tidy = want.lexically_normal();
  return tidy;
}

std::filesystem::path NbaSaveRootFor(const std::filesystem::path& user_data_root,
                                     const std::filesystem::path& game_root) {
  const std::filesystem::path want = NbaSavePath(game_root);
  if (want.empty()) {
    return user_data_root;
  }
  std::error_code ec;
  std::filesystem::create_directories(want, ec);
  return want;
}
