// nbajam_ofe - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "window_title.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>

class NbajamOfeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

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
    if (REXCVAR_GET(license_mask) == 0u) {
      REXCVAR_SET(license_mask, 1u);
    }
    // Stated at boot because the game does not query the mask until well into
    // the front end, so the log is the only way to confirm it without playing.
    REXLOG_INFO("license_mask = {} ({})", REXCVAR_GET(license_mask),
                REXCVAR_GET(license_mask) & 1u ? "full version" : "TRIAL");
    rex::ReXApp::OnPreSetup(config);
  }

  // The window is named after the executable unless something says otherwise.
  void OnPostSetup() override {
    if (window()) {
      window()->SetTitle(NbaWindowTitle());
    }
    rex::ReXApp::OnPostSetup();
  }

  // And from here on it carries the frame rate; see src/window_title.cpp for
  // why the count is taken from ImGui rather than from the game.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    NbaTrackFrameRateInTitle(drawer, window());
    rex::ReXApp::OnCreateDialogs(drawer);
  }
};
