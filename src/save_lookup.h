// NBA JAM: On Fire Edition - finding the save the game made last time
//
// See save_lookup.cpp.

#pragma once

#include <rex/ppc/context.h>

// Mid-ASM hook in sub_8254C8E8, the content-listing wrapper, on the
// instruction after its call to XamContentAggregateCreateEnumerator. Redoes
// that call with the player's whole 64-bit id.
void NbaSaveLookupFullId(PPCRegister& r1, PPCRegister& r3, PPCRegister& r26,
                         PPCRegister& r27, PPCRegister& r31);
