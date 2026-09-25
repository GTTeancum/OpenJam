// nbajam_ofe - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <windows.h>

#include <filesystem>
#include <system_error>

#include "mod_picker.h"
#include "mod_saves.h"
#include "mod_swap.h"
#include "window_title.h"

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>

class NbajamOfeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // The folder the executable is in, which in a staged game is the game
  // folder itself.
  static std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) {
      return {};
    }
    return std::filesystem::path(buf).parent_path();
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NbajamOfeApp>(new NbajamOfeApp(ctx, "nbajam_ofe",
        PPCImageConfig));
  }

  // Report the title as owned rather than as a trial.
  //
  // XBLA titles decide this from XamContentGetLicenseMask, where each bit is a
  // granted license and bit 0 conventionally means "purchased". ReXGlue returns
  // whatever the `license_mask` CVar holds, and that defaults to 0 - so every
  // recompiled XBLA title comes up in trial mode out of the box. NBA JAM does
  // exactly that: it has a whole `data/xenon/fe/bounce/screens/trial` tree and
  // gates content on this.
  //
  // The extracted package carries a LIVE signature with License 0 granting
  // bit 0, so 1 is what the real console reports for an owned copy.
  //
  // This only fills in a mask that is still zero. Both the config file and the
  // command line are applied in SetupEnvironment, before this hook runs in
  // SetupPresentation, so setting it unconditionally would silently override
  // whatever the user asked for. Guarding on zero keeps `--license_mask=3` and
  // friends working.
  //
  // The one thing it cannot preserve is an explicit `--license_mask=0`, which
  // is indistinguishable from the default. To look at the trial-only screens
  // under `data/xenon/fe/bounce/screens/trial`, comment out the line below.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // First thing of all, and before the guest's memory is mapped: if this
    // process is a mod swap, the one it replaced may still be holding the
    // address that memory has to go at. See src/mod_swap.cpp.
    NbaModSwapWaitForPredecessor();
    // A staged game is a folder you can double-click into, so the flags that
    // are mandatory for this title are supplied when nothing else did. Only
    // when still unset: the config file and the command line are both applied
    // before this runs, so anything asked for stays asked for.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";      // otherwise the window is black
    }
    if (REXCVAR_GET(license_mask) == 0u) {
      REXCVAR_SET(license_mask, 1u);
    }
    // Windowed unless someone asks for otherwise. The runtime starts
    // fullscreen by default, and the game then asks for its own video mode a
    // moment later, so a plain double-click filled the screen and then
    // dropped into a window - which is both startling and not what this port
    // is for.
    if (!rex::cvar::HasNonDefaultValue("fullscreen")) {
      REXCVAR_SET(fullscreen, false);
    }
    // Stated at boot because the game does not query the mask until well into
    // the front end, so the log is the only way to confirm it without playing.
    REXLOG_INFO("license_mask = {} ({})", REXCVAR_GET(license_mask),
                REXCVAR_GET(license_mask) & 1u ? "full version" : "TRIAL");
    rex::ReXApp::OnPreSetup(config);
  }

  // Each mod keeps its own saves; see src/mod_saves.cpp for why sharing them
  // would be worse than it sounds. Here rather than anywhere else because this
  // runs after both the defaults and the command line have been read and
  // before anything opens a save, so it covers however the game was started.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // The executable sits in the game folder, so with nothing said it plays
    // what is beside it. That is what makes a staged game a folder rather
    // than a command line, and what lets a mod be reached by pointing the
    // same executable at mods\<name> instead.
    const std::filesystem::path home = ExeDir();
    if (paths.game_data_root.empty() && !home.empty()) {
      paths.game_data_root = home;
    }
    std::error_code ec;
    if (paths.metadata_root.empty() && !home.empty() &&
        std::filesystem::exists(home / "metadata", ec)) {
      paths.metadata_root = home / "metadata";
    }
    paths.user_data_root =
        NbaSaveRootFor(paths.user_data_root, paths.game_data_root);
    // And this is the only place that knows which root won, which is what a
    // swap needs to find the list of mods and its place in it.
    NbaModSwapUseRoot(paths.game_data_root);
    rex::ReXApp::OnConfigurePaths(paths);
  }

  // The window is named after the executable unless something says otherwise.
  void OnPostSetup() override {
    // The chooser reads the controller itself and needs to be able to hold
    // the game's own input still while it does; see src/mod_picker.cpp.
    NbaModPickerUseInput(
        static_cast<rex::input::InputSystem*>(runtime()->input_system()));
    // Said here rather than where it is decided, because paths are resolved
    // before logging exists.
    REXLOG_INFO("saves: {}{}", user_data_root().string(),
                NbaSavePath(game_data_root()).empty()
                    ? " (shared; this root names no mod)" : "");
    if (window()) {
      window()->SetTitle(NbaWindowTitle());
      // A swap replaces the process, and a new process starts behind the one
      // it replaced unless it is told otherwise. See src/mod_swap.cpp.
      NbaModSwapTakeForeground(window()->GetNativeWindowHandle());
    }
    rex::ReXApp::OnPostSetup();
  }

  // The mod list is drawn in the game's own typefaces, which tools/dlc.py
  // unpacks out of the front end's font archive into a folder beside the
  // executable. Here because the atlas is built once, before any dialog.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    NbaModPickerFonts(atlas);
    rex::ReXApp::OnConfigureFonts(atlas);
  }

  // And from here on it carries the frame rate; see src/window_title.cpp for
  // why the count is taken from ImGui rather than from the game.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    NbaTrackFrameRateInTitle(drawer, window());
    NbaModPickerCreate(drawer);
    rex::ReXApp::OnCreateDialogs(drawer);
  }
};
