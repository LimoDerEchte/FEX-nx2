// SPDX-License-Identifier: ISC
// Copyright 2026 nx-hbmenu Switch 2 port contributors (texture, upload and draw-group handling adapted from ounce-hbmenu
// switch2/graphics_vulkan.c); shape pipeline and Renderer interface for the FEX Switch 2 launcher UI.
//
// One pipeline draws everything as quads: textured quads (RGBA or alpha-only textures) and SDF shapes (filled or stroked
// rounded boxes, so rects, rounded rects, circles and rings). Consecutive quads with the same texture and scissor are one
// draw. One frame is in flight: BeginFrame waits for the previous frame, so the vertex buffers, staging buffers and
// released textures can be reused or destroyed without tracking more than one submission.
#include "render/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "platform/Log.h"

#include "ui_vert.h"
#include "ui_frag.h"

namespace ui {
namespace {

using vk::Step;

constexpr uint32_t MaxTextures = 1024;
constexpr uint32_t InitialQuadCapacity = 16384;

struct Vertex {
  float X, Y;        // device pixels
  float U, V;        // normalised texture coordinates
  float Lx, Ly;      // shapes: pixels from the shape centre
  float Hw, Hh, Radius, Stroke;
  float Mode;        // 0 textured, 1 filled shape, 2 stroked shape
  uint32_t Color;    // R8G8B8A8, R in the low byte
};
static_assert(sizeof(Vertex) == 48, "vertex layout is described to the pipeline");

struct IRect {
  int X = 0, Y = 0, W = 0, H = 0;
  bool operator==(const IRect& O) const { return X == O.X && Y == O.Y && W == O.W && H == O.H; }
};

struct HostBuffer {
  VkBuffer Buffer = VK_NULL_HANDLE;
  VkDeviceMemory Memory = VK_NULL_HANDLE;
  void* Mapped = nullptr;
  VkDeviceSize Size = 0;
};

struct Texture {
  VkImage Image = VK_NULL_HANDLE;
  VkDeviceMemory Memory = VK_NULL_HANDLE;
  VkImageView View = VK_NULL_HANDLE;
  VkDescriptorSet Set = VK_NULL_HANDLE;
  int W = 0, H = 0;
  uint32_t TexelBytes = 4;
  bool Used = false;
  bool Initialized = false;  // in SHADER_READ_ONLY_OPTIMAL
  bool Uploading = false;    // in this frame's upload list
  bool Released = false;
};

struct Upload {
  TextureId Tex;
  HostBuffer Staging;
  uint32_t X, Y, W, H;
};

struct DrawGroup {
  TextureId Tex;
  IRect Scissor;
  uint32_t FirstQuad;
  uint32_t QuadCount;
};

uint32_t PackColor(Color C) { return uint32_t(C.R) | uint32_t(C.G) << 8 | uint32_t(C.B) << 16 | uint32_t(C.A) << 24; }

class VulkanRenderer final : public Renderer {
public:
  explicit VulkanRenderer(vk::Target& T) : T(T) {}
  ~VulkanRenderer() override { Shutdown(); }

  bool Init();

  Size TargetSize() const override { return Size {int(T.Extent.width), int(T.Extent.height)}; }

  bool BeginFrame(Color Clear) override;
  bool EndFrame() override;
  void FillRect(Rect R, Color C) override { AddShape(R, 0.0f, 0.0f, 1.0f, C); }
  void FillRoundRect(Rect R, float Radius, Color C) override { AddShape(R, Radius, 0.0f, 1.0f, C); }
  void StrokeRoundRect(Rect R, float Radius, float Thickness, Color C) override { AddShape(R, Radius, Thickness, 2.0f, C); }
  bool Line(float X0, float Y0, float X1, float Y1, float Thickness, Color C) override;
  void Image(TextureId Tex, Rect Dst, Rect Src, Color Tint) override;
  void PushClip(Rect R) override;
  void PopClip() override;
  TextureId CreateTexture(int W, int H, TextureFormat Format, TextureFilter Filter) override;
  bool UpdateTexture(TextureId Tex, int X, int Y, int W, int H, const void* Pixels, size_t StrideBytes) override;
  void DestroyTexture(TextureId Tex) override;

private:
  vk::Target& T;
  VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
  VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
  VkSampler Nearest = VK_NULL_HANDLE, Linear = VK_NULL_HANDLE;
  VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
  VkPipeline Pipeline = VK_NULL_HANDLE;
  VkCommandBuffer Cmd = VK_NULL_HANDLE;
  HostBuffer VertexBuffer, IndexBuffer;
  uint32_t QuadCapacity = 0;

  std::vector<Texture> Textures;
  TextureId White = NoTexture;
  std::vector<Upload> Pending, InFlight;
  std::vector<Vertex> Vertices;
  std::vector<DrawGroup> Groups;
  std::vector<IRect> ClipStack;
  IRect Clip;
  Color ClearColor;
  bool InFrame = false;
  bool Ready = false;

  void Shutdown();
  bool FindMemoryType(uint32_t Allowed, VkMemoryPropertyFlags Wanted, uint32_t& Out) const;
  VkResult AllocateMemory(const VkMemoryRequirements& Req, VkMemoryPropertyFlags Wanted, VkMemoryPropertyFlags Fallback, VkDeviceMemory& Out);
  void DestroyHostBuffer(HostBuffer& B);
  VkResult CreateHostBuffer(VkDeviceSize Size, VkBufferUsageFlags Usage, HostBuffer& Out);
  VkResult CreateQuadBuffers(uint32_t Capacity);
  VkResult InitTexture(Texture& Tex, int W, int H, TextureFormat Format, TextureFilter Filter);
  void DestroyTextureResources(Texture& Tex);
  Texture* TextureFor(TextureId Id) { return Id >= 0 && size_t(Id) < Textures.size() && Textures[size_t(Id)].Used ? &Textures[size_t(Id)] : nullptr; }
  bool CreatePipeline();
  void AddQuad(TextureId Tex, const Vertex Corners[4]);
  void AddShape(Rect R, float Radius, float Stroke, float Mode, Color C);
  void RecordUploads();
  void RecordDraws(uint32_t Index);
  void RetireFinishedFrame();
};

// ---- Memory ---------------------------------------------------------------

bool VulkanRenderer::FindMemoryType(uint32_t Allowed, VkMemoryPropertyFlags Wanted, uint32_t& Out) const {
  for (uint32_t I = 0; I < T.MemoryProps.memoryTypeCount; ++I)
    if ((Allowed & (1u << I)) && (T.MemoryProps.memoryTypes[I].propertyFlags & Wanted) == Wanted) {
      Out = I;
      return true;
    }
  return false;
}

VkResult VulkanRenderer::AllocateMemory(const VkMemoryRequirements& Req, VkMemoryPropertyFlags Wanted, VkMemoryPropertyFlags Fallback,
                                        VkDeviceMemory& Out) {
  uint32_t Type;
  if (!FindMemoryType(Req.memoryTypeBits, Wanted, Type) && !FindMemoryType(Req.memoryTypeBits, Fallback, Type))
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VkMemoryAllocateInfo Info {};
  Info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  Info.allocationSize = Req.size;
  Info.memoryTypeIndex = Type;
  return vkAllocateMemory(T.Device, &Info, nullptr, &Out);
}

void VulkanRenderer::DestroyHostBuffer(HostBuffer& B) {
  if (B.Mapped)
    vkUnmapMemory(T.Device, B.Memory);
  if (B.Buffer)
    vkDestroyBuffer(T.Device, B.Buffer, nullptr);
  if (B.Memory)
    vkFreeMemory(T.Device, B.Memory, nullptr);
  B = HostBuffer {};
}

VkResult VulkanRenderer::CreateHostBuffer(VkDeviceSize Size, VkBufferUsageFlags Usage, HostBuffer& Out) {
  Out = HostBuffer {};
  VkBufferCreateInfo Info {};
  Info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  Info.size = Size;
  Info.usage = Usage;
  Info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult R = vkCreateBuffer(T.Device, &Info, nullptr, &Out.Buffer);
  if (R != VK_SUCCESS)
    return R;
  VkMemoryRequirements Req;
  vkGetBufferMemoryRequirements(T.Device, Out.Buffer, &Req);
  const VkMemoryPropertyFlags Host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  R = AllocateMemory(Req, Host, Host, Out.Memory);
  if (R == VK_SUCCESS)
    R = vkBindBufferMemory(T.Device, Out.Buffer, Out.Memory, 0);
  if (R == VK_SUCCESS)
    R = vkMapMemory(T.Device, Out.Memory, 0, Size, 0, &Out.Mapped);
  if (R != VK_SUCCESS) {
    DestroyHostBuffer(Out);
    return R;
  }
  Out.Size = Size;
  return VK_SUCCESS;
}

VkResult VulkanRenderer::CreateQuadBuffers(uint32_t Capacity) {
  DestroyHostBuffer(VertexBuffer);
  DestroyHostBuffer(IndexBuffer);
  QuadCapacity = 0;
  VkResult R = CreateHostBuffer(VkDeviceSize(Capacity) * 4 * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VertexBuffer);
  if (R != VK_SUCCESS)
    return R;
  R = CreateHostBuffer(VkDeviceSize(Capacity) * 6 * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, IndexBuffer);
  if (R != VK_SUCCESS)
    return R;
  uint32_t* Indices = static_cast<uint32_t*>(IndexBuffer.Mapped);
  for (uint32_t Quad = 0; Quad < Capacity; ++Quad) {
    uint32_t V = Quad * 4;
    Indices[0] = V;
    Indices[1] = V + 1;
    Indices[2] = V + 2;
    Indices[3] = V + 2;
    Indices[4] = V + 3;
    Indices[5] = V;
    Indices += 6;
  }
  QuadCapacity = Capacity;
  return VK_SUCCESS;
}

// ---- Textures -------------------------------------------------------------

void VulkanRenderer::DestroyTextureResources(Texture& Tex) {
  if (Tex.Set)
    vkFreeDescriptorSets(T.Device, DescriptorPool, 1, &Tex.Set);
  if (Tex.View)
    vkDestroyImageView(T.Device, Tex.View, nullptr);
  if (Tex.Image)
    vkDestroyImage(T.Device, Tex.Image, nullptr);
  if (Tex.Memory)
    vkFreeMemory(T.Device, Tex.Memory, nullptr);
  Tex = Texture {};
}

VkResult VulkanRenderer::InitTexture(Texture& Tex, int W, int H, TextureFormat Format, TextureFilter Filter) {
  const bool Alpha = Format == TextureFormat::Alpha8;
  const VkFormat VkFmt = Alpha ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
  Tex.TexelBytes = Alpha ? 1 : 4;
  Tex.W = W;
  Tex.H = H;
  VkImageCreateInfo ImageInfo {};
  ImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ImageInfo.imageType = VK_IMAGE_TYPE_2D;
  ImageInfo.format = VkFmt;
  ImageInfo.extent = {uint32_t(W), uint32_t(H), 1};
  ImageInfo.mipLevels = 1;
  ImageInfo.arrayLayers = 1;
  ImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  ImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  ImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult R = vkCreateImage(T.Device, &ImageInfo, nullptr, &Tex.Image);
  if (R != VK_SUCCESS)
    return R;
  VkMemoryRequirements Req;
  vkGetImageMemoryRequirements(T.Device, Tex.Image, &Req);
  R = AllocateMemory(Req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, Tex.Memory);
  if (R == VK_SUCCESS)
    R = vkBindImageMemory(T.Device, Tex.Image, Tex.Memory, 0);
  if (R != VK_SUCCESS)
    return R;
  const VkComponentMapping Identity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                       VK_COMPONENT_SWIZZLE_IDENTITY};
  const VkComponentMapping Coverage = {VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R};
  VkImageViewCreateInfo ViewInfo {};
  ViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ViewInfo.image = Tex.Image;
  ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ViewInfo.format = VkFmt;
  ViewInfo.components = Alpha ? Coverage : Identity;
  ViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  R = vkCreateImageView(T.Device, &ViewInfo, nullptr, &Tex.View);
  if (R != VK_SUCCESS)
    return R;
  VkDescriptorSetAllocateInfo SetInfo {};
  SetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  SetInfo.descriptorPool = DescriptorPool;
  SetInfo.descriptorSetCount = 1;
  SetInfo.pSetLayouts = &SetLayout;
  R = vkAllocateDescriptorSets(T.Device, &SetInfo, &Tex.Set);
  if (R != VK_SUCCESS)
    return R;
  VkDescriptorImageInfo Descriptor {};
  Descriptor.sampler = Filter == TextureFilter::Linear ? Linear : Nearest;
  Descriptor.imageView = Tex.View;
  Descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet Write {};
  Write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  Write.dstSet = Tex.Set;
  Write.descriptorCount = 1;
  Write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  Write.pImageInfo = &Descriptor;
  vkUpdateDescriptorSets(T.Device, 1, &Write, 0, nullptr);
  return VK_SUCCESS;
}

TextureId VulkanRenderer::CreateTexture(int W, int H, TextureFormat Format, TextureFilter Filter) {
  if (!Ready || W <= 0 || H <= 0)
    return NoTexture;
  TextureId Id = NoTexture;
  for (size_t I = 0; I < Textures.size(); ++I)
    if (!Textures[I].Used) {
      Id = TextureId(I);
      break;
    }
  if (Id == NoTexture) {
    if (Textures.size() >= MaxTextures) {
      Log("FAIL CreateTexture: texture table full (%u)", MaxTextures);
      return NoTexture;
    }
    Id = TextureId(Textures.size());
    Textures.emplace_back();
  }
  Texture& Tex = Textures[size_t(Id)];
  VkResult R = InitTexture(Tex, W, H, Format, Filter);
  if (R != VK_SUCCESS) {
    Step("CreateTexture", R);
    DestroyTextureResources(Tex);
    return NoTexture;
  }
  Tex.Used = true;
  return Id;
}

void VulkanRenderer::DestroyTexture(TextureId Id) {
  if (Texture* Tex = TextureFor(Id))
    Tex->Released = true;  // destroyed in the next BeginFrame, after the previous frame is done
}

bool VulkanRenderer::UpdateTexture(TextureId Id, int X, int Y, int W, int H, const void* Pixels, size_t StrideBytes) {
  Texture* Tex = TextureFor(Id);
  if (!Ready || !Tex || Tex->Released || !Pixels || W <= 0 || H <= 0 || X < 0 || Y < 0 || X + W > Tex->W || Y + H > Tex->H)
    return false;
  const size_t RowBytes = size_t(W) * Tex->TexelBytes;
  if (!StrideBytes)
    StrideBytes = RowBytes;
  Upload Up {};
  Up.Tex = Id;
  Up.X = uint32_t(X);
  Up.Y = uint32_t(Y);
  Up.W = uint32_t(W);
  Up.H = uint32_t(H);
  VkResult R = CreateHostBuffer(RowBytes * size_t(H), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Up.Staging);
  if (R != VK_SUCCESS) {
    Step("UpdateTexture staging", R);
    return false;
  }
  for (int Row = 0; Row < H; ++Row)
    std::memcpy(static_cast<uint8_t*>(Up.Staging.Mapped) + size_t(Row) * RowBytes, static_cast<const uint8_t*>(Pixels) + size_t(Row) * StrideBytes, RowBytes);
  Pending.push_back(Up);
  return true;
}

void VulkanRenderer::RecordUploads() {
  auto Barrier = [&](Texture& Tex, VkImageLayout From, VkImageLayout To, VkAccessFlags SrcAccess, VkAccessFlags DstAccess,
                     VkPipelineStageFlags SrcStage, VkPipelineStageFlags DstStage) {
    VkImageMemoryBarrier B {};
    B.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    B.srcAccessMask = SrcAccess;
    B.dstAccessMask = DstAccess;
    B.oldLayout = From;
    B.newLayout = To;
    B.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    B.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    B.image = Tex.Image;
    B.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(Cmd, SrcStage, DstStage, 0, 0, nullptr, 0, nullptr, 1, &B);
  };
  for (Upload& Up : Pending) {
    Texture* Tex = TextureFor(Up.Tex);
    if (!Tex)
      continue;
    if (!Tex->Uploading) {
      Barrier(*Tex, Tex->Initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      Tex->Uploading = true;
    }
    VkBufferImageCopy Region {};
    Region.bufferOffset = 0;
    Region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    Region.imageOffset = {int32_t(Up.X), int32_t(Up.Y), 0};
    Region.imageExtent = {Up.W, Up.H, 1};
    vkCmdCopyBufferToImage(Cmd, Up.Staging.Buffer, Tex->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);
  }
  for (Upload& Up : Pending) {
    Texture* Tex = TextureFor(Up.Tex);
    if (!Tex || !Tex->Uploading)
      continue;
    Barrier(*Tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    Tex->Uploading = false;
    Tex->Initialized = true;
  }
}

// After the previous frame completed (BeginFrame waited for the fence).
void VulkanRenderer::RetireFinishedFrame() {
  for (Upload& Up : InFlight)
    DestroyHostBuffer(Up.Staging);
  InFlight.clear();
  // Drop queued uploads of textures released since.
  for (size_t I = 0; I < Textures.size(); ++I)
    if (Textures[I].Used && Textures[I].Released) {
      for (size_t U = 0; U < Pending.size();)
        if (Pending[U].Tex == TextureId(I)) {
          DestroyHostBuffer(Pending[U].Staging);
          Pending.erase(Pending.begin() + long(U));
        } else {
          ++U;
        }
      DestroyTextureResources(Textures[I]);
    }
}

// ---- Pipeline -------------------------------------------------------------

bool VulkanRenderer::CreatePipeline() {
  VkDescriptorSetLayoutBinding Binding {};
  Binding.binding = 0;
  Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  Binding.descriptorCount = 1;
  Binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo LayoutInfo {};
  LayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  LayoutInfo.bindingCount = 1;
  LayoutInfo.pBindings = &Binding;
  if (!Step("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(T.Device, &LayoutInfo, nullptr, &SetLayout)))
    return false;
  VkDescriptorPoolSize PoolSize {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxTextures};
  VkDescriptorPoolCreateInfo PoolInfo {};
  PoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  PoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  PoolInfo.maxSets = MaxTextures;
  PoolInfo.poolSizeCount = 1;
  PoolInfo.pPoolSizes = &PoolSize;
  if (!Step("vkCreateDescriptorPool", vkCreateDescriptorPool(T.Device, &PoolInfo, nullptr, &DescriptorPool)))
    return false;
  VkSamplerCreateInfo SamplerInfo {};
  SamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  SamplerInfo.magFilter = VK_FILTER_NEAREST;
  SamplerInfo.minFilter = VK_FILTER_NEAREST;
  SamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  SamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  SamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  SamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (!Step("vkCreateSampler(nearest)", vkCreateSampler(T.Device, &SamplerInfo, nullptr, &Nearest)))
    return false;
  SamplerInfo.magFilter = VK_FILTER_LINEAR;
  SamplerInfo.minFilter = VK_FILTER_LINEAR;
  if (!Step("vkCreateSampler(linear)", vkCreateSampler(T.Device, &SamplerInfo, nullptr, &Linear)))
    return false;
  VkPushConstantRange Push {VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
  VkPipelineLayoutCreateInfo PipelineLayoutInfo {};
  PipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  PipelineLayoutInfo.setLayoutCount = 1;
  PipelineLayoutInfo.pSetLayouts = &SetLayout;
  PipelineLayoutInfo.pushConstantRangeCount = 1;
  PipelineLayoutInfo.pPushConstantRanges = &Push;
  if (!Step("vkCreatePipelineLayout", vkCreatePipelineLayout(T.Device, &PipelineLayoutInfo, nullptr, &PipelineLayout)))
    return false;

  VkShaderModule Vert = VK_NULL_HANDLE, Frag = VK_NULL_HANDLE;
  VkShaderModuleCreateInfo ShaderInfo {};
  ShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ShaderInfo.codeSize = sizeof(ui_vert);
  ShaderInfo.pCode = ui_vert;
  if (!Step("vkCreateShaderModule(vert)", vkCreateShaderModule(T.Device, &ShaderInfo, nullptr, &Vert)))
    return false;
  ShaderInfo.codeSize = sizeof(ui_frag);
  ShaderInfo.pCode = ui_frag;
  if (!Step("vkCreateShaderModule(frag)", vkCreateShaderModule(T.Device, &ShaderInfo, nullptr, &Frag))) {
    vkDestroyShaderModule(T.Device, Vert, nullptr);
    return false;
  }
  VkPipelineShaderStageCreateInfo Stages[2] {};
  Stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  Stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  Stages[0].module = Vert;
  Stages[0].pName = "main";
  Stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  Stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  Stages[1].module = Frag;
  Stages[1].pName = "main";
  VkVertexInputBindingDescription BindingDesc {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription Attributes[6] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, X)},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, U)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, Lx)},
      {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, Hw)},
      {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, Mode)},
      {5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, Color)},
  };
  VkPipelineVertexInputStateCreateInfo VertexInput {};
  VertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VertexInput.vertexBindingDescriptionCount = 1;
  VertexInput.pVertexBindingDescriptions = &BindingDesc;
  VertexInput.vertexAttributeDescriptionCount = 6;
  VertexInput.pVertexAttributeDescriptions = Attributes;
  VkPipelineInputAssemblyStateCreateInfo Assembly {};
  Assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  Assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo ViewportState {};
  ViewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  ViewportState.viewportCount = 1;
  ViewportState.scissorCount = 1;
  const VkDynamicState Dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo DynamicState {};
  DynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  DynamicState.dynamicStateCount = 2;
  DynamicState.pDynamicStates = Dynamic;
  VkPipelineRasterizationStateCreateInfo Raster {};
  Raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  Raster.polygonMode = VK_POLYGON_MODE_FILL;
  Raster.cullMode = VK_CULL_MODE_NONE;
  Raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
  Raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo Multisample {};
  Multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  // dst = src * a + dst * (1 - a); alpha channel keeps the maximum.
  VkPipelineColorBlendAttachmentState Blend {};
  Blend.blendEnable = VK_TRUE;
  Blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  Blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  Blend.colorBlendOp = VK_BLEND_OP_ADD;
  Blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  Blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  Blend.alphaBlendOp = VK_BLEND_OP_MAX;
  Blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo BlendState {};
  BlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  BlendState.attachmentCount = 1;
  BlendState.pAttachments = &Blend;
  VkGraphicsPipelineCreateInfo Info {};
  Info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  Info.stageCount = 2;
  Info.pStages = Stages;
  Info.pVertexInputState = &VertexInput;
  Info.pInputAssemblyState = &Assembly;
  Info.pViewportState = &ViewportState;
  Info.pRasterizationState = &Raster;
  Info.pMultisampleState = &Multisample;
  Info.pColorBlendState = &BlendState;
  Info.pDynamicState = &DynamicState;
  Info.layout = PipelineLayout;
  Info.renderPass = T.RenderPass;
  Info.subpass = 0;
  VkResult R = vkCreateGraphicsPipelines(T.Device, VK_NULL_HANDLE, 1, &Info, nullptr, &Pipeline);
  vkDestroyShaderModule(T.Device, Vert, nullptr);
  vkDestroyShaderModule(T.Device, Frag, nullptr);
  return Step("vkCreateGraphicsPipelines", R);
}

bool VulkanRenderer::Init() {
  if (!CreatePipeline())
    return false;
  VkResult R = CreateQuadBuffers(InitialQuadCapacity);
  if (!Step("create quad buffers", R))
    return false;
  VkCommandBufferAllocateInfo CmdInfo {};
  CmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  CmdInfo.commandPool = T.CommandPool;
  CmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  CmdInfo.commandBufferCount = 1;
  if (!Step("vkAllocateCommandBuffers", vkAllocateCommandBuffers(T.Device, &CmdInfo, &Cmd)))
    return false;
  Ready = true;
  // Texture 0: one white texel, bound by shape draws.
  White = CreateTexture(1, 1, TextureFormat::Rgba8, TextureFilter::Nearest);
  const uint8_t WhiteTexel[4] = {255, 255, 255, 255};
  if (White != 0 || !UpdateTexture(White, 0, 0, 1, 1, WhiteTexel, 0)) {
    Log("FAIL white texture");
    return false;
  }
  Log("step white texture: ok");
  return true;
}

void VulkanRenderer::Shutdown() {
  if (!T.Device)
    return;
  vkDeviceWaitIdle(T.Device);
  for (Upload& Up : InFlight)
    DestroyHostBuffer(Up.Staging);
  for (Upload& Up : Pending)
    DestroyHostBuffer(Up.Staging);
  InFlight.clear();
  Pending.clear();
  for (Texture& Tex : Textures)
    if (Tex.Used || Tex.Image)
      DestroyTextureResources(Tex);
  Textures.clear();
  DestroyHostBuffer(VertexBuffer);
  DestroyHostBuffer(IndexBuffer);
  if (Cmd)
    vkFreeCommandBuffers(T.Device, T.CommandPool, 1, &Cmd);
  if (Pipeline)
    vkDestroyPipeline(T.Device, Pipeline, nullptr);
  if (PipelineLayout)
    vkDestroyPipelineLayout(T.Device, PipelineLayout, nullptr);
  if (Nearest)
    vkDestroySampler(T.Device, Nearest, nullptr);
  if (Linear)
    vkDestroySampler(T.Device, Linear, nullptr);
  if (DescriptorPool)
    vkDestroyDescriptorPool(T.Device, DescriptorPool, nullptr);
  if (SetLayout)
    vkDestroyDescriptorSetLayout(T.Device, SetLayout, nullptr);
  Cmd = VK_NULL_HANDLE;
  Pipeline = VK_NULL_HANDLE;
  Ready = false;
}

// ---- Drawing --------------------------------------------------------------

void VulkanRenderer::AddQuad(TextureId Tex, const Vertex Corners[4]) {
  if (!InFrame || Clip.W <= 0 || Clip.H <= 0)
    return;
  uint32_t QuadIndex = uint32_t(Vertices.size() / 4);
  if (!Groups.empty() && Groups.back().Tex == Tex && Groups.back().Scissor == Clip) {
    ++Groups.back().QuadCount;
  } else {
    Groups.push_back(DrawGroup {Tex, Clip, QuadIndex, 1});
  }
  Vertices.insert(Vertices.end(), Corners, Corners + 4);
}

void VulkanRenderer::AddShape(Rect R, float Radius, float Stroke, float Mode, Color C) {
  C = WithOpacity(C);
  if (!InFrame || R.W <= 0 || R.H <= 0 || C.A == 0)
    return;
  const float Hw = R.W * 0.5f, Hh = R.H * 0.5f;
  Radius = std::clamp(Radius, 0.0f, std::min(Hw, Hh));
  const float Pad = 1.0f;  // room for the antialiased edge
  const float Cx = R.X + Hw, Cy = R.Y + Hh;
  const float X0 = R.X - Pad, Y0 = R.Y - Pad, X1 = R.X + R.W + Pad, Y1 = R.Y + R.H + Pad;
  const uint32_t Col = PackColor(C);
  auto V = [&](float X, float Y) { return Vertex {X, Y, 0, 0, X - Cx, Y - Cy, Hw, Hh, Radius, Stroke, Mode, Col}; };
  const Vertex Corners[4] = {V(X0, Y0), V(X1, Y0), V(X1, Y1), V(X0, Y1)};
  AddQuad(White, Corners);
}

bool VulkanRenderer::Line(float X0, float Y0, float X1, float Y1, float Thickness, Color C) {
  const bool Horizontal = Y0 == Y1, Vertical = X0 == X1;
  if (!Horizontal && !Vertical)
    return false;
  const float T2 = std::max(1.0f, std::round(Thickness));
  if (Horizontal) {
    const float Xa = Snap(std::min(X0, X1)), Xb = Snap(std::max(X0, X1));
    FillRect(Rect {Xa, Snap(Y0 - T2 * 0.5f), Xb - Xa, T2}, C);
  } else {
    const float Ya = Snap(std::min(Y0, Y1)), Yb = Snap(std::max(Y0, Y1));
    FillRect(Rect {Snap(X0 - T2 * 0.5f), Ya, T2, Yb - Ya}, C);
  }
  return true;
}

void VulkanRenderer::Image(TextureId Id, Rect Dst, Rect Src, Color Tint) {
  Texture* Tex = TextureFor(Id);
  if (!InFrame || !Tex || Tex->Released || Dst.W <= 0 || Dst.H <= 0 || Src.W <= 0 || Src.H <= 0)
    return;
  const float U0 = Src.X / float(Tex->W), V0 = Src.Y / float(Tex->H);
  const float U1 = (Src.X + Src.W) / float(Tex->W), V1 = (Src.Y + Src.H) / float(Tex->H);
  const uint32_t Col = PackColor(WithOpacity(Tint));
  const float X0 = Dst.X, Y0 = Dst.Y, X1 = Dst.X + Dst.W, Y1 = Dst.Y + Dst.H;
  const Vertex Corners[4] = {
      {X0, Y0, U0, V0, 0, 0, 0, 0, 0, 0, 0, Col},
      {X1, Y0, U1, V0, 0, 0, 0, 0, 0, 0, 0, Col},
      {X1, Y1, U1, V1, 0, 0, 0, 0, 0, 0, 0, Col},
      {X0, Y1, U0, V1, 0, 0, 0, 0, 0, 0, 0, Col},
  };
  AddQuad(Id, Corners);
}

void VulkanRenderer::PushClip(Rect R) {
  ClipStack.push_back(Clip);
  int X0 = int(std::floor(R.X)), Y0 = int(std::floor(R.Y));
  int X1 = int(std::ceil(R.X + R.W)), Y1 = int(std::ceil(R.Y + R.H));
  X0 = std::max(X0, Clip.X);
  Y0 = std::max(Y0, Clip.Y);
  X1 = std::min(X1, Clip.X + Clip.W);
  Y1 = std::min(Y1, Clip.Y + Clip.H);
  Clip = IRect {X0, Y0, std::max(0, X1 - X0), std::max(0, Y1 - Y0)};
}

void VulkanRenderer::PopClip() {
  if (ClipStack.empty())
    return;
  Clip = ClipStack.back();
  ClipStack.pop_back();
}

bool VulkanRenderer::BeginFrame(Color Clear) {
  InFrame = false;
  if (!Ready || !T.BeginFrame())
    return false;
  RetireFinishedFrame();
  Vertices.clear();
  Groups.clear();
  ClipStack.clear();
  Clip = IRect {0, 0, int(T.Extent.width), int(T.Extent.height)};
  ClearColor = Clear;
  InFrame = true;
  return true;
}

void VulkanRenderer::RecordDraws(uint32_t Index) {
  VkClearValue Clear {};
  Clear.color.float32[0] = float(ClearColor.R) / 255.0f;
  Clear.color.float32[1] = float(ClearColor.G) / 255.0f;
  Clear.color.float32[2] = float(ClearColor.B) / 255.0f;
  Clear.color.float32[3] = float(ClearColor.A) / 255.0f;
  VkRenderPassBeginInfo Pass {};
  Pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  Pass.renderPass = T.RenderPass;
  Pass.framebuffer = T.Framebuffers[Index];
  Pass.renderArea.extent = T.Extent;
  Pass.clearValueCount = 1;
  Pass.pClearValues = &Clear;
  vkCmdBeginRenderPass(Cmd, &Pass, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline);
  VkViewport Viewport {0, 0, float(T.Extent.width), float(T.Extent.height), 0.0f, 1.0f};
  vkCmdSetViewport(Cmd, 0, 1, &Viewport);
  const float Scale[2] = {2.0f / float(T.Extent.width), 2.0f / float(T.Extent.height)};
  vkCmdPushConstants(Cmd, PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Scale), Scale);
  const VkDeviceSize Offset = 0;
  vkCmdBindVertexBuffers(Cmd, 0, 1, &VertexBuffer.Buffer, &Offset);
  vkCmdBindIndexBuffer(Cmd, IndexBuffer.Buffer, 0, VK_INDEX_TYPE_UINT32);
  for (const DrawGroup& G : Groups) {
    Texture* Tex = TextureFor(G.Tex);
    if (!Tex || Tex->Released)
      continue;
    // Clamp the scissor to the framebuffer (the output size may have changed since the clip was set).
    int X0 = std::clamp(G.Scissor.X, 0, int(T.Extent.width)), Y0 = std::clamp(G.Scissor.Y, 0, int(T.Extent.height));
    int X1 = std::clamp(G.Scissor.X + G.Scissor.W, 0, int(T.Extent.width)), Y1 = std::clamp(G.Scissor.Y + G.Scissor.H, 0, int(T.Extent.height));
    VkRect2D Scissor {{X0, Y0}, {uint32_t(X1 - X0), uint32_t(Y1 - Y0)}};
    vkCmdSetScissor(Cmd, 0, 1, &Scissor);
    vkCmdBindDescriptorSets(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineLayout, 0, 1, &Tex->Set, 0, nullptr);
    vkCmdDrawIndexed(Cmd, G.QuadCount * 6, 1, G.FirstQuad * 6, 0, 0);
  }
  vkCmdEndRenderPass(Cmd);
}

bool VulkanRenderer::EndFrame() {
  if (!InFrame)
    return false;
  InFrame = false;
  const uint32_t Count = uint32_t(Vertices.size() / 4);
  if (Count > QuadCapacity) {
    uint32_t Capacity = QuadCapacity;
    while (Capacity < Count)
      Capacity *= 2;
    VkResult R = CreateQuadBuffers(Capacity);  // the previous frame is complete
    if (R != VK_SUCCESS) {
      if (T.ThrottleFailure())
        Step("grow quad buffers", R);
      return false;
    }
    Log("quad buffers grown to %u quads", Capacity);
  }
  if (Count)
    std::memcpy(VertexBuffer.Mapped, Vertices.data(), Vertices.size() * sizeof(Vertex));
  uint32_t Index = 0;
  if (!T.AcquireImage(Index))
    return false;
  VkResult R = vkResetCommandBuffer(Cmd, 0);
  VkCommandBufferBeginInfo Begin {};
  Begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  Begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (R == VK_SUCCESS)
    R = vkBeginCommandBuffer(Cmd, &Begin);
  if (R != VK_SUCCESS) {
    if (T.ThrottleFailure())
      Step("vkBeginCommandBuffer", R);
    return false;
  }
  RecordUploads();
  RecordDraws(Index);
  T.RecordAfterDraw(Cmd, Index);
  R = vkEndCommandBuffer(Cmd);
  if (R != VK_SUCCESS) {
    if (T.ThrottleFailure())
      Step("vkEndCommandBuffer", R);
    return false;
  }
  // Uploads are consumed; their staging buffers live until this frame completes.
  for (Upload& Up : Pending)
    InFlight.push_back(Up);
  Pending.clear();
  return T.Submit(Cmd, Index);
}

} // namespace

std::unique_ptr<Renderer> CreateVulkanRenderer(vk::Target& Target) {
  auto R = std::make_unique<VulkanRenderer>(Target);
  if (!R->Init())
    return nullptr;
  Log("step renderer ready: ok");
  return R;
}

} // namespace ui
