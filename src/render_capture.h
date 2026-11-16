#pragma once

namespace rex::ui {
class ImGuiDrawer;
class Presenter;
}

// Optional, one-shot capture of the game's own rendered image. No desktop APIs.
void NbaRenderCaptureStart(rex::ui::ImGuiDrawer* drawer,
                           rex::ui::Presenter* presenter);
