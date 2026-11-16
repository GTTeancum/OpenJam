#include "graphics_backend.h"

#include <stdexcept>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

REXCVAR_DEFINE_STRING(renderer, OPENJAM_DEFAULT_RENDERER, "GPU",
                     "Graphics backend: auto, d3d12, or vulkan (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

void NbaConfigureGraphicsBackend(rex::RuntimeConfig& config) {
  if (config.graphics || config.gpu_plugin.empty()) return;
  const std::string renderer = REXCVAR_GET(renderer);
  if (renderer != "auto" && renderer != "d3d12" && renderer != "vulkan") {
    throw std::runtime_error("Unknown renderer '" + renderer +
                             "'; choose auto, d3d12, or vulkan.");
  }
  config.graphics = rex::system::LoadGpuPlugin(
      config.gpu_plugin, renderer == "auto" ? "any" : renderer);
  if (!config.graphics) {
    throw std::runtime_error("Renderer '" + renderer +
                             "' could not be loaded. Check the graphics plugin and log.");
  }
  REXLOG_INFO("renderer: {}", renderer);
}
