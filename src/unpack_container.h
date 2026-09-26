// NBA JAM: On Fire Edition - getting the game out of an Xbox Live download
//
// See unpack_container.cpp.

#pragma once

#include <filesystem>
#include <functional>
#include <string>

// Whether this file is an Xbox Live container - the single extensionless file
// an Xbox Live Arcade game is on a console's drive. Cheap: it reads a header,
// not the 850 MB behind it.
bool NbaIsLiveContainer(const std::filesystem::path& file);

// The first Xbox Live container in this folder, or an empty path. Containers
// have no extension and a long hexadecimal name, so the folder is scanned
// rather than guessed at.
std::filesystem::path NbaFindLiveContainer(const std::filesystem::path& folder);

// Write everything inside the container out to `into`. `say` is called with
// each file's name and how far along it is, for whatever is showing progress.
// Returns the number of files written, or -1 if the container would not open.
//
// Existing files are overwritten. The caller decides where `into` is; nothing
// here writes outside it.
int NbaUnpackLiveContainer(
    const std::filesystem::path& container, const std::filesystem::path& into,
    const std::function<void(const std::string&, int, int)>& say = nullptr);

// `--unpack <container> [--into <folder>]` on the command line: unpack the
// container and leave without starting the game. Does nothing and returns if
// the command line does not ask for it. Called before anything is set up.
void NbaUnpackIfAsked();
