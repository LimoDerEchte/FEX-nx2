// SPDX-License-Identifier: MIT
// Adapted from ounce-hbmenu switch2/graphics_vulkan.c (ISC, Copyright 2026 nx-hbmenu Switch 2 port contributors).
#include "render/vulkan/Target.h"

#include <algorithm>
#include <cstring>

#include "platform/Log.h"

namespace ui::vk {

const char* ResultName(VkResult R) {
  switch (R) {
#define CASE(X) case X: return #X
    CASE(VK_SUCCESS);
    CASE(VK_NOT_READY);
    CASE(VK_TIMEOUT);
    CASE(VK_INCOMPLETE);
    CASE(VK_SUBOPTIMAL_KHR);
    CASE(VK_ERROR_OUT_OF_HOST_MEMORY);
    CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CASE(VK_ERROR_INITIALIZATION_FAILED);
    CASE(VK_ERROR_DEVICE_LOST);
    CASE(VK_ERROR_MEMORY_MAP_FAILED);
    CASE(VK_ERROR_LAYER_NOT_PRESENT);
    CASE(VK_ERROR_EXTENSION_NOT_PRESENT);
    CASE(VK_ERROR_FEATURE_NOT_PRESENT);
    CASE(VK_ERROR_INCOMPATIBLE_DRIVER);
    CASE(VK_ERROR_TOO_MANY_OBJECTS);
    CASE(VK_ERROR_FORMAT_NOT_SUPPORTED);
    CASE(VK_ERROR_FRAGMENTED_POOL);
    CASE(VK_ERROR_UNKNOWN);
    CASE(VK_ERROR_SURFACE_LOST_KHR);
    CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
    CASE(VK_ERROR_OUT_OF_DATE_KHR);
#undef CASE
  default: return "VkResult(unknown)";
  }
}

bool Step(const char* Name, VkResult R) {
  if (R == VK_SUCCESS) {
    Log("step %s: ok", Name);
    return true;
  }
  Log("FAIL %s: %s (%d)", Name, ResultName(R), int(R));
  return false;
}

Target::~Target() {}

bool Target::ThrottleFailure() {
  ++FailureCount;
  return FailureCount <= 8 || FailureCount % 300 == 0;
}

bool Target::Open() {
  // Instance.
  {
    std::vector<const char*> Exts = InstanceExtensions();
    VkApplicationInfo App {};
    App.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    App.pApplicationName = AppName();
    App.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo Info {};
    Info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    Info.pApplicationInfo = &App;
    Info.enabledExtensionCount = uint32_t(Exts.size());
    Info.ppEnabledExtensionNames = Exts.data();
    if (!Step("vkCreateInstance", vkCreateInstance(&Info, nullptr, &Instance)))
      return false;
  }
  // Physical device.
  {
    VkPhysicalDevice Devices[8];
    uint32_t Count = 8;
    VkResult R = vkEnumeratePhysicalDevices(Instance, &Count, Devices);
    if (R != VK_SUCCESS && R != VK_INCOMPLETE) {
      Step("vkEnumeratePhysicalDevices", R);
      return false;
    }
    Log("step vkEnumeratePhysicalDevices: ok (%u device%s)", Count, Count == 1 ? "" : "s");
    int Best = -1, BestScore = -1;
    for (uint32_t I = 0; I < Count; ++I) {
      VkPhysicalDeviceProperties Props;
      vkGetPhysicalDeviceProperties(Devices[I], &Props);
      int Score = ScoreDevice(Props, I, Count);
      Log("  device %u: %s type=%d api=%u.%u.%u score=%d", I, Props.deviceName, int(Props.deviceType), VK_VERSION_MAJOR(Props.apiVersion),
          VK_VERSION_MINOR(Props.apiVersion), VK_VERSION_PATCH(Props.apiVersion), Score);
      if (Score > BestScore) {
        BestScore = Score;
        Best = int(I);
      }
    }
    if (Best < 0) {
      Log("FAIL pick physical device: none usable");
      return false;
    }
    Physical = Devices[Best];
    vkGetPhysicalDeviceMemoryProperties(Physical, &MemoryProps);
    VkQueueFamilyProperties Families[16];
    uint32_t FamilyCount = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(Physical, &FamilyCount, Families);
    uint32_t Family = FamilyCount;
    for (uint32_t I = 0; I < FamilyCount; ++I)
      if (Families[I].queueCount && (Families[I].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        Family = I;
        break;
      }
    if (Family == FamilyCount) {
      Log("FAIL pick queue family: no graphics queue");
      return false;
    }
    QueueFamily = Family;
    Log("step pick device: ok (device %d, queue family %u, %u memory types)", Best, Family, MemoryProps.memoryTypeCount);
  }
  // Device.
  {
    const float Priority = 1.0f;
    VkDeviceQueueCreateInfo QueueInfo {};
    QueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    QueueInfo.queueFamilyIndex = QueueFamily;
    QueueInfo.queueCount = 1;
    QueueInfo.pQueuePriorities = &Priority;
    std::vector<const char*> Exts = DeviceExtensions();
    VkDeviceCreateInfo Info {};
    Info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    Info.queueCreateInfoCount = 1;
    Info.pQueueCreateInfos = &QueueInfo;
    Info.enabledExtensionCount = uint32_t(Exts.size());
    Info.ppEnabledExtensionNames = Exts.data();
    if (!Step("vkCreateDevice", vkCreateDevice(Physical, &Info, nullptr, &Device)))
      return false;
    vkGetDeviceQueue(Device, QueueFamily, 0, &Queue);
    if (!Queue) {
      Log("FAIL vkGetDeviceQueue: null queue");
      return false;
    }
    Log("step vkGetDeviceQueue: ok");
  }
  // Images (surface and swapchain, or the offscreen image).
  if (!CreateImages())
    return false;
  LastExtent = Extent;
  // Render pass.
  {
    VkAttachmentDescription Attachment {};
    Attachment.format = Format;
    Attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    Attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    Attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    Attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    Attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    Attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Attachment.finalLayout = FinalLayout();
    VkAttachmentReference Color {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription Subpass {};
    Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    Subpass.colorAttachmentCount = 1;
    Subpass.pColorAttachments = &Color;
    VkSubpassDependency Deps[2] {};
    // The acquire semaphore is waited at colour output; the layout transition must not start before it.
    Deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    Deps[0].dstSubpass = 0;
    Deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    Deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    Deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // Offscreen readback copies after the pass.
    Deps[1].srcSubpass = 0;
    Deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    Deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    Deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    Deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    Deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo Info {};
    Info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    Info.attachmentCount = 1;
    Info.pAttachments = &Attachment;
    Info.subpassCount = 1;
    Info.pSubpasses = &Subpass;
    Info.dependencyCount = 2;
    Info.pDependencies = Deps;
    if (!Step("vkCreateRenderPass", vkCreateRenderPass(Device, &Info, nullptr, &RenderPass)))
      return false;
  }
  if (!CreateFramebuffers())
    return false;
  // Sync objects and command pool.
  {
    VkSemaphoreCreateInfo SemInfo {};
    SemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (UsesSemaphores()) {
      if (!Step("vkCreateSemaphore(acquired)", vkCreateSemaphore(Device, &SemInfo, nullptr, &Acquired)))
        return false;
      Ready.assign(16, VK_NULL_HANDLE);
      for (auto& S : Ready)
        if (vkCreateSemaphore(Device, &SemInfo, nullptr, &S) != VK_SUCCESS) {
          Log("FAIL vkCreateSemaphore(ready): failed");
          return false;
        }
      Log("step vkCreateSemaphore(ready x16): ok");
    }
    VkFenceCreateInfo FenceInfo {};
    FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!Step("vkCreateFence", vkCreateFence(Device, &FenceInfo, nullptr, &FrameFence)))
      return false;
    VkCommandPoolCreateInfo PoolInfo {};
    PoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    PoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    PoolInfo.queueFamilyIndex = QueueFamily;
    if (!Step("vkCreateCommandPool", vkCreateCommandPool(Device, &PoolInfo, nullptr, &CommandPool)))
      return false;
  }
  Opened = true;
  return true;
}

void Target::DestroyFramebuffers() {
  for (VkFramebuffer F : Framebuffers)
    if (F)
      vkDestroyFramebuffer(Device, F, nullptr);
  Framebuffers.clear();
  for (VkImageView V : Views)
    if (V)
      vkDestroyImageView(Device, V, nullptr);
  Views.clear();
}

bool Target::CreateFramebuffers() {
  Views.assign(Images.size(), VK_NULL_HANDLE);
  Framebuffers.assign(Images.size(), VK_NULL_HANDLE);
  for (size_t I = 0; I < Images.size(); ++I) {
    VkImageViewCreateInfo ViewInfo {};
    ViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ViewInfo.image = Images[I];
    ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ViewInfo.format = Format;
    ViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkResult R = vkCreateImageView(Device, &ViewInfo, nullptr, &Views[I]);
    if (R != VK_SUCCESS) {
      Step("vkCreateImageView", R);
      return false;
    }
    VkFramebufferCreateInfo Info {};
    Info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    Info.renderPass = RenderPass;
    Info.attachmentCount = 1;
    Info.pAttachments = &Views[I];
    Info.width = Extent.width;
    Info.height = Extent.height;
    Info.layers = 1;
    R = vkCreateFramebuffer(Device, &Info, nullptr, &Framebuffers[I]);
    if (R != VK_SUCCESS) {
      Step("vkCreateFramebuffer", R);
      return false;
    }
  }
  Log("step framebuffers: ok (%zu at %ux%u)", Images.size(), Extent.width, Extent.height);
  return true;
}

bool Target::RecreateImages() {
  vkDeviceWaitIdle(Device);
  DestroyFramebuffers();
  bool Ok = CreateImages() && CreateFramebuffers();
  Recreate = false;
  if (Ok && (Extent.width != LastExtent.width || Extent.height != LastExtent.height)) {
    Log("size change: %ux%u -> %ux%u", LastExtent.width, LastExtent.height, Extent.width, Extent.height);
    LastExtent = Extent;
    ++SizeSerial;
  } else if (Ok) {
    Log("swapchain recreated: size unchanged %ux%u", Extent.width, Extent.height);
  }
  if (!Ok)
    Log("FAIL recreate images");
  return Ok;
}

bool Target::BeginFrame() {
  if (!Opened)
    return false;
  VkResult R = vkWaitForFences(Device, 1, &FrameFence, VK_TRUE, UINT64_MAX);
  if (R != VK_SUCCESS) {
    if (ThrottleFailure())
      Step("vkWaitForFences", R);
    return false;
  }
  HasPending = false;
  if (Recreate)
    return RecreateImages();
  return true;
}

bool Target::AcquireImage(uint32_t& Index) {
  VkResult R = DoAcquire(Index);
  if (R == VK_ERROR_OUT_OF_DATE_KHR) {
    Log("acquire: out of date, recreating");
    if (!RecreateImages())
      return false;
    R = DoAcquire(Index);
  }
  if (R == VK_SUBOPTIMAL_KHR) {
    Recreate = true;
    R = VK_SUCCESS;
  }
  if (R != VK_SUCCESS) {
    if (ThrottleFailure())
      Step("vkAcquireNextImageKHR", R);
    return false;
  }
  return true;
}

bool Target::Submit(VkCommandBuffer Cmd, uint32_t Index) {
  VkResult R = vkResetFences(Device, 1, &FrameFence);
  if (R != VK_SUCCESS) {
    if (ThrottleFailure())
      Step("vkResetFences", R);
    return false;
  }
  const VkPipelineStageFlags WaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo Submit {};
  Submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  if (UsesSemaphores()) {
    Submit.waitSemaphoreCount = 1;
    Submit.pWaitSemaphores = &Acquired;
    Submit.pWaitDstStageMask = &WaitStage;
    Submit.signalSemaphoreCount = 1;
    Submit.pSignalSemaphores = &Ready[Index];
  }
  Submit.commandBufferCount = 1;
  Submit.pCommandBuffers = &Cmd;
  R = vkQueueSubmit(Queue, 1, &Submit, FrameFence);
  if (R != VK_SUCCESS) {
    if (ThrottleFailure())
      Step("vkQueueSubmit", R);
    // The fence was reset and will never signal: replace it with a signaled one so the next BeginFrame does not hang.
    vkDestroyFence(Device, FrameFence, nullptr);
    VkFenceCreateInfo FenceInfo {};
    FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(Device, &FenceInfo, nullptr, &FrameFence);
    return false;
  }
  ++SubmittedFrames;
  PendingIndex = Index;
  HasPending = true;
  return true;
}

bool Target::PresentImage() {
  if (!HasPending)
    return false;
  HasPending = false;
  VkResult R = DoPresent(PendingIndex);
  if (R == VK_SUBOPTIMAL_KHR || R == VK_ERROR_OUT_OF_DATE_KHR) {
    Recreate = true;
    R = VK_SUCCESS;
  }
  if (R != VK_SUCCESS) {
    if (ThrottleFailure())
      Step("present", R);
    return false;
  }
  AfterPresent(PendingIndex);
  return true;
}

void Target::Close() {
  if (Device) {
    vkDeviceWaitIdle(Device);
    if (FrameFence)
      vkDestroyFence(Device, FrameFence, nullptr);
    if (Acquired)
      vkDestroySemaphore(Device, Acquired, nullptr);
    for (VkSemaphore S : Ready)
      if (S)
        vkDestroySemaphore(Device, S, nullptr);
    if (CommandPool)
      vkDestroyCommandPool(Device, CommandPool, nullptr);
    DestroyFramebuffers();
    if (RenderPass)
      vkDestroyRenderPass(Device, RenderPass, nullptr);
    DestroyImages();
    vkDestroyDevice(Device, nullptr);
  }
  if (Instance)
    vkDestroyInstance(Instance, nullptr);
  FrameFence = VK_NULL_HANDLE;
  Acquired = VK_NULL_HANDLE;
  Ready.clear();
  CommandPool = VK_NULL_HANDLE;
  RenderPass = VK_NULL_HANDLE;
  Device = VK_NULL_HANDLE;
  Instance = VK_NULL_HANDLE;
  Opened = false;
}

} // namespace ui::vk
