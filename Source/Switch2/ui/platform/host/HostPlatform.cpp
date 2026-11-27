// SPDX-License-Identifier: MIT
#include "platform/host/HostPlatform.h"

#include <chrono>
#include <cstring>

#include "platform/Log.h"
#include "render/vulkan/Target.h"
#include "render/vulkan/VulkanRenderer.h"

namespace ui {

void HostScriptedInput::Poll(std::vector<Event>& Out) {
  if (D.SizeSerial() != LastSerial) {
    LastSerial = D.SizeSerial();
    Event E;
    E.Kind = Event::Type::SizeChanged;
    E.W = D.GetSize().W;
    E.H = D.GetSize().H;
    Out.push_back(E);
  }
  while (!Queue.empty()) {
    Out.push_back(Queue.front());
    Queue.pop_front();
  }
}

uint64_t HostSystem::MonotonicMs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
bool HostSystem::WallClock(WallTime& Out) {
  Out = WallTime {2026, 1, 1, 12, 0, 0};
  return true;
}
bool HostSystem::FreeSpace(const char*, uint64_t& FreeBytes, uint64_t& TotalBytes) {
  FreeBytes = 12ull << 30;
  TotalBytes = 64ull << 30;
  return true;
}
bool HostSystem::NetworkAddress(std::string& Dotted) {
  Dotted = "192.168.1.50";
  return true;
}
bool HostSystem::Battery(int& Percent, bool& Charging) {
  Percent = 80;
  Charging = false;
  return true;
}

namespace {

// Offscreen RGBA8 image with a host-visible readback buffer.
class HostTarget final : public vk::Target {
public:
  HostTarget(int W, int H) : Want {W, H} {}
  ~HostTarget() override { Close(); }

  Size Want;
  VkImage Image = VK_NULL_HANDLE;
  VkDeviceMemory ImageMemory = VK_NULL_HANDLE;
  VkBuffer Readback = VK_NULL_HANDLE;
  VkDeviceMemory ReadbackMemory = VK_NULL_HANDLE;
  void* ReadbackMapped = nullptr;

protected:
  const char* AppName() const override { return "fex-switch2-ui-host"; }
  std::vector<const char*> InstanceExtensions() const override { return {}; }
  std::vector<const char*> DeviceExtensions() const override { return {}; }
  int ScoreDevice(const VkPhysicalDeviceProperties& P, uint32_t, uint32_t) const override {
    switch (P.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 100;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 50;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 20;
    default: return 10;
    }
  }
  bool UsesSemaphores() const override { return false; }
  VkImageLayout FinalLayout() const override { return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }

  uint32_t FindType(uint32_t Allowed, VkMemoryPropertyFlags Wanted) const {
    for (uint32_t I = 0; I < MemoryProps.memoryTypeCount; ++I)
      if ((Allowed & (1u << I)) && (MemoryProps.memoryTypes[I].propertyFlags & Wanted) == Wanted)
        return I;
    return UINT32_MAX;
  }

  bool CreateImages() override {
    DestroyImages();
    Format = VK_FORMAT_R8G8B8A8_UNORM;
    Extent = {uint32_t(Want.W), uint32_t(Want.H)};
    VkImageCreateInfo Info {};
    Info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    Info.imageType = VK_IMAGE_TYPE_2D;
    Info.format = Format;
    Info.extent = {Extent.width, Extent.height, 1};
    Info.mipLevels = 1;
    Info.arrayLayers = 1;
    Info.samples = VK_SAMPLE_COUNT_1_BIT;
    Info.tiling = VK_IMAGE_TILING_OPTIMAL;
    Info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vk::Step("vkCreateImage(offscreen)", vkCreateImage(Device, &Info, nullptr, &Image)))
      return false;
    VkMemoryRequirements Req;
    vkGetImageMemoryRequirements(Device, Image, &Req);
    uint32_t Type = FindType(Req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (Type == UINT32_MAX)
      Type = FindType(Req.memoryTypeBits, 0);
    VkMemoryAllocateInfo Alloc {};
    Alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    Alloc.allocationSize = Req.size;
    Alloc.memoryTypeIndex = Type;
    if (!vk::Step("vkAllocateMemory(offscreen)", vkAllocateMemory(Device, &Alloc, nullptr, &ImageMemory)) ||
        !vk::Step("vkBindImageMemory(offscreen)", vkBindImageMemory(Device, Image, ImageMemory, 0)))
      return false;
    VkBufferCreateInfo BufInfo {};
    BufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BufInfo.size = VkDeviceSize(Extent.width) * Extent.height * 4;
    BufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk::Step("vkCreateBuffer(readback)", vkCreateBuffer(Device, &BufInfo, nullptr, &Readback)))
      return false;
    vkGetBufferMemoryRequirements(Device, Readback, &Req);
    Type = FindType(Req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (Type == UINT32_MAX) {
      Log("FAIL readback memory: no host-visible coherent type");
      return false;
    }
    Alloc.allocationSize = Req.size;
    Alloc.memoryTypeIndex = Type;
    if (!vk::Step("vkAllocateMemory(readback)", vkAllocateMemory(Device, &Alloc, nullptr, &ReadbackMemory)) ||
        !vk::Step("vkBindBufferMemory(readback)", vkBindBufferMemory(Device, Readback, ReadbackMemory, 0)) ||
        !vk::Step("vkMapMemory(readback)", vkMapMemory(Device, ReadbackMemory, 0, BufInfo.size, 0, &ReadbackMapped)))
      return false;
    Images = {Image};
    return true;
  }

  void DestroyImages() override {
    if (ReadbackMapped)
      vkUnmapMemory(Device, ReadbackMemory);
    if (Readback)
      vkDestroyBuffer(Device, Readback, nullptr);
    if (ReadbackMemory)
      vkFreeMemory(Device, ReadbackMemory, nullptr);
    if (Image)
      vkDestroyImage(Device, Image, nullptr);
    if (ImageMemory)
      vkFreeMemory(Device, ImageMemory, nullptr);
    ReadbackMapped = nullptr;
    Readback = VK_NULL_HANDLE;
    ReadbackMemory = VK_NULL_HANDLE;
    Image = VK_NULL_HANDLE;
    ImageMemory = VK_NULL_HANDLE;
    Images.clear();
  }

  VkResult DoAcquire(uint32_t& Index) override {
    Index = 0;
    return VK_SUCCESS;
  }
  VkResult DoPresent(uint32_t) override { return vkWaitForFences(Device, 1, &FrameFence, VK_TRUE, UINT64_MAX); }

  void RecordAfterDraw(VkCommandBuffer Cmd, uint32_t) override {
    VkBufferImageCopy Region {};
    Region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    Region.imageExtent = {Extent.width, Extent.height, 1};
    vkCmdCopyImageToBuffer(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Readback, 1, &Region);
    VkMemoryBarrier Barrier {};
    Barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    Barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &Barrier, 0, nullptr, 0, nullptr);
  }
};

class HostDisplay final : public Display {
public:
  HostDisplay(int W, int H) : Tgt(W, H) {}
  bool Open() { return Tgt.Open(); }
  Size GetSize() const override { return Size {int(Tgt.Extent.width), int(Tgt.Extent.height)}; }
  uint32_t SizeSerial() const override { return Tgt.SizeSerial; }
  bool BeginFrame() override { return Tgt.BeginFrame(); }
  bool Present() override { return Tgt.PresentImage(); }
  bool SetVsync(bool On) override { return !On; }  // no display: never vsynced
  bool Vsync() const override { return false; }
  bool ReadPixels(std::vector<uint8_t>& Rgba, Size& OutSize) override {
    if (!Tgt.ReadbackMapped)
      return false;
    OutSize = GetSize();
    Rgba.assign(static_cast<uint8_t*>(Tgt.ReadbackMapped), static_cast<uint8_t*>(Tgt.ReadbackMapped) + size_t(OutSize.W) * size_t(OutSize.H) * 4);
    return true;
  }
  void SetSize(int W, int H) {
    Tgt.Want = Size {W, H};
    Tgt.Recreate = true;
  }
  HostTarget Tgt;
};

class HostPlatformImpl final : public HostPlatform {
public:
  HostPlatformImpl(int W, int H) : Disp(W, H), In(Disp) {}
  ~HostPlatformImpl() override { Rend.reset(); }
  bool Open() {
    if (!Disp.Open())
      return false;
    Rend = CreateVulkanRenderer(Disp.Tgt);
    return Rend != nullptr;
  }
  Display& GetDisplay() override { return Disp; }
  Input& GetInput() override { return In; }
  System& GetSystem() override { return Sys; }
  Renderer& GetRenderer() override { return *Rend; }
  void SetSize(int W, int H) override { Disp.SetSize(W, H); }
  HostScriptedInput& ScriptedInput() override { return In; }

private:
  HostDisplay Disp;
  HostScriptedInput In;
  HostSystem Sys;
  std::unique_ptr<Renderer> Rend;
};

} // namespace

std::unique_ptr<HostPlatform> CreateHostPlatform(const PlatformOptions& Options) {
  Log("platform: host headless Vulkan, %dx%d", Options.Width, Options.Height);
  auto P = std::make_unique<HostPlatformImpl>(Options.Width, Options.Height);
  if (!P->Open())
    return nullptr;
  return P;
}

std::unique_ptr<Platform> CreatePlatform(const PlatformOptions& Options) { return CreateHostPlatform(Options); }

} // namespace ui
