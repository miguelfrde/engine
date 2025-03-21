// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "impeller/core/formats.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/entity/contents/test/recording_render_pass.h"
#include "impeller/entity/contents/tiled_texture_contents.h"
#include "impeller/entity/entity_playground.h"
#include "impeller/playground/playground_test.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace impeller {
namespace testing {

using EntityTest = EntityPlayground;

TEST_P(EntityTest, TiledTextureContentsRendersWithCorrectPipeline) {
  TextureDescriptor texture_desc;
  texture_desc.size = {100, 100};
  texture_desc.type = TextureType::kTexture2D;
  texture_desc.format = PixelFormat::kR8G8B8A8UNormInt;
  texture_desc.storage_mode = StorageMode::kDevicePrivate;
  auto texture =
      GetContext()->GetResourceAllocator()->CreateTexture(texture_desc);

  TiledTextureContents contents;
  contents.SetTexture(texture);
  contents.SetGeometry(Geometry::MakeCover());

  auto content_context = GetContentContext();
  auto buffer = content_context->GetContext()->CreateCommandBuffer();
  auto render_target = RenderTarget::CreateOffscreenMSAA(
      *content_context->GetContext(),
      *GetContentContext()->GetRenderTargetCache(), {100, 100},
      /*mip_count=*/1);
  auto render_pass = buffer->CreateRenderPass(render_target);
  auto recording_pass = std::make_shared<RecordingRenderPass>(
      render_pass, GetContext(), render_target);

  ASSERT_TRUE(contents.Render(*GetContentContext(), {}, *recording_pass));
  const std::vector<Command>& commands = recording_pass->GetCommands();

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_TRUE(commands[0].pipeline->GetDescriptor().GetLabel().find(
                  "TextureFill Pipeline") != std::string::npos);

  if (GetParam() == PlaygroundBackend::kMetal) {
    recording_pass->EncodeCommands();
  }
}

// GL_OES_EGL_image_external isn't supported on MacOS hosts.
#if !defined(FML_OS_MACOSX)
TEST_P(EntityTest, TiledTextureContentsRendersWithCorrectPipelineExternalOES) {
  if (GetParam() != PlaygroundBackend::kOpenGLES) {
    GTEST_SKIP_(
        "External OES textures are only valid for the OpenGLES backend.");
  }

  TextureDescriptor texture_desc;
  texture_desc.size = {100, 100};
  texture_desc.type = TextureType::kTextureExternalOES;
  texture_desc.format = PixelFormat::kR8G8B8A8UNormInt;
  texture_desc.storage_mode = StorageMode::kDevicePrivate;
  auto texture =
      GetContext()->GetResourceAllocator()->CreateTexture(texture_desc);

  TiledTextureContents contents;
  contents.SetTexture(texture);
  contents.SetGeometry(Geometry::MakeCover());

  auto content_context = GetContentContext();
  auto buffer = content_context->GetContext()->CreateCommandBuffer();
  auto render_target = RenderTarget::CreateOffscreenMSAA(
      *content_context->GetContext(),
      *GetContentContext()->GetRenderTargetCache(), {100, 100},
      /*mip_count=*/1);
  auto render_pass = buffer->CreateRenderPass(render_target);

  ASSERT_TRUE(contents.Render(*GetContentContext(), {}, *render_pass));
  const std::vector<Command>& commands = render_pass->GetCommands();

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_TRUE(commands[0].pipeline->GetDescriptor().GetLabel().find(
                  "TiledTextureFillExternal Pipeline") != std::string::npos);
}
#endif

}  // namespace testing
}  // namespace impeller
