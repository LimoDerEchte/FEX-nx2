// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "render/Renderer.h"
#include "render/vulkan/Target.h"

namespace ui {

// Vulkan 2D renderer drawing into the target's images. The target must outlive the renderer. Logs every init step.
std::unique_ptr<Renderer> CreateVulkanRenderer(vk::Target& Target);

} // namespace ui
