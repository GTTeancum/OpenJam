#include "render_capture.h"

#include "host.h"

#include <charconv>
#include <fstream>
#include <string>
#include <utility>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/presenter.h>

namespace {
class RenderCapture final : public rex::ui::ImGuiDialog {
 public:
  RenderCapture(rex::ui::ImGuiDrawer* drawer, rex::ui::Presenter* presenter,
                std::filesystem::path path, uint64_t delay)
      : ImGuiDialog(drawer), presenter_(presenter), path_(std::move(path)),
        due_(NbaNowMs() + delay) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    if (NbaNowMs() < due_) return;
    rex::ui::RawImage image;
    if (!presenter_->CaptureGuestOutput(image) || !image.width || !image.height ||
        image.stride < size_t(image.width) * 4 ||
        image.data.size() < (size_t(image.height) - 1) * image.stride +
                                size_t(image.width) * 4) {
      REXLOG_ERROR("render capture: no readable guest image");
      Close();
      return;
    }
    // PPM keeps this diagnostic independent of image encoders and captures
    // actual guest-output dimensions before window scaling or UI overlays.
    std::ofstream out(path_, std::ios::binary);
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (uint32_t y = 0; y < image.height && out; ++y) {
      const uint8_t* row = image.data.data() + size_t(y) * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) {
        out.write(reinterpret_cast<const char*>(row + size_t(x) * 4), 3);
      }
    }
    out.close();
    if (out) {
      REXLOG_INFO("render capture: {} x {} -> {}", image.width, image.height,
                  path_.string());
    } else {
      REXLOG_ERROR("render capture: could not write {}", path_.string());
    }
    Close();
  }

 private:
  rex::ui::Presenter* presenter_;
  std::filesystem::path path_;
  uint64_t due_;
};
}  // namespace

void NbaRenderCaptureStart(rex::ui::ImGuiDrawer* drawer,
                           rex::ui::Presenter* presenter) {
  const std::string path = NbaEnv("NBAJAM_CAPTURE_PATH");
  if (path.empty() || !drawer || !presenter) return;
  uint64_t delay = 60000;
  const std::string timing = NbaEnv("NBAJAM_CAPTURE_AFTER_MS");
  if (!timing.empty()) {
    const auto [end, error] =
        std::from_chars(timing.data(), timing.data() + timing.size(), delay);
    if (error != std::errc{} || end != timing.data() + timing.size() || delay > 180000) {
      REXLOG_ERROR("render capture: delay must be 0..180000 milliseconds");
      return;
    }
  }
  new RenderCapture(drawer, presenter, NbaPathFromUtf8(path), delay);
}
