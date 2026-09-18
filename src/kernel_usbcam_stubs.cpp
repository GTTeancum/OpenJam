// NBA JAM: On Fire Edition - Xbox LIVE Vision camera kernel stubs
//
// NBA JAM imports XUsbcamGetState and XUsbcamSetConfig. ReXGlue v0.10.0 ships
// an implementation of these (src/kernel/xboxkrnl/xboxkrnl_usbcam.cpp) but the
// file is commented out of the SDK's kernel build, so the symbols are absent
// from rexruntime and the link fails.
//
// This file supplies them locally. The behaviour matches the SDK source: report
// the camera as not connected, which is the correct answer on a PC with no
// Xbox LIVE Vision camera attached, and is what the game sees on a 360 without
// one plugged in.
//
// Remove this file if a future SDK release enables xboxkrnl_usbcam.cpp.

#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace nbajam_ofe {

// X_STATUS_SUCCESS expands to a cast through X_STATUS, which lives in rex::.
// The SDK's own copy of this file sits inside namespace rex::kernel::xboxkrnl
// and picks it up unqualified; this one does not.
using rex::X_STATUS;

u32 XUsbcamCreate_entry(u32 buffer, u32 buffer_size,
                             mapped_void unk3_ptr) {
  // Must report success: titles that check this result can take a degenerate
  // init path on failure and then run on uninitialized data.
  return X_STATUS_SUCCESS;
}

u32 XUsbcamGetState_entry() {
  // 0 = no camera connected.
  return 0;
}

}  // namespace nbajam_ofe

REX_EXPORT(__imp__XUsbcamCreate, nbajam_ofe::XUsbcamCreate_entry)
REX_EXPORT(__imp__XUsbcamGetState, nbajam_ofe::XUsbcamGetState_entry)

REX_EXPORT_STUB(__imp__XUsbcamSetCaptureMode);
REX_EXPORT_STUB(__imp__XUsbcamGetConfig);
REX_EXPORT_STUB(__imp__XUsbcamSetConfig);
REX_EXPORT_STUB(__imp__XUsbcamReadFrame);
REX_EXPORT_STUB(__imp__XUsbcamSnapshot);
REX_EXPORT_STUB(__imp__XUsbcamSetView);
REX_EXPORT_STUB(__imp__XUsbcamGetView);
REX_EXPORT_STUB(__imp__XUsbcamDestroy);
REX_EXPORT_STUB(__imp__XUsbcamReset);
