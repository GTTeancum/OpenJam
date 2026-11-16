// SDL controller input for the mod chooser, independent of guest input gating.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SDL_Gamepad;

struct NbaHostPadState {
  uint32_t id = 0;
  uint16_t buttons = 0;  // Xbox button bits, regardless of the physical pad.
  uint8_t left_trigger = 0;
  std::string name;
};

class NbaHostPads {
 public:
  explicit NbaHostPads(std::filesystem::path mappings);
  ~NbaHostPads();
  NbaHostPads(const NbaHostPads&) = delete;
  NbaHostPads& operator=(const NbaHostPads&) = delete;

  // Call on the UI thread. Any pad can open the chooser with LT+Y; while
  // captured, only that pad can operate it. A disconnect returns neutral input
  // and false so the caller can close the chooser and release guest input.
  bool Poll(NbaHostPadState& state, bool captured);
  const std::string& error() const { return error_; }

 private:
  struct Pad {
    uint32_t id;
    SDL_Gamepad* handle;
  };
  std::filesystem::path mappings_;
  std::vector<Pad> pads_;
  uint32_t selected_ = 0;
  bool attempted_ = false;
  bool initialized_ = false;
  bool owns_events_ = false;
  std::string error_;
};
