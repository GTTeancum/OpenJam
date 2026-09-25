// NBA JAM: On Fire Edition - the list of mods, on screen, to choose from
//
// The main menu's right-hand panel lists what is installed and marks the one
// running, but it is part of the front end: EA's screen, drawn from data that
// was fixed when the game folder was built. It cannot hold a cursor, because
// nothing in it can change while the game is up.
//
// So choosing happens here instead, drawn over the panel by the runtime's own
// ImGui layer - the same layer the frame rate in the window title is counted
// from. Hold the left trigger and press Y and the panel comes alive: the same
// list in the same place, with the orange box now following the d-pad instead
// of marking what is running. A loads the one it is on, B puts it back. Arrow
// keys, Enter and Escape do the same from the keyboard, and F9 opens it,
// which is worth having on a PC port and is the way in if a pad is not being
// read.
//
// It is drawn over the panel rather than in the middle of the screen so that
// nothing pops up: what was already on the menu is what is being used. That
// takes knowing where the panel is, which is the block of stage coordinates
// below - measured off the screen, and in the same units tools/frontend.py
// places the static cards with.
//
// **Being obviously a mode.** The panel on the menu is not live - it is text
// and boxes baked into the front end, and the game has no idea a cursor
// exists until the trigger is held. If the live one looked the same, people
// would press A at the wrong time and start a game instead. So while it is
// up: the screen behind dims, the panel takes an orange frame, the header
// changes from MODS to SELECT A MOD, the button hints appear, and the orange
// box stops meaning "this is running" and starts meaning "this is where the
// cursor is". None of that is decoration; it is the difference between the
// two states.
//
// **Being there at once.** Both halves of the first frame used to be slow:
// the list was read from disk when first asked for, and the typefaces were
// rasterised a glyph at a time as they were first drawn, so the panel
// underneath showed through for a moment. Both are done at startup now,
// before anyone has asked for anything - see Warm.
//
// **It is drawn in the game's own typefaces.** The front end's four faces sit
// in data/xenon/fe/fonts/fonts.ast as ordinary TrueType files;
// tools/dlc.py unpacks the two the port needs into a `ui` folder beside the
// executable, and they are added to the font atlas at startup. The display
// face is the one the menu items themselves are set in, so the mod list reads
// as part of the same screen rather than as a debug overlay. Without those
// files nothing breaks: the list falls back to the runtime's own face.
//
// **The buttons it names are the game's own pictures.** The help bar along
// the bottom of every screen draws A, B, Y, the triggers and the d-pad from a
// set of textures in the front end; tools/dlc.py unpacks the five this needs
// into the same `ui` folder as the typefaces, and they are packed into the
// font atlas at startup and drawn from there. So the prompts here are the
// prompts the rest of the game gives, down to the pixel. If the files are not
// there, discs with letters on them are drawn instead and nothing breaks.
//
// **It lives on the main menu and nowhere else.** src/apt_trace.cpp watches
// the script the front end runs and can tell when the main menu is the screen
// showing; this is drawn whenever that is true and not drawn at all when it
// is not, so it cannot be left over a match. On that screen it is simply part
// of the furniture - the list, with the game that is loaded marked - until
// the trigger and Y wake it up, and then it has the pad until B closes it,
// something is loaded, or the screen changes. It can be a plain toggle
// because it cannot outlive the screen it belongs to.
//
// **Where the controller comes from.** Not from the game. The game asks the
// kernel for the pad with a flag the runtime does not accept - it answers
// "nothing plugged in" before it looks at any hardware - so the one place in
// the frame where the pad was supposed to be waiting is always empty. The
// rest of the front end is unaffected because it reads the pad a different
// way, which is why every other button works and this one did not.
//
// So the chooser asks Windows directly. That is independent of anything the
// runtime does with the pad, and it is why the trigger works here while the
// game itself never sees it at that call.
//
// While it is open the game must not also be taking the input, or moving down
// the list would move down the menu as well. The runtime has a switch for
// exactly this - it is what it uses for its own overlays - and holding it
// down makes every driver report an untouched pad to the game while the
// chooser is up. src/scripted_input.cpp also empties the state it hands over,
// which covers the scripted-input path used for testing.
//
// Two threads meet here. The pad arrives on a guest thread and the drawing
// happens on the UI thread, so what they share is small and behind a lock:
// whether it is open, which row is chosen, and the list itself.

#include "mod_picker.h"

#include "apt_trace.h"
#include "host_pad.h"
#include "mod_swap.h"

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>   // custom atlas rectangles, for the buttons

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>


namespace {

// The buttons this watches, as XINPUT reports them.
constexpr uint16_t kUp = 0x0001;
constexpr uint16_t kDown = 0x0002;
constexpr uint16_t kBack = 0x0020;
constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;
constexpr uint16_t kY = 0x8000;

// Far enough in to be deliberate, short of the point the game itself treats
// the trigger as held.
constexpr uint8_t kTriggerHeld = 64;

// The panel's colours, which are the ones the main menu's mod list uses.
const ImU32 kOrange = IM_COL32(255, 162, 60, 255);
const ImU32 kBlue = IM_COL32(74, 134, 232, 255);
const ImU32 kPlateTop = IM_COL32(7, 12, 21, 255);
const ImU32 kPlateEdge = IM_COL32(38, 54, 78, 255);
const ImU32 kRule = IM_COL32(40, 56, 80, 255);
const ImU32 kDim = IM_COL32(0, 0, 0, 120);
const ImU32 kRowIdle = IM_COL32(16, 28, 48, 245);
const ImU32 kRowChosen = IM_COL32(58, 40, 18, 245);
const ImU32 kName = IM_COL32(245, 247, 250, 255);
const ImU32 kQuiet = IM_COL32(150, 165, 185, 255);

// The face colours of the buttons themselves, as the front end draws them:
// a coloured disc with a dark letter.
const ImU32 kFaceA = IM_COL32(61, 168, 71, 255);
const ImU32 kFaceB = IM_COL32(201, 56, 45, 255);
const ImU32 kFaceY = IM_COL32(234, 180, 32, 255);
const ImU32 kFacePlate = IM_COL32(38, 46, 60, 255);
const ImU32 kFaceRing = IM_COL32(236, 240, 245, 255);

// The front end is drawn on a fixed 1280x720 stage whatever the window is,
// so everything here is in stage units and scaled on the way out.
constexpr float kStageW = 1280.0f;
constexpr float kStageH = 720.0f;

// The JAMnet panel, and the cards inside it. The panel movie's own
// coordinates turn out to be stage coordinates offset by where it sits, so
// the card figures are the ones the static cards are placed with.
constexpr float kPanelX = 600.0f;
constexpr float kPanelY = 100.0f;
constexpr float kPanelW = 500.0f;
constexpr float kPanelH = 517.0f;
constexpr float kCardX = 26.0f;
constexpr float kCardW = 446.0f;
constexpr float kCardTop = 84.0f;
constexpr float kCardPitch = 52.0f;
constexpr float kCardH = 46.0f;
constexpr float kHeadX = 46.0f;
constexpr float kHeadY = 47.0f;

// The block at the bottom that says what the cursor is sitting on, and the
// button hints under it. Both are pinned to the bottom of the panel so that
// the layout does not move about as mods are added.
constexpr float kAboutY = 352.0f;
constexpr float kFootY = 468.0f;

// The panel wears a JAMnet tab across its top-left corner and a stick icon in
// its top-right, both of which sit a little outside the panel's own edge, so
// the fill starts slightly above and to the left of it and takes them with
// it. Measured, like everything else here, off the screen.
constexpr float kPlateOut = 5.0f;

// The panel's corners are rounded, and so are these.
constexpr float kPlateRound = 10.0f;

constexpr float kHeadSize = 23.0f;
constexpr float kRowSize = 20.0f;
constexpr float kTagSize = 14.0f;
constexpr float kAboutSize = 19.0f;
constexpr float kLineSize = 15.0f;
constexpr float kFootSize = 16.0f;

// How many cards fit above the block that describes the chosen one.
constexpr int kVisible = int((kAboutY - 16.0f - kCardTop) / kCardPitch);

// The two faces the front end is set in, loaded from the `ui` folder beside
// the executable. Null until then, and null for good if the folder is not
// there, which is what the fallbacks below are for.
ImFont* g_display = nullptr;
ImFont* g_text = nullptr;

ImFont* Display() { return g_display ? g_display : ImGui::GetFont(); }
ImFont* Text() { return g_text ? g_text : ImGui::GetFont(); }

// One of the game's button pictures, parked in the font atlas. The pixels are
// kept because the atlas repacks itself as new letter sizes are baked into
// it, and a rectangle that has moved has to be written again where it landed.
struct Picture {
  ImFontAtlasRectId id = ImFontAtlasRectId_Invalid;
  // How big to draw it next to text, as a multiple of the others. The d-pad
  // is drawn on a larger canvas than the face buttons with a lot of space
  // around it, so at the same height it comes out half the size.
  float relative = 1.0f;
  int w = 0;
  int h = 0;
  int at_x = -1;
  int at_y = -1;
  std::vector<unsigned char> pixels;
  bool loaded() const { return id != ImFontAtlasRectId_Invalid; }
};

Picture g_pic_a;
Picture g_pic_b;
Picture g_pic_y;
Picture g_pic_lt;
Picture g_pic_pad;

// Put a picture where the atlas says it now lives. Cheap when nothing moved,
// which is almost always.
void Settle(ImFontAtlas* atlas, Picture& p) {
  if (!atlas || !p.loaded()) return;
  ImFontAtlasRect r;
  if (!atlas->GetCustomRect(p.id, &r)) return;
  if (int(r.x) == p.at_x && int(r.y) == p.at_y) return;
  ImTextureData* tex = atlas->TexData;
  if (!tex || !tex->Pixels || tex->BytesPerPixel != 4) return;
  for (int y = 0; y < p.h; ++y) {
    std::memcpy(tex->GetPixelsAt(int(r.x), int(r.y) + y),
                p.pixels.data() + size_t(y) * size_t(p.w) * 4u,
                size_t(p.w) * 4u);
  }
  tex->UseColors = true;
  ImFontAtlasTextureBlockQueueUpload(atlas, tex, int(r.x), int(r.y), p.w, p.h);
  p.at_x = int(r.x);
  p.at_y = int(r.y);
}

// The runtime's input system, so the game can be made deaf while the chooser
// has the pad.
rex::input::InputSystem* g_input = nullptr;

// Whether the controller is being read here rather than through the game. It
// is, whenever Windows can see one.
std::atomic<bool> g_own_pad{false};

std::mutex g_lock;
std::vector<NbaMod> g_mods;
size_t g_running = 0;          // the one being played
int g_chosen = 0;              // the one the cursor is on
bool g_listed = false;         // mods.list has been read
std::atomic<bool> g_open{false};

// Read the list once, the first time it is wanted. Nothing can add a mod
// while the game is up: a new one arrives by staging the game folder again,
// which the game is not running during.
void EnsureListed() {
  if (g_listed) return;
  g_listed = true;
  NbaModList(&g_mods, &g_running);
  g_chosen = int(g_running);
}

void Open() {
  {
    std::lock_guard<std::mutex> guard(g_lock);
    EnsureListed();
    g_chosen = int(g_running);   // always open on the one playing
  }
  g_open.store(true, std::memory_order_relaxed);
}

// Named for what it does rather than `Close`, which is already a member of
// the dialog this draws through.
void Shut() {
  g_open.store(false, std::memory_order_relaxed);
}

void Move(int by) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_mods.empty()) return;
  const int n = int(g_mods.size());
  g_chosen = (g_chosen + by % n + n) % n;
}

// Take the one the cursor is on. This does not come back when it works: the
// process is replaced by one playing that mod.
void Load() {
  NbaMod want;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_mods.empty()) return;
    if (g_chosen < 0 || size_t(g_chosen) >= g_mods.size()) return;
    want = g_mods[size_t(g_chosen)];
  }
  g_open.store(false, std::memory_order_relaxed);
  NbaModSwapTo(want);
}

// The front end sets every menu item in capitals, so these are too.
std::string Shout(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = char(toupper(static_cast<unsigned char>(c)));
  return out;
}

float Wide(ImFont* font, float size, const char* text) {
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

// The stage, fitted into the window the way the game's own picture is: as
// large as it goes with its shape kept, and centred in whatever is left over.
struct Fit {
  float scale = 1.0f;
  float x = 0.0f;
  float y = 0.0f;
  ImVec2 at(float sx, float sy) const {
    return ImVec2(x + sx * scale, y + sy * scale);
  }
};

Fit StageFit(const ImVec2& display) {
  Fit f;
  f.scale = (std::min)(display.x / kStageW, display.y / kStageH);
  f.x = (display.x - kStageW * f.scale) * 0.5f;
  f.y = (display.y - kStageH * f.scale) * 0.5f;
  return f;
}

// The game's own picture of a button, drawn to a given height. Returns how
// much room it took, or nothing when that picture was not installed.
float Stamp(ImDrawList* dl, Picture& p, float x, float mid, float h) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (!atlas || !p.loaded()) return 0.0f;
  Settle(atlas, p);
  ImFontAtlasRect r;
  if (!atlas->GetCustomRect(p.id, &r)) return 0.0f;
  const float tall = h * p.relative;
  const float w = tall * float(p.w) / float(p.h);
  dl->AddImage(atlas->TexRef, ImVec2(x, mid - tall * 0.5f),
               ImVec2(x + w, mid + tall * 0.5f), r.uv0, r.uv1);
  return w;
}

// The fallback, for a game folder staged before the pictures were unpacked: a
// disc in the button's colour, a pale ring, and a white letter in the menu
// face. Returns how much room it took.
float Button(ImDrawList* dl, float x, float mid, float r, const char* letter,
             ImU32 face) {
  const ImVec2 c(x + r, mid);
  dl->AddCircleFilled(c, r, face, 28);
  dl->AddCircle(c, r - 0.5f, kFaceRing, 28, (std::max)(1.0f, r * 0.15f));
  const float size = r * 1.45f;
  const float w = Wide(Display(), size, letter);
  dl->AddText(Display(), size, ImVec2(c.x - w * 0.5f, c.y - size * 0.60f),
              kFaceRing, letter);
  return r * 2.0f;
}

// And the d-pad, which has no letter: the same disc with a triangle at each
// end of it.
float DPad(ImDrawList* dl, float x, float mid, float r) {
  const ImVec2 c(x + r, mid);
  const float t = r * 0.40f;
  dl->AddCircleFilled(c, r, kFacePlate, 28);
  dl->AddCircle(c, r - 0.5f, kFaceRing, 28, (std::max)(1.0f, r * 0.15f));
  dl->AddTriangleFilled(ImVec2(c.x, mid - r * 0.56f),
                        ImVec2(c.x - t, mid - r * 0.10f),
                        ImVec2(c.x + t, mid - r * 0.10f), kFaceRing);
  dl->AddTriangleFilled(ImVec2(c.x, mid + r * 0.56f),
                        ImVec2(c.x - t, mid + r * 0.10f),
                        ImVec2(c.x + t, mid + r * 0.10f), kFaceRing);
  return r * 2.0f;
}

class Picker : public rex::ui::ImGuiDialog {
 public:
  explicit Picker(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    Warm(io);
    // Off the main menu there is nothing to draw and nothing to be in the
    // middle of: whatever was going on stops being true the moment the screen
    // changes.
    if (!NbaFrontEndOnMainMenu()) {
      if (g_open.load(std::memory_order_relaxed)) Shut();
      return;
    }
    Pad();
    Keyboard();
    const bool live = g_open.load(std::memory_order_relaxed);

    std::vector<NbaMod> mods;
    size_t running = 0;
    int chosen = 0;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      mods = g_mods;
      running = g_running;
      chosen = g_chosen;
    }

    // No ImGui window: this is drawn straight onto the frame, in the panel's
    // own place, so that it reads as the panel rather than as something on
    // top of it.
    const Fit fit = StageFit(io.DisplaySize);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float s = fit.scale;

    // Everything else steps back, but only while this has the pad.
    if (live) {
      dl->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, kDim);
    }

    // The plate, over the whole panel - tab, header and all.
    const ImVec2 p0 = fit.at(kPanelX - kPlateOut, kPanelY - kPlateOut);
    const ImVec2 p1 = fit.at(kPanelX + kPanelW, kPanelY + kPanelH);
    const float round = kPlateRound * s;
    dl->AddRectFilled(p0, p1, kPlateTop, round);
    dl->AddRect(p0, p1, live ? kOrange : kPlateEdge, round, 0,
                (std::max)(live ? 2.0f : 1.0f, (live ? 3.0f : 2.0f) * s));

    // The header says which of the two states this is in.
    dl->AddText(Display(), kHeadSize * s,
                fit.at(kPanelX + kHeadX, kPanelY + kHeadY),
                live ? kOrange : kName, live ? "SELECT A MOD" : "MODS");
    char count[48];
    std::snprintf(count, sizeof(count), "%d GAMES", int(mods.size()));
    const float cw = Wide(Display(), kLineSize * s, count);
    ImVec2 cat = fit.at(kPanelX + kCardX + kCardW, kPanelY + kHeadY + 6.0f);
    cat.x -= cw;
    dl->AddText(Display(), kLineSize * s, cat, kQuiet, count);
    Rule(dl, fit, kCardTop - 14.0f);

    // Only so many cards fit. The list slides under the cursor rather than
    // the cursor running off the end of the panel.
    const int count_n = int(mods.size());
    int first = 0;
    if (count_n > kVisible) {
      first = (std::min)((std::max)(chosen - kVisible / 2, 0),
                         count_n - kVisible);
    }
    const int last = (std::min)(count_n, first + kVisible);

    if (mods.empty()) {
      dl->AddText(Text(), kRowSize * s,
                  fit.at(kPanelX + kCardX, kPanelY + kCardTop), kQuiet,
                  "Nothing installed.");
    }
    for (int i = first; i < last; ++i) {
      const float top = kCardTop + float(i - first) * kCardPitch;
      const ImVec2 a = fit.at(kPanelX + kCardX, kPanelY + top);
      const ImVec2 b = fit.at(kPanelX + kCardX + kCardW, kPanelY + top + kCardH);
      const bool is_running = size_t(i) == running;
      // Orange means "this is the one loaded" until the trigger is held, and
      // "this is where the cursor is" after that.
      const bool is_chosen = live ? (i == chosen) : is_running;
      dl->AddRectFilled(a, b, is_chosen ? kRowChosen : kRowIdle, 3.0f * s);
      dl->AddRect(a, b, is_chosen ? kOrange : kBlue, 3.0f * s, 0,
                  (std::max)(1.0f, (is_chosen ? 3.0f : 2.0f) * s));
      const std::string name = Shout(mods[size_t(i)].name);
      dl->AddText(Display(), kRowSize * s,
                  fit.at(kPanelX + kCardX + 16.0f,
                         kPanelY + top + (kCardH - kRowSize) * 0.5f - 1.0f),
                  kName, name.c_str());
      if (is_running) {
        const float w = Wide(Display(), kTagSize * s, "ACTIVE");
        ImVec2 at = fit.at(kPanelX + kCardX + kCardW - 16.0f,
                           kPanelY + top + (kCardH - kTagSize) * 0.5f);
        at.x -= w;
        dl->AddText(Display(), kTagSize * s, at, kOrange, "ACTIVE");
      }
    }
    // A mark when the list runs past the panel in either direction.
    if (first > 0) {
      Arrow(dl, fit, kCardTop - 9.0f, true);
    }
    if (last < count_n) {
      Arrow(dl, fit, kCardTop + float(kVisible) * kCardPitch - 1.0f, false);
    }

    // What the cursor is sitting on. The room is there, and a mod nobody can
    // tell apart from another mod is not much use.
    Rule(dl, fit, kAboutY - 18.0f);
    const int about = live ? chosen : int(running);
    if (!mods.empty() && about >= 0 && about < count_n) {
      const NbaMod& m = mods[size_t(about)];
      dl->AddText(Display(), kAboutSize * s,
                  fit.at(kPanelX + kCardX, kPanelY + kAboutY), kName,
                  Shout(m.name).c_str());

      std::string who;
      if (!m.version.empty()) who = "Version " + m.version;
      if (!m.author.empty()) {
        if (!who.empty()) who += " ";
        who += "by " + m.author;
      }
      if (who.empty()) who = "The game as EA shipped it.";
      dl->AddText(Text(), kLineSize * s,
                  fit.at(kPanelX + kCardX, kPanelY + kAboutY + 30.0f), kQuiet,
                  who.c_str());

      std::string what = m.files.empty()
                             ? std::string("Nothing changed: teams and "
                                           "players as they shipped.")
                             : m.files + " files changed: teams, players, "
                                         "courts and art.";
      dl->AddText(Text(), kLineSize * s,
                  fit.at(kPanelX + kCardX, kPanelY + kAboutY + 52.0f), kQuiet,
                  what.c_str());
      dl->AddText(Text(), kLineSize * s,
                  fit.at(kPanelX + kCardX, kPanelY + kAboutY + 74.0f), kQuiet,
                  (size_t(about) == running)
                      ? "Playing now. Its saves, records and unlocks are its "
                        "own."
                      : "Keeps its own saves, records and unlocks.");
    }

    // Along the bottom: how to wake it up, or what it can do now that it is
    // awake. Never both, because a prompt for a button that does nothing yet
    // is how people come to think a screen is listening when it is not.
    const float mid = fit.at(0.0f, kPanelY + kFootY + kFootSize * 0.5f).y;
    float x = fit.at(kPanelX + kCardX, 0.0f).x;
    if (!live) {
      const float r = kFootSize * 0.72f * s;
      float took = Stamp(dl, g_pic_lt, x, mid, r * 2.2f);
      if (took <= 0.0f) took = Button(dl, x, mid, r, "LT", kFacePlate);
      x += took + 6.0f * s;
      took = Stamp(dl, g_pic_y, x, mid, r * 2.2f);
      if (took <= 0.0f) took = Button(dl, x, mid, r, "Y", kFaceY);
      x += took + 9.0f * s;
      dl->AddText(Display(), kFootSize * s,
                  ImVec2(x, mid - kFootSize * s * 0.60f), kQuiet,
                  "together to change mods");
      return;
    }
    const struct {
      Picture* pic;
      const char* letter;
      ImU32 face;
      const char* label;
    } keys[] = {
        {&g_pic_pad, nullptr, 0, "Choose"},
        {&g_pic_a, "A", kFaceA, "Load"},
        {&g_pic_b, "B", kFaceB, "Close"},
    };
    for (const auto& k : keys) {
      const float r = kFootSize * 0.72f * s;
      float took = Stamp(dl, *k.pic, x, mid, r * 2.2f);
      if (took <= 0.0f) {
        took = k.letter ? Button(dl, x, mid, r, k.letter, k.face)
                        : DPad(dl, x, mid, r);
      }
      x += took;
      x += 7.0f * s;
      dl->AddText(Display(), kFootSize * s,
                  ImVec2(x, mid - kFootSize * s * 0.60f), kName, k.label);
      x += Wide(Display(), kFootSize * s, k.label) + 22.0f * s;
    }
  }

  // A hairline across the panel, at a height in stage units.
  static void Rule(ImDrawList* dl, const Fit& fit, float y) {
    dl->AddLine(fit.at(kPanelX + kCardX, kPanelY + y),
                fit.at(kPanelX + kCardX + kCardW, kPanelY + y), kRule,
                (std::max)(1.0f, fit.scale));
  }

  // There is more list above or below than the panel is showing.
  static void Arrow(ImDrawList* dl, const Fit& fit, float y, bool up) {
    const float w = 7.0f, h = up ? -7.0f : 7.0f;
    const float cx = kPanelX + kCardX + kCardW * 0.5f;
    dl->AddTriangleFilled(fit.at(cx, kPanelY + y + h),
                          fit.at(cx - w, kPanelY + y),
                          fit.at(cx + w, kPanelY + y), kQuiet);
  }

  // Everything the first frame would otherwise have to do while someone is
  // looking at it: read the list, and put every letter this draws into the
  // font atlas at the sizes it draws them. Done once, and again if the window
  // changes size, because a size is rasterised per size.
  static void Warm(const ImGuiIO& io) {
    static float was_scale = 0.0f;
    const float s = StageFit(io.DisplaySize).scale;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      EnsureListed();
    }
    if (s == was_scale) return;
    was_scale = s;
    const float sizes[] = {kHeadSize, kRowSize, kTagSize, kAboutSize,
                           kLineSize, kFootSize, kFootSize + 1.0f,
                           kFootSize * 0.72f * 1.45f};
    for (float size : sizes) {
      Bake(Display(), size * s);
      Bake(Text(), size * s);
    }
  }

  static void Bake(ImFont* font, float size) {
    if (!font || size <= 0.0f) return;
    ImFontBaked* baked = font->GetFontBaked(size);
    if (!baked) return;
    for (ImWchar c = 32; c < 127; ++c) {
      baked->FindGlyph(c);
    }
  }

 private:
  // The controller, read here rather than taken from the game. Same rules as
  // the guest-side path below: the trigger and Y open it, the trigger keeps
  // it open, and letting go puts it away.
  static void Pad() {
    // A test drives the chooser through the guest instead, and on a machine
    // with a controller plugged in the two would fight, so it can say so.
    static const bool ignore = [] {
      char buf[8]{};
      const DWORD n = GetEnvironmentVariableA("NBAJAM_IGNORE_HOST_PAD", buf,
                                              sizeof(buf));
      return n > 0 && n < sizeof(buf) && buf[0] != '0';
    }();
    uint16_t buttons = 0;
    uint8_t trigger = 0;
    if (ignore || !NbaHostPad(&buttons, &trigger)) {
      g_own_pad.store(false, std::memory_order_relaxed);
      return;
    }
    if (!g_own_pad.exchange(true, std::memory_order_relaxed)) {
      REXLOG_INFO("mods: the chooser is reading the controller itself");
    }
    NbaModPickerFeed(buttons, trigger);
  }

  // The same moves from the keyboard. Worth having on its own account, and it
  // is the way in when the pad is not reaching the game.
  static void Keyboard() {
    const bool open = g_open.load(std::memory_order_relaxed);
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
      if (open) {
        Shut();
      } else {
        Open();
      }
      return;
    }
    if (!open) return;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) Move(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) Move(1);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      Shut();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      Load();
    }
  }
};

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return {};
  return std::filesystem::path(buf).parent_path();
}

ImFont* AddFace(ImFontAtlas* atlas, const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    return nullptr;
  }
  // Loaded once at a nominal size; every draw asks for the size it wants and
  // the atlas rasterises that size on demand.
  return atlas->AddFontFromFileTTF(file.string().c_str(), 32.0f);
}

}  // namespace

// A picture as tools/dlc.py leaves it: a tag, the size, then the pixels.
// Deliberately the plainest thing that works, so that nothing in the runtime
// has to know how to read an image file.
void AddPicture(ImFontAtlas* atlas, const std::filesystem::path& file,
                Picture* out) {
  std::FILE* f = nullptr;
  if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) return;
  unsigned char head[12]{};
  const bool ok = std::fread(head, 1, sizeof(head), f) == sizeof(head) &&
                  std::memcmp(head, "NBTX", 4) == 0;
  if (!ok) {
    std::fclose(f);
    return;
  }
  const int w = int(head[4] | head[5] << 8 | head[6] << 16 | head[7] << 24);
  const int h = int(head[8] | head[9] << 8 | head[10] << 16 | head[11] << 24);
  if (w <= 0 || h <= 0 || w > 256 || h > 256) {
    std::fclose(f);
    return;
  }
  std::vector<unsigned char> pixels(size_t(w) * size_t(h) * 4u);
  const bool read = std::fread(pixels.data(), 1, pixels.size(), f) ==
                    pixels.size();
  std::fclose(f);
  if (!read) return;

  ImFontAtlasRect r;
  const ImFontAtlasRectId id = atlas->AddCustomRect(w, h, &r);
  if (id == ImFontAtlasRectId_Invalid) return;
  out->id = id;
  out->w = w;
  out->h = h;
  out->pixels = std::move(pixels);
  Settle(atlas, *out);
}

void NbaModPickerFonts(ImFontAtlas* atlas) {
  if (!atlas) return;
  const std::filesystem::path ui = ExeDir() / "ui";
  g_display = AddFace(atlas, ui / "display.ttf");
  g_text = AddFace(atlas, ui / "text.ttf");
  AddPicture(atlas, ui / "glyph_a.tex", &g_pic_a);
  AddPicture(atlas, ui / "glyph_b.tex", &g_pic_b);
  AddPicture(atlas, ui / "glyph_y.tex", &g_pic_y);
  AddPicture(atlas, ui / "glyph_lt.tex", &g_pic_lt);
  AddPicture(atlas, ui / "glyph_dpad.tex", &g_pic_pad);
  g_pic_pad.relative = 1.7f;
  if (!g_display || !g_text) {
    REXLOG_INFO("mods: no game typefaces in '{}' - drawing the list in the "
                "runtime's own", ui.string());
  }
}

void NbaModPickerUseInput(rex::input::InputSystem* input) {
  g_input = input;
  if (!g_input) return;
  // While the chooser has the pad, every driver reports an untouched one to
  // the game. This is the runtime's own mechanism for its overlays; taking it
  // over means the menu underneath does not move while the list is being
  // walked.
  g_input->SetActiveCallback([] {
    return !g_open.load(std::memory_order_relaxed);
  });
}

void NbaModPickerCreate(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer) return;
  // A dialog adds itself to the drawer and is owned by it from then on; this
  // one is never closed, so it lives as long as the drawer does.
  new Picker(drawer);
}

bool NbaModPickerPad(uint16_t buttons, uint8_t left_trigger) {
  // Two things can offer a pad: the frame loop, with the controller read
  // straight from Windows, and src/scripted_input.cpp with whatever the guest
  // was handed, which is how a test drives it with no controller about. Only
  // one of them gets listened to, or the one holding nothing would undo the
  // one holding something on the very next frame.
  if (g_own_pad.load(std::memory_order_relaxed)) {
    return g_open.load(std::memory_order_relaxed);
  }
  return NbaModPickerFeed(buttons, left_trigger);
}

bool NbaModPickerFeed(uint16_t buttons, uint8_t left_trigger) {
  // The pad is read on every screen; this only has a claim on one of them.
  if (!NbaFrontEndOnMainMenu()) {
    if (g_open.load(std::memory_order_relaxed)) Shut();
    return false;
  }
  static uint16_t last = 0;
  static bool last_combo = false;
  const uint16_t pressed = uint16_t(buttons & ~last);
  last = buttons;

  const bool held = left_trigger >= kTriggerHeld;
  const bool combo = held && (buttons & kY) != 0;
  const bool combo_now = combo && !last_combo;
  last_combo = combo;

  const bool was_open = g_open.load(std::memory_order_relaxed);
  if (combo_now) {
    // The same pair of buttons both ways, so whoever opened it knows how to
    // put it away without being told.
    if (was_open) {
      Shut();
    } else {
      Open();
    }
    return true;      // the Y that worked it is not the game's
  }
  if (!was_open) {
    return false;
  }
  if (pressed & kUp) Move(-1);
  if (pressed & kDown) Move(1);
  if (pressed & (kB | kBack)) {
    Shut();
  } else if (pressed & kA) {
    Load();
  }
  return true;        // open: the game sees nothing until it closes
}
