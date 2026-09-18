// NBA JAM: On Fire Edition - scripted controller input
//
// See scripted_input.cpp. Codegen emits its own extern declaration for each
// hook, so nothing needs to include this; it exists for the implementation
// file and for readers.

#pragma once

#include <rex/ppc/context.h>

// 0x8299C6D4, the instruction after the XamInputGetState call in
// sub_8299C658. r1 is the stack pointer the XINPUT_STATE sits on (at +208,
// buttons at +212); r26 still holds the user index the call was made with.
void NbaScriptedInput(PPCRegister& r1, PPCRegister& r26, PPCRegister& r3);
