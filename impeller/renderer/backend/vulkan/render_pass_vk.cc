// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/render_pass_vk.h"

#include <array>
#include <cstdint>
#include <vector>

#include "fml/status.h"
#include "impeller/base/validation.h"
#include "impeller/core/device_buffer.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture.h"
#include "impeller/renderer/backend/vulkan/barrier_vk.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/command_encoder_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/device_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/formats_vk.h"
#include "impeller/renderer/backend/vulkan/pipeline_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_builder_vk.h"
#include "impeller/renderer/backend/vulkan/sampler_vk.h"
#include "impeller/renderer/backend/vulkan/shared_object_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/backend/vulkan/vk.h"
#include "vulkan/vulkan_handles.hpp"

namespace impeller {

// Warning: if any of the constant values or layouts are changed in the
// framebuffer fetch shader, then this input binding may need to be
// manually changed.
//
// See: impeller/entity/shaders/blending/framebuffer_blend.frag
static constexpr size_t kMagicSubpassInputBinding = 64u;

static vk::ClearColorValue VKClearValueFromColor(Color color) {
  vk::ClearColorValue value;
  value.setFloat32(
      std::array<float, 4>{color.red, color.green, color.blue, color.alpha});
  return value;
}

static vk::ClearDepthStencilValue VKClearValueFromDepthStencil(uint32_t stencil,
                                                               Scalar depth) {
  vk::ClearDepthStencilValue value;
  value.depth = depth;
  value.stencil = stencil;
  return value;
}

static std::vector<vk::ClearValue> GetVKClearValues(
    const RenderTarget& target) {
  std::vector<vk::ClearValue> clears;

  for (const auto& [_, color] : target.GetColorAttachments()) {
    clears.emplace_back(VKClearValueFromColor(color.clear_color));
    if (color.resolve_texture) {
      clears.emplace_back(VKClearValueFromColor(color.clear_color));
    }
  }

  const auto& depth = target.GetDepthAttachment();
  const auto& stencil = target.GetStencilAttachment();

  if (depth.has_value()) {
    clears.emplace_back(VKClearValueFromDepthStencil(
        stencil ? stencil->clear_stencil : 0u, depth->clear_depth));
  }

  if (stencil.has_value()) {
    clears.emplace_back(VKClearValueFromDepthStencil(
        stencil->clear_stencil, depth ? depth->clear_depth : 0.0f));
  }

  return clears;
}

SharedHandleVK<vk::RenderPass> RenderPassVK::CreateVKRenderPass(
    const ContextVK& context,
    const SharedHandleVK<vk::RenderPass>& recycled_renderpass,
    const std::shared_ptr<CommandBufferVK>& command_buffer) const {
  BarrierVK barrier;
  barrier.new_layout = vk::ImageLayout::eGeneral;
  barrier.cmd_buffer = command_buffer->GetEncoder()->GetCommandBuffer();
  barrier.src_access = vk::AccessFlagBits::eShaderRead;
  barrier.src_stage = vk::PipelineStageFlagBits::eFragmentShader;
  barrier.dst_access = vk::AccessFlagBits::eColorAttachmentWrite |
                       vk::AccessFlagBits::eTransferWrite;
  barrier.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                      vk::PipelineStageFlagBits::eTransfer;

  RenderPassBuilderVK builder;

  for (const auto& [bind_point, color] : render_target_.GetColorAttachments()) {
    builder.SetColorAttachment(
        bind_point,                                          //
        color.texture->GetTextureDescriptor().format,        //
        color.texture->GetTextureDescriptor().sample_count,  //
        color.load_action,                                   //
        color.store_action                                   //
    );
    TextureVK::Cast(*color.texture).SetLayout(barrier);
    if (color.resolve_texture) {
      TextureVK::Cast(*color.resolve_texture).SetLayout(barrier);
    }
  }

  if (auto depth = render_target_.GetDepthAttachment(); depth.has_value()) {
    builder.SetDepthStencilAttachment(
        depth->texture->GetTextureDescriptor().format,        //
        depth->texture->GetTextureDescriptor().sample_count,  //
        depth->load_action,                                   //
        depth->store_action                                   //
    );
    TextureVK::Cast(*depth->texture).SetLayout(barrier);
  }

  if (auto stencil = render_target_.GetStencilAttachment();
      stencil.has_value()) {
    builder.SetStencilAttachment(
        stencil->texture->GetTextureDescriptor().format,        //
        stencil->texture->GetTextureDescriptor().sample_count,  //
        stencil->load_action,                                   //
        stencil->store_action                                   //
    );
    TextureVK::Cast(*stencil->texture).SetLayout(barrier);
  }

  // There may exist a previous recycled render pass that we can continue using.
  // This is probably compatible with the render pass we are about to construct,
  // but I have not conclusively proven this. If there are scenarios that
  // produce validation warnings, we could use them to determine if we need
  // additional checks at this point to determine reusability.
  if (recycled_renderpass != nullptr) {
    return recycled_renderpass;
  }

  auto pass = builder.Build(context.GetDevice());

  if (!pass) {
    VALIDATION_LOG << "Failed to create render pass for framebuffer.";
    return {};
  }

  context.SetDebugName(pass.get(), debug_label_.c_str());

  return MakeSharedVK(std::move(pass));
}

RenderPassVK::RenderPassVK(const std::shared_ptr<const Context>& context,
                           const RenderTarget& target,
                           std::shared_ptr<CommandBufferVK> command_buffer)
    : RenderPass(context, target), command_buffer_(std::move(command_buffer)) {
  color_image_vk_ =
      render_target_.GetColorAttachments().find(0u)->second.texture;
  resolve_image_vk_ =
      render_target_.GetColorAttachments().find(0u)->second.resolve_texture;

  const auto& vk_context = ContextVK::Cast(*context);
  const std::shared_ptr<CommandEncoderVK>& encoder =
      command_buffer_->GetEncoder();
  command_buffer_vk_ = encoder->GetCommandBuffer();
  render_target_.IterateAllAttachments(
      [&encoder](const auto& attachment) -> bool {
        encoder->Track(attachment.texture);
        encoder->Track(attachment.resolve_texture);
        return true;
      });

  SharedHandleVK<vk::RenderPass> recycled_render_pass;
  SharedHandleVK<vk::Framebuffer> recycled_framebuffer;
  if (resolve_image_vk_) {
    recycled_render_pass = TextureVK::Cast(*resolve_image_vk_).GetRenderPass();
    recycled_framebuffer = TextureVK::Cast(*resolve_image_vk_).GetFramebuffer();
  }

  const auto& target_size = render_target_.GetRenderTargetSize();

  render_pass_ =
      CreateVKRenderPass(vk_context, recycled_render_pass, command_buffer_);
  if (!render_pass_) {
    VALIDATION_LOG << "Could not create renderpass.";
    is_valid_ = false;
    return;
  }

  auto framebuffer = (recycled_framebuffer == nullptr)
                         ? CreateVKFramebuffer(vk_context, *render_pass_)
                         : recycled_framebuffer;
  if (!framebuffer) {
    VALIDATION_LOG << "Could not create framebuffer.";
    is_valid_ = false;
    return;
  }

  if (!encoder->Track(framebuffer) || !encoder->Track(render_pass_)) {
    is_valid_ = false;
    return;
  }
  if (resolve_image_vk_) {
    TextureVK::Cast(*resolve_image_vk_).SetFramebuffer(framebuffer);
    TextureVK::Cast(*resolve_image_vk_).SetRenderPass(render_pass_);
  }

  auto clear_values = GetVKClearValues(render_target_);

  vk::RenderPassBeginInfo pass_info;
  pass_info.renderPass = *render_pass_;
  pass_info.framebuffer = *framebuffer;
  pass_info.renderArea.extent.width = static_cast<uint32_t>(target_size.width);
  pass_info.renderArea.extent.height =
      static_cast<uint32_t>(target_size.height);
  pass_info.setClearValues(clear_values);

  command_buffer_vk_.beginRenderPass(pass_info, vk::SubpassContents::eInline);

  // Set the initial viewport.
  const auto vp = Viewport{.rect = Rect::MakeSize(target_size)};
  vk::Viewport viewport = vk::Viewport()
                              .setWidth(vp.rect.GetWidth())
                              .setHeight(-vp.rect.GetHeight())
                              .setY(vp.rect.GetHeight())
                              .setMinDepth(0.0f)
                              .setMaxDepth(1.0f);
  command_buffer_vk_.setViewport(0, 1, &viewport);

  // Set the initial scissor.
  const auto sc = IRect::MakeSize(target_size);
  vk::Rect2D scissor =
      vk::Rect2D()
          .setOffset(vk::Offset2D(sc.GetX(), sc.GetY()))
          .setExtent(vk::Extent2D(sc.GetWidth(), sc.GetHeight()));
  command_buffer_vk_.setScissor(0, 1, &scissor);

  // Set the initial stencil reference.
  command_buffer_vk_.setStencilReference(
      vk::StencilFaceFlagBits::eVkStencilFrontAndBack, 0u);

  is_valid_ = true;
}

RenderPassVK::~RenderPassVK() = default;

bool RenderPassVK::IsValid() const {
  return is_valid_;
}

void RenderPassVK::OnSetLabel(std::string label) {
#ifdef IMPELLER_DEBUG
  ContextVK::Cast(*context_).SetDebugName(render_pass_->Get(),
                                          std::string(label).c_str());
#endif  // IMPELLER_DEBUG
}

SharedHandleVK<vk::Framebuffer> RenderPassVK::CreateVKFramebuffer(
    const ContextVK& context,
    const vk::RenderPass& pass) const {
  vk::FramebufferCreateInfo fb_info;

  fb_info.renderPass = pass;

  const auto target_size = render_target_.GetRenderTargetSize();
  fb_info.width = target_size.width;
  fb_info.height = target_size.height;
  fb_info.layers = 1u;

  std::vector<vk::ImageView> attachments;

  // This bit must be consistent to ensure compatibility with the pass created
  // earlier. Follow this order: Color attachments, then depth-stencil, then
  // stencil.
  for (const auto& [_, color] : render_target_.GetColorAttachments()) {
    // The bind point doesn't matter here since that information is present in
    // the render pass.
    attachments.emplace_back(
        TextureVK::Cast(*color.texture).GetRenderTargetView());
    if (color.resolve_texture) {
      attachments.emplace_back(
          TextureVK::Cast(*color.resolve_texture).GetRenderTargetView());
    }
  }
  if (auto depth = render_target_.GetDepthAttachment(); depth.has_value()) {
    attachments.emplace_back(
        TextureVK::Cast(*depth->texture).GetRenderTargetView());
  } else if (auto stencil = render_target_.GetStencilAttachment();
             stencil.has_value()) {
    attachments.emplace_back(
        TextureVK::Cast(*stencil->texture).GetRenderTargetView());
  }

  fb_info.setAttachments(attachments);

  auto [result, framebuffer] =
      context.GetDevice().createFramebufferUnique(fb_info);

  if (result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create framebuffer: " << vk::to_string(result);
    return {};
  }

  return MakeSharedVK(std::move(framebuffer));
}

// |RenderPass|
void RenderPassVK::SetPipeline(
    const std::shared_ptr<Pipeline<PipelineDescriptor>>& pipeline) {
  PipelineVK& pipeline_vk = PipelineVK::Cast(*pipeline);

  auto descriptor_result =
      command_buffer_->GetEncoder()->AllocateDescriptorSets(
          pipeline_vk.GetDescriptorSetLayout(), ContextVK::Cast(*context_));
  if (!descriptor_result.ok()) {
    return;
  }
  pipeline_valid_ = true;
  descriptor_set_ = descriptor_result.value();
  pipeline_layout_ = pipeline_vk.GetPipelineLayout();
  command_buffer_vk_.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                  pipeline_vk.GetPipeline());

  pipeline_uses_input_attachments_ =
      pipeline->GetDescriptor().GetVertexDescriptor()->UsesInputAttacments();

  if (pipeline_uses_input_attachments_) {
    if (bound_image_offset_ >= kMaxBindings) {
      pipeline_valid_ = false;
      return;
    }
    vk::DescriptorImageInfo image_info;
    image_info.imageLayout = vk::ImageLayout::eGeneral;
    image_info.sampler = VK_NULL_HANDLE;
    image_info.imageView = TextureVK::Cast(*color_image_vk_).GetImageView();
    image_workspace_[bound_image_offset_++] = image_info;

    vk::WriteDescriptorSet write_set;
    write_set.dstBinding = kMagicSubpassInputBinding;
    write_set.descriptorCount = 1u;
    write_set.descriptorType = vk::DescriptorType::eInputAttachment;
    write_set.pImageInfo = &image_workspace_[bound_image_offset_ - 1];

    write_workspace_[descriptor_write_offset_++] = write_set;
  }
}

// |RenderPass|
void RenderPassVK::SetCommandLabel(std::string_view label) {
#ifdef IMPELLER_DEBUG
  command_buffer_->GetEncoder()->PushDebugGroup(label);
  has_label_ = true;
#endif  // IMPELLER_DEBUG
}

// |RenderPass|
void RenderPassVK::SetStencilReference(uint32_t value) {
  command_buffer_vk_.setStencilReference(
      vk::StencilFaceFlagBits::eVkStencilFrontAndBack, value);
}

// |RenderPass|
void RenderPassVK::SetBaseVertex(uint64_t value) {
  base_vertex_ = value;
}

// |RenderPass|
void RenderPassVK::SetViewport(Viewport viewport) {
  vk::Viewport viewport_vk = vk::Viewport()
                                 .setWidth(viewport.rect.GetWidth())
                                 .setHeight(-viewport.rect.GetHeight())
                                 .setY(viewport.rect.GetHeight())
                                 .setMinDepth(0.0f)
                                 .setMaxDepth(1.0f);
  command_buffer_vk_.setViewport(0, 1, &viewport_vk);
}

// |RenderPass|
void RenderPassVK::SetScissor(IRect scissor) {
  vk::Rect2D scissor_vk =
      vk::Rect2D()
          .setOffset(vk::Offset2D(scissor.GetX(), scissor.GetY()))
          .setExtent(vk::Extent2D(scissor.GetWidth(), scissor.GetHeight()));
  command_buffer_vk_.setScissor(0, 1, &scissor_vk);
}

// |RenderPass|
void RenderPassVK::SetInstanceCount(size_t count) {
  instance_count_ = count;
}

// |RenderPass|
bool RenderPassVK::SetVertexBuffer(VertexBuffer buffer) {
  vertex_count_ = buffer.vertex_count;
  if (buffer.index_type == IndexType::kUnknown || !buffer.vertex_buffer) {
    return false;
  }

  if (!command_buffer_->GetEncoder()->Track(buffer.vertex_buffer.buffer)) {
    return false;
  }

  // Bind the vertex buffer.
  vk::Buffer vertex_buffer_handle =
      DeviceBufferVK::Cast(*buffer.vertex_buffer.buffer).GetBuffer();
  vk::Buffer vertex_buffers[] = {vertex_buffer_handle};
  vk::DeviceSize vertex_buffer_offsets[] = {buffer.vertex_buffer.range.offset};

  command_buffer_vk_.bindVertexBuffers(0u, 1u, vertex_buffers,
                                       vertex_buffer_offsets);

  // Bind the index buffer.
  if (buffer.index_type != IndexType::kNone) {
    has_index_buffer_ = true;
    const BufferView& index_buffer_view = buffer.index_buffer;
    if (!index_buffer_view) {
      return false;
    }

    const std::shared_ptr<const DeviceBuffer>& index_buffer =
        index_buffer_view.buffer;
    if (!index_buffer) {
      VALIDATION_LOG << "Failed to acquire device buffer"
                     << " for index buffer view";
      return false;
    }

    if (!command_buffer_->GetEncoder()->Track(index_buffer)) {
      return false;
    }

    vk::Buffer index_buffer_handle =
        DeviceBufferVK::Cast(*index_buffer).GetBuffer();
    command_buffer_vk_.bindIndexBuffer(index_buffer_handle,
                                       index_buffer_view.range.offset,
                                       ToVKIndexType(buffer.index_type));
  } else {
    has_index_buffer_ = false;
  }
  return true;
}

// |RenderPass|
fml::Status RenderPassVK::Draw() {
  if (!pipeline_valid_) {
    return fml::Status(fml::StatusCode::kCancelled,
                       "No valid pipeline is bound to the RenderPass.");
  }

  const ContextVK& context_vk = ContextVK::Cast(*context_);
  for (auto i = 0u; i < descriptor_write_offset_; i++) {
    write_workspace_[i].dstSet = descriptor_set_;
  }

  context_vk.GetDevice().updateDescriptorSets(descriptor_write_offset_,
                                              write_workspace_.data(), 0u, {});

  command_buffer_vk_.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics,  // bind point
      pipeline_layout_,                  // layout
      0,                                 // first set
      1,                                 // set count
      &descriptor_set_,                  // sets
      0,                                 // offset count
      nullptr                            // offsets
  );

  if (pipeline_uses_input_attachments_) {
    InsertBarrierForInputAttachmentRead(
        command_buffer_vk_, TextureVK::Cast(*color_image_vk_).GetImage());
  }

  if (has_index_buffer_) {
    command_buffer_vk_.drawIndexed(vertex_count_,    // index count
                                   instance_count_,  // instance count
                                   0u,               // first index
                                   base_vertex_,     // vertex offset
                                   0u                // first instance
    );
  } else {
    command_buffer_vk_.draw(vertex_count_,    // vertex count
                            instance_count_,  // instance count
                            base_vertex_,     // vertex offset
                            0u                // first instance
    );
  }

#ifdef IMPELLER_DEBUG
  if (has_label_) {
    command_buffer_->GetEncoder()->PopDebugGroup();
  }
#endif  // IMPELLER_DEBUG
  has_label_ = false;
  has_index_buffer_ = false;
  bound_image_offset_ = 0u;
  bound_buffer_offset_ = 0u;
  descriptor_write_offset_ = 0u;
  instance_count_ = 1u;
  base_vertex_ = 0u;
  vertex_count_ = 0u;
  pipeline_valid_ = false;
  pipeline_uses_input_attachments_ = false;
  return fml::Status();
}

// The RenderPassVK binding methods only need the binding, set, and buffer type
// information.
bool RenderPassVK::BindResource(ShaderStage stage,
                                DescriptorType type,
                                const ShaderUniformSlot& slot,
                                const ShaderMetadata& metadata,
                                BufferView view) {
  return BindResource(slot.binding, type, view);
}

bool RenderPassVK::BindResource(
    ShaderStage stage,
    DescriptorType type,
    const ShaderUniformSlot& slot,
    const std::shared_ptr<const ShaderMetadata>& metadata,
    BufferView view) {
  return BindResource(slot.binding, type, view);
}

bool RenderPassVK::BindResource(size_t binding,
                                DescriptorType type,
                                const BufferView& view) {
  if (bound_buffer_offset_ >= kMaxBindings) {
    return false;
  }

  const std::shared_ptr<const DeviceBuffer>& device_buffer = view.buffer;
  auto buffer = DeviceBufferVK::Cast(*device_buffer).GetBuffer();
  if (!buffer) {
    return false;
  }

  if (!command_buffer_->GetEncoder()->Track(device_buffer)) {
    return false;
  }

  uint32_t offset = view.range.offset;

  vk::DescriptorBufferInfo buffer_info;
  buffer_info.buffer = buffer;
  buffer_info.offset = offset;
  buffer_info.range = view.range.length;
  buffer_workspace_[bound_buffer_offset_++] = buffer_info;

  vk::WriteDescriptorSet write_set;
  write_set.dstBinding = binding;
  write_set.descriptorCount = 1u;
  write_set.descriptorType = ToVKDescriptorType(type);
  write_set.pBufferInfo = &buffer_workspace_[bound_buffer_offset_ - 1];

  write_workspace_[descriptor_write_offset_++] = write_set;
  return true;
}

bool RenderPassVK::BindResource(ShaderStage stage,
                                DescriptorType type,
                                const SampledImageSlot& slot,
                                const ShaderMetadata& metadata,
                                std::shared_ptr<const Texture> texture,
                                const std::unique_ptr<const Sampler>& sampler) {
  if (bound_buffer_offset_ >= kMaxBindings) {
    return false;
  }
  if (!texture->IsValid() || !sampler) {
    return false;
  }
  const TextureVK& texture_vk = TextureVK::Cast(*texture);
  const SamplerVK& sampler_vk = SamplerVK::Cast(*sampler);

  if (!command_buffer_->GetEncoder()->Track(texture)) {
    return false;
  }

  vk::DescriptorImageInfo image_info;
  image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  image_info.sampler = sampler_vk.GetSampler();
  image_info.imageView = texture_vk.GetImageView();
  image_workspace_[bound_image_offset_++] = image_info;

  vk::WriteDescriptorSet write_set;
  write_set.dstBinding = slot.binding;
  write_set.descriptorCount = 1u;
  write_set.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write_set.pImageInfo = &image_workspace_[bound_image_offset_ - 1];

  write_workspace_[descriptor_write_offset_++] = write_set;
  return true;
}

bool RenderPassVK::OnEncodeCommands(const Context& context) const {
  command_buffer_->GetEncoder()->GetCommandBuffer().endRenderPass();

  // If this render target will be consumed by a subsequent render pass,
  // perform a layout transition to a shader read state.
  const std::shared_ptr<Texture>& result_texture =
      resolve_image_vk_ ? resolve_image_vk_ : color_image_vk_;
  if (result_texture->GetTextureDescriptor().usage &
      static_cast<TextureUsageMask>(TextureUsage::kShaderRead)) {
    BarrierVK barrier;
    barrier.cmd_buffer = command_buffer_vk_;
    barrier.src_access = vk::AccessFlagBits::eColorAttachmentWrite |
                         vk::AccessFlagBits::eTransferWrite;
    barrier.src_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits::eTransfer;
    barrier.dst_access = vk::AccessFlagBits::eShaderRead;
    barrier.dst_stage = vk::PipelineStageFlagBits::eFragmentShader;

    barrier.new_layout = vk::ImageLayout::eShaderReadOnlyOptimal;

    if (!TextureVK::Cast(*result_texture).SetLayout(barrier)) {
      return false;
    }
  }

  return true;
}

}  // namespace impeller
