// NBA JAM: On Fire Edition - keeping one mod's save away from another's
//
// See mod_saves.cpp.

#pragma once

#include <filesystem>
#include <string>

// What mod a game root holds, from the `mod.id` file tools/dlc.py leaves in
// it. Empty for a root that is not one of ours - the extracted game itself,
// say - which is not an error.
std::string NbaModId(const std::filesystem::path& game_root);

// Where that mod's saves belong, given wherever saves would otherwise go.
// Returns `user_data_root` unchanged when the root carries no id.
std::filesystem::path NbaSaveRootFor(const std::filesystem::path& user_data_root,
                                     const std::filesystem::path& game_root);
