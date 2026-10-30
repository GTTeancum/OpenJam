// NBA JAM: On Fire Edition - a Video tab in the game's own Settings screen
//
// See video_tab.cpp. Each of these is a mid-ASM hook named in
// nbajam_ofe_hooks.toml.

#pragma once

#include <rex/ppc/context.h>

// The tab list (sub_822D7688): put Video on the end, name it, give it its
// line of text, and skip the description table it has no entry in.
void NbaVideoTabList(PPCRegister& r1, PPCRegister& r25);
void NbaVideoTabNameKey(PPCRegister& r5, PPCRegister& r28);
void NbaVideoTabName(PPCRegister& r6, PPCRegister& r28);
void NbaVideoTabMessage(PPCRegister& r6, PPCRegister& r28);
void NbaVideoTabNoDescription(PPCRegister& r27, PPCRegister& r28);

// A tab's rows (sub_822E2308), at the switch's common exit.
void NbaVideoTabRows(PPCRegister& r3, PPCRegister& r29, PPCRegister& r31);

// At the first instruction of the choices, current-choice, change and
// Restore Defaults methods. True means the Video tab answered and the game's
// method is skipped.
bool NbaVideoTabRowCount(PPCRegister& r3, PPCRegister& r4);
bool NbaVideoTabChoices(PPCRegister& r3, PPCRegister& r4, PPCRegister& r6);
bool NbaVideoTabValue(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5);
bool NbaVideoTabChange(PPCRegister& r3, PPCRegister& r4, PPCRegister& r6);
bool NbaVideoTabReset(PPCRegister& r4);
