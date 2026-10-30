// NBA JAM: On Fire Edition - where the guest's memory actually is
//
// See guest_base.h.

#include "guest_base.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace nba_detail {

std::atomic<uintptr_t> g_guest_base{0};

uintptr_t LookUpGuestBase() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) {
    return 0;
  }
  const auto base =
      reinterpret_cast<uintptr_t>(kernel->memory()->virtual_membase());
  // Every caller that gets here would store the same answer, so a race
  // between two of them is harmless.
  g_guest_base.store(base, std::memory_order_relaxed);
  return base;
}

}  // namespace nba_detail

void NbaReportGuestBase() {
  constexpr uintptr_t kUsual = 0x0000000100000000ull;
  const uintptr_t base = NbaGuestBase();
  if (base == kUsual) {
    REXLOG_INFO("guest memory: at 0x{:016X}, where it usually is", base);
  } else {
    REXLOG_WARN("guest memory: at 0x{:016X}, not the usual 0x{:016X} - "
                "something else in this process got there first. The port "
                "follows it there.", base, kUsual);
  }
}
