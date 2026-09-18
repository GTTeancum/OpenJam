// NBA JAM: On Fire Edition - DDS texture replacement
//
// See dds_textures.cpp. Codegen emits its own extern declaration for each
// hook, so nothing needs to include this; it exists for the implementation
// file and for readers.

#pragma once

#include <rex/ppc/context.h>

// sub_823FC250(r3 = pointer to a texture resource, r5 = ...).
//
// The guest calls this to turn a blob loaded out of a .ast archive into a
// texture. The hook runs first and, if the active mod supplies a .dds file
// for this texture, rewrites the blob in place so the guest goes on to build
// the replacement instead of the original.
void NbaDdsSubstitute(PPCRegister& r3);
