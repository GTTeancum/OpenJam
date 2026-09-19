// NBA JAM: On Fire Edition - switching mods from inside the game
//
// See mod_swap.cpp. Not a hook of its own: src/scripted_input.cpp already sits
// where the pad has been read and not yet used, so it calls this.

#pragma once

#include <cstdint>

// The real pad, as the kernel filled it in, before anything else touches it.
void NbaModSwapPad(uint16_t buttons);
