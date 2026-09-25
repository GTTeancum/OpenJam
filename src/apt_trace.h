// NBA JAM: On Fire Edition - front-end script tracing
//
// See apt_trace.cpp. Codegen emits its own extern declaration for each hook,
// so nothing needs to include this; it exists for the implementation file and
// for readers.

#pragma once

#include <rex/ppc/context.h>

// 0x82330438, the ActionScript execution loop. r4 is the block of bytecode to
// run and r6 its length, with a flag in the top bit.
void NbaAptBlock(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                 PPCRegister& r6);

// Whether the game is showing its main menu, which is the one screen the mod
// chooser belongs on. Cheap: a comparison against a timestamp the script hook
// keeps up to date. See apt_trace.cpp for how it knows.
bool NbaFrontEndOnMainMenu();
