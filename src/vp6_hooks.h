// NBA JAM: On Fire Edition - VP6 decoder instrumentation
//
// See vp6_hooks.cpp. Codegen emits its own extern declaration for each hook
// into the generated source, so nothing needs to include this; it exists for
// the implementation file and for readers.

#pragma once

#include <rex/ppc/context.h>

// sub_824E5FD8(r3 = destination, r4 = 8x8 signed 16-bit residual block,
// r5 = plane stride).
//
// Registered with `return_on_true`, so the return value decides whether the
// guest body still runs: true means this replaced it. That keeps the hook
// inert unless NBAJAM_VP6_NATIVE_BLOCK is set - an unconditional `ret` would
// disable the guest function on every ordinary run.
bool NbaVp6WriteBlock(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5);
