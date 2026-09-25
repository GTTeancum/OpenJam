// NBA JAM: On Fire Edition - switching mods from inside the game
//
// See mod_swap.cpp. The list and the leaving are here; the screen that shows
// the list is src/mod_picker.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// One installed game: the base game or a mod. The id is what its saves are
// filed under, the path is the game root it plays, the name is what a person
// calls it. The last three are what the chooser says about it and may be
// empty, which means the mod did not say.
struct NbaMod {
  std::string id;
  std::string path;
  std::string name;
  std::string version;
  std::string author;
  std::string files;
};

// Everything installed beside this game, in the order it is listed, and which
// of them is the one running. False when there is no list to read - a game
// root that was not staged by tools/dlc.py, which is not an error.
bool NbaModList(std::vector<NbaMod>* mods, size_t* active);

// Leave this game and come back up in that one. Does not return when it
// works: this process is replaced by the new one.
void NbaModSwapTo(const NbaMod& mod);

// Which game root is being played. A staged game is started by
// double-clicking it, with nothing on the command line at all, so the only
// thing that knows is whatever resolved the paths.
void NbaModSwapUseRoot(const std::filesystem::path& root);

// Called before anything is mapped: if this process is a replacement, wait
// for the one it replaced to finish leaving. See mod_swap.cpp.
void NbaModSwapWaitForPredecessor();

// Whether this process is a replacement - it took over from a game that was
// already running, rather than being started by someone.
bool NbaModSwapIsRelaunch();

// Put this window in front, for a replacement taking over from the game the
// person was already looking at. Does nothing otherwise.
void NbaModSwapTakeForeground(void* hwnd);
