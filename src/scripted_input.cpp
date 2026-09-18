// NBA JAM: On Fire Edition - scripted controller input
//
// Driving the game from the outside means synthesising keyboard events, and
// those only reach it while its window has focus - which means taking focus
// from whatever the user is doing. This does the same job from inside the
// process instead: no focus, no window, nothing touched on the desktop.
//
// The game reads the pad in sub_8299C658:
//
//     addi r5,r1,208      ; XINPUT_STATE on the stack
//     li   r4,8
//     mr   r3,r26         ; user index
//     bl   0x82A9C9A0     ; -> XamInputGetState
//     cmplwi r3,0         ; <- the hook sits here
//     lhz  r11,100(r1)
//     lhz  r10,212(r1)    ; wButtons, at r1+208+4
//
// So the hook runs immediately after the kernel has filled the structure and
// before the game reads it. r26 is callee-saved, so the user index the call
// was made with is still there and only pad 0 is touched.
//
// It does not just OR buttons in, because there is usually nothing to OR them
// into: with no controller bound, XamInputGetState returns 1167,
// ERROR_DEVICE_NOT_CONNECTED, the structure is left as the 0xBE stack fill,
// and the game branches away without ever reading it. So the hook presents a
// whole pad - status 0, a packet number that advances when the buttons
// change, and everything else neutral. That makes the port able to drive
// itself with no controller attached and no window focus.
//
// Set a schedule to use it:
//
//     NBAJAM_INPUT_SCRIPT="4000:START;1500:A;1500:A;2000:START"
//
// Each step is <delay in ms>:<button>. The delay is measured from the end of
// the previous step, the button is held for NBAJAM_INPUT_HOLD ms (default
// 150), and then the next step's delay begins. WAIT presses nothing.
//
// Buttons: A B X Y START BACK LB RB LT RT UP DOWN LEFT RIGHT WAIT

#include "scripted_input.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace {

constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

// The XINPUT_STATE the game passes sits at r1+208, so wButtons - the first
// field of the gamepad, after the packet number - is at r1+212. The game
// itself reads it with `lhz r10,212(r1)`.
constexpr uint32_t kStateOffset = 208;
constexpr uint32_t kButtonsOffset = 212;

struct Named {
  const char* name;
  uint16_t bits;
};

constexpr Named kButtons[] = {
    {"UP", 0x0001},   {"DOWN", 0x0002},  {"LEFT", 0x0004},  {"RIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040},    {"RS", 0x0080},
    {"LB", 0x0100},   {"RB", 0x0200},    {"A", 0x1000},     {"B", 0x2000},
    {"X", 0x4000},    {"Y", 0x8000},     {"WAIT", 0x0000},
};

struct Step {
  uint64_t press_at = 0;   // ms since the first poll
  uint64_t release_at = 0;
  uint16_t bits = 0;
  std::string name;
};

std::vector<Step> g_steps;
std::once_flag g_once;
uint64_t g_first_tick = 0;
int g_reported = -1;
bool g_active = false;

std::string EnvStr(const char* name) {
  char buf[1024]{};
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

uint64_t EnvNum(const char* name, uint64_t fallback) {
  const std::string v = EnvStr(name);
  if (v.empty()) return fallback;
  const uint64_t n = strtoull(v.c_str(), nullptr, 10);
  return n ? n : fallback;
}

void Parse() {
  const std::string script = EnvStr("NBAJAM_INPUT_SCRIPT");
  if (script.empty()) {
    return;
  }
  const uint64_t hold = EnvNum("NBAJAM_INPUT_HOLD", 150);

  uint64_t clock = 0;
  size_t pos = 0;
  while (pos <= script.size()) {
    const size_t end = script.find(';', pos);
    std::string piece = script.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = (end == std::string::npos) ? script.size() + 1 : end + 1;
    if (piece.empty()) continue;

    const size_t colon = piece.find(':');
    if (colon == std::string::npos) continue;
    const uint64_t delay = strtoull(piece.substr(0, colon).c_str(), nullptr, 10);
    std::string name = piece.substr(colon + 1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
    for (char& c : name) c = char(toupper(static_cast<unsigned char>(c)));

    uint16_t bits = 0;
    bool known = false;
    for (const auto& b : kButtons) {
      if (name == b.name) {
        bits = b.bits;
        known = true;
        break;
      }
    }
    if (!known) {
      REXLOG_WARN("input script: unknown button '{}'", name);
      continue;
    }
    clock += delay;
    g_steps.push_back({clock, clock + hold, bits, name});
    clock += hold;
  }
  g_active = !g_steps.empty();
  if (g_active) {
    REXLOG_INFO("input script: {} step(s), last at {} ms", g_steps.size(),
                g_steps.back().release_at);
  }
}

}  // namespace

void NbaScriptedInput(PPCRegister& r1, PPCRegister& r26, PPCRegister& r3) {
  std::call_once(g_once, Parse);
  if (!g_active) {
    return;
  }
  static int traced = 0;
  if (traced < 6) {
    ++traced;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(kGuestVirtualBase + r1.u32 + kButtonsOffset);
    REXLOG_INFO("input script: poll pad {} status 0x{:X} r1 0x{:08X} buttons 0x{:04X}",
                r26.u32, r3.u32, r1.u32, (uint16_t(q[0]) << 8) | q[1]);
  }
  if (r26.u32 != 0) {
    return;   // pad 0 only; the others stay disconnected
  }

  const uint64_t now = GetTickCount64();
  if (g_first_tick == 0) {
    g_first_tick = now;
  }
  const uint64_t t = now - g_first_tick;

  uint16_t inject = 0;
  int index = -1;
  for (size_t i = 0; i < g_steps.size(); ++i) {
    const Step& s = g_steps[i];
    if (t >= s.press_at && t < s.release_at) {
      inject |= s.bits;
      index = int(i);
    }
  }
  if (index >= 0 && index != g_reported) {
    g_reported = index;
    REXLOG_INFO("input script: step {} '{}' at {} ms", index, g_steps[index].name, t);
  }
  // Present a connected pad whether or not one exists. Everything is big
  // endian: XINPUT_STATE is dwPacketNumber then XINPUT_GAMEPAD, which is
  // wButtons, two triggers, and four thumb axes.
  static uint16_t last_buttons = 0xFFFF;
  static uint32_t packet = 0;
  if (inject != last_buttons) {
    last_buttons = inject;
    ++packet;
  }

  uint8_t* st = reinterpret_cast<uint8_t*>(kGuestVirtualBase + r1.u32 + kStateOffset);
  st[0] = uint8_t(packet >> 24);
  st[1] = uint8_t(packet >> 16);
  st[2] = uint8_t(packet >> 8);
  st[3] = uint8_t(packet);
  st[4] = uint8_t(inject >> 8);
  st[5] = uint8_t(inject);
  std::memset(st + 6, 0, 10);   // triggers and both sticks centred

  r3.u32 = 0;   // ERROR_SUCCESS: tell the game the pad is there
}
