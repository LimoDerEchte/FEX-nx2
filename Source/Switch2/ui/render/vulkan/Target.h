// SPDX-License-Identifier: MIT
// Vulkan render target shared by the console and host backends (only ui/platform/ and ui/render/ include Vulkan).
// Adapted from ounce-hbmenu switch2/graphics_vulkan.c (ISC, Copyright 2026 nx-hbmenu Switch 2 port contributors):
// instance, device, render pass, per-image framebuffers, one frame in flight (fence), acquire and present with recreate
// on a size change. A backend derives from Target and supplies the images (swapchain on the console, one offscreen
// image on the host).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace ui::vk {

const char* ResultName(VkResult R);
// Logs "step <Name>: ok" or "FAIL <Name>: <VkResult name> (<value>)". Returns R == VK_SUCCESS.
bool Step(const char* Name, VkResult R);

class Target {
public:
  virtual ~Target();

  // Backend commands recorded after the render pass in the frame's command buffer (host: copy the image for readback).
  virtual void RecordAfterDraw(VkCommandBuffer Cmd, uint32_t Index) { (void)Cmd; (void)Index; }

  VkInstance Instance = VK_NULL_HANDLE;
  VkPhysicalDevice Physical = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties MemoryProps {};
  VkDevice Device = VK_NULL_HANDLE;
  VkQueue Queue = VK_NULL_HANDLE;
  uint32_t QueueFamily = 0;

  VkFormat Format = VK_FORMAT_UNDEFINED;
  VkExtent2D Extent {};
  VkRenderPass RenderPass = VK_NULL_HANDLE;
  VkCommandPool CommandPool = VK_NULL_HANDLE;
  VkFence FrameFence = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> Framebuffers;

  uint64_t SubmittedFrames = 0;  // frames submitted so far
  uint32_t SizeSerial = 0;       // incremented on every extent change after the first
  bool Recreate = false;         // the output size changed: images are rebuilt at the next BeginFrame

  // Creates instance, device, images, render pass, framebuffers, sync objects and the command pool; logs every step.
  bool Open();
  // Waits for the previous frame and applies a pending recreate. false = unusable.
  bool BeginFrame();
  // Acquires the image to draw into (recreates and retries once if the old swapchain is out of date).
  bool AcquireImage(uint32_t& Index);
  // Submits the recorded command buffer for image Index and remembers it for Present.
  bool Submit(VkCommandBuffer Cmd, uint32_t Index);
  // Presents the submitted image. A suboptimal or out-of-date result schedules a recreate and still counts as success.
  bool PresentImage();
  // Closes everything (idempotent). Derived destructors call it first.
  void Close();

  // Log a failure at most a few times, then every 300th occurrence (a per-frame failure must not flood the log).
  bool ThrottleFailure();


protected:
  // Backend hooks.
  virtual const char* AppName() const = 0;
  virtual std::vector<const char*> InstanceExtensions() const = 0;
  virtual std::vector<const char*> DeviceExtensions() const = 0;
  // Higher wins; negative = unusable.
  virtual int ScoreDevice(const VkPhysicalDeviceProperties& Props, uint32_t Index, uint32_t Count) const = 0;
  // Creates (or recreates, replacing the old swapchain) the images, and sets Format, Extent and Images.
  virtual bool CreateImages() = 0;
  virtual void DestroyImages() = 0;  // everything CreateImages made, including the surface when Final
  virtual VkResult DoAcquire(uint32_t& Index) = 0;
  virtual VkResult DoPresent(uint32_t Index) = 0;
  virtual bool UsesSemaphores() const = 0;  // acquire/ready semaphores around submit (swapchain)
  virtual VkImageLayout FinalLayout() const = 0;
  virtual void AfterPresent(uint32_t Index) { (void)Index; }

  std::vector<VkImage> Images;
  std::vector<VkImageView> Views;
  VkSemaphore Acquired = VK_NULL_HANDLE;
  std::vector<VkSemaphore> Ready;
  VkExtent2D LastExtent {};
  uint32_t PendingIndex = 0;
  bool HasPending = false;
  bool Opened = false;
  uint32_t FailureCount = 0;

  bool RecreateImages();
  void DestroyFramebuffers();
  bool CreateFramebuffers();
};

} // namespace ui::vk
