// NBA JAM: On Fire Edition - the controller, straight from Windows
//
// See host_pad.cpp.

#pragma once

#include <cstdint>

// The first controller Windows can see: its buttons, as XINPUT reports them,
// and how far the left trigger is pressed. False when there is none.
//
// This is deliberately not the pad the game is holding. See src/mod_picker.cpp
// for why the mod chooser has to read its own.
bool NbaHostPad(uint16_t* buttons, uint8_t* left_trigger);
