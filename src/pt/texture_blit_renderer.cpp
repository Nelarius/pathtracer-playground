#include "gpu_context.hpp"
#include "gui.hpp"
#include "shader_source.hpp"
#include "texture_blit_renderer.hpp"
#include "webgpu_utils.hpp"
#include "window.hpp"

#include <common/assert.hpp>

#include <fmt/core.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace nlrs
{
TextureBlitRenderer::TextureBlitRenderer(
    const GpuContext&                    gpuContext,
    const TextureBlitRendererDescriptor& desc)
    : mVertexBuffer(
          gpuContext.device,
          "Vertex buffer",
          {GpuBufferUsage::Vertex, GpuBufferUsage::CopyDst},
          std::span<const float[2]>(quadVertexData)),
      mTexture(nullptr),
      mTextureView(nullptr),
      mSampler(nullptr),
      mTextureBindGroupLayout(),
      mTextureBindGroup(),
      mPipeline(nullptr)
{
    // color attachment texture

    constexpr WGPUTextureFormat TEXTURE_FORMAT = Window::SWAP_CHAIN_FORMAT;

    mTexture = [&gpuContext, &desc]() -> WGPUTexture {
        const std::array<const WGPUTextureFormat, 1> viewFormats{TEXTURE_FORMAT};

        const WGPUTextureDescriptor textureDesc{
            .nextInChain = nullptr,
            .label = "Offscreen texture",
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
            .dimension = WGPUTextureDimension_2D,
            .size = {desc.framebufferSize.x, desc.framebufferSize.y, 1},
            .format = TEXTURE_FORMAT,
            .mipLevelCount = 1,
            .sampleCount = 1,
            .viewFormatCount = viewFormats.size(),
            .viewFormats = viewFormats.data(),
        };
        return wgpuDeviceCreateTexture(gpuContext.device, &textureDesc);
    }();
    if (!mTexture)
    {
        throw std::runtime_error("Failed to create TextureBlitRenderer color texture");
    }

    mTextureView = [this]() -> WGPUTextureView {
        const WGPUTextureViewDescriptor textureViewDesc{
            .nextInChain = nullptr,
            .label = "Offscreen texture view",
            .format = TEXTURE_FORMAT,
            .dimension = WGPUTextureViewDimension_2D,
            .baseMipLevel = 0,
            .mipLevelCount = 1,
            .baseArrayLayer = 0,
            .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All,
        };
        return wgpuTextureCreateView(mTexture, &textureViewDesc);
    }();
    NLRS_ASSERT(mTextureView != nullptr);

    {
        const WGPUSamplerDescriptor samplerDesc{
            .nextInChain = nullptr,
            .label = "Offscreen sampler",
            .addressModeU = WGPUAddressMode_ClampToEdge,
            .addressModeV = WGPUAddressMode_ClampToEdge,
            .addressModeW = WGPUAddressMode_ClampToEdge,
            .magFilter = WGPUFilterMode_Nearest,
            .minFilter = WGPUFilterMode_Nearest,
            .mipmapFilter = WGPUMipmapFilterMode_Nearest,
            .lodMinClamp = 0.f,
            .lodMaxClamp = 32.f,
            .compare = WGPUCompareFunction_Undefined,
            .maxAnisotropy = 1,
        };
        mSampler = wgpuDeviceCreateSampler(gpuContext.device, &samplerDesc);
        NLRS_ASSERT(mSampler != nullptr);
    }

    // texture bind group
    {
        std::array<const WGPUBindGroupLayoutEntry, 2> textureBindGroupLayoutEntries{
            textureBindGroupLayoutEntry(0, WGPUTextureSampleType_Float),
            samplerBindGroupLayoutEntry(1, WGPUSamplerBindingType_Filtering)};
        mTextureBindGroupLayout = GpuBindGroupLayout{
            gpuContext.device, "Texture bind group layout", textureBindGroupLayoutEntries};

        const std::array<WGPUBindGroupEntry, 2> textureBindGroupEntries{
            textureBindGroupEntry(0, mTextureView),
            samplerBindGroupEntry(1, mSampler),
        };
        mTextureBindGroup = GpuBindGroup(
            gpuContext.device,
            "Texture bind group",
            mTextureBindGroupLayout.ptr(),
            std::span<const WGPUBindGroupEntry>(textureBindGroupEntries));
    }

    {
        // Blend state for color target

        const WGPUBlendState blendState{
            .color =
                WGPUBlendComponent{
                    .operation = WGPUBlendOperation_Add,
                    .srcFactor = WGPUBlendFactor_One,
                    .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
                },
            .alpha =
                WGPUBlendComponent{
                    .operation = WGPUBlendOperation_Add,
                    .srcFactor = WGPUBlendFactor_Zero,
                    .dstFactor = WGPUBlendFactor_One,
                },
        };

        // Color target information for fragment state

        const WGPUColorTargetState colorTarget{
            .nextInChain = nullptr,
            .format = Window::SWAP_CHAIN_FORMAT,
            .blend = &blendState,
            // We could write to only some of the color channels.
            .writeMask = WGPUColorWriteMask_All,
        };

        // Shader modules

        const WGPUShaderModuleWGSLDescriptor shaderCodeDesc = {
            .chain =
                WGPUChainedStruct{
                    .next = nullptr,
                    .sType = WGPUSType_ShaderModuleWGSLDescriptor,
                },
            .code = TEXTURE_BLIT_SOURCE,
        };

        const WGPUShaderModuleDescriptor shaderDesc{
            .nextInChain = &shaderCodeDesc.chain,
            .label = "Texture blitter shader",
        };

        const WGPUShaderModule shaderModule =
            wgpuDeviceCreateShaderModule(gpuContext.device, &shaderDesc);

        const WGPUFragmentState fragmentState{
            .nextInChain = nullptr,
            .module = shaderModule,
            .entryPoint = "fsMain",
            .constantCount = 0,
            .constants = nullptr,
            .targetCount = 1,
            .targets = &colorTarget,
        };

        // Vertex layout

        const std::array<WGPUVertexAttribute, 1> vertexAttributes{WGPUVertexAttribute{
            // position
            .format = WGPUVertexFormat_Float32x2,
            .offset = 0,
            .shaderLocation = 0,
        }};

        const WGPUVertexBufferLayout vertexBufferLayout{
            .arrayStride = sizeof(float[2]),
            .stepMode = WGPUVertexStepMode_Vertex,
            .attributeCount = vertexAttributes.size(),
            .attributes = vertexAttributes.data(),
        };

        // pipeline layout

        const std::array<WGPUBindGroupLayout, 1> bindGroupLayouts{mTextureBindGroupLayout.ptr()};

        const WGPUPipelineLayoutDescriptor pipelineLayoutDesc{
            .nextInChain = nullptr,
            .label = "Pipeline layout",
            .bindGroupLayoutCount = bindGroupLayouts.size(),
            .bindGroupLayouts = bindGroupLayouts.data(),
        };

        const WGPUPipelineLayout pipelineLayout =
            wgpuDeviceCreatePipelineLayout(gpuContext.device, &pipelineLayoutDesc);

        // pipeline

        const WGPURenderPipelineDescriptor pipelineDesc{
            .nextInChain = nullptr,
            .label = "Render pipeline",
            .layout = pipelineLayout,
            .vertex =
                WGPUVertexState{
                    .nextInChain = nullptr,
                    .module = shaderModule,
                    .entryPoint = "vsMain",
                    .constantCount = 0,
                    .constants = nullptr,
                    .bufferCount = 1,
                    .buffers = &vertexBufferLayout,
                },
            // NOTE: the primitive assembly config, defines how the primitive assembly and
            // rasterization stages are configured.
            .primitive =
                WGPUPrimitiveState{
                    .nextInChain = nullptr,
                    .topology = WGPUPrimitiveTopology_TriangleList,
                    .stripIndexFormat = WGPUIndexFormat_Undefined,
                    .frontFace = WGPUFrontFace_CCW,
                    .cullMode = WGPUCullMode_Back,
                },
            .depthStencil = nullptr,
            .multisample =
                WGPUMultisampleState{
                    .nextInChain = nullptr,
                    .count = 1,
                    .mask = ~0u,
                    .alphaToCoverageEnabled = false,
                },
            // NOTE: the fragment state is a potentially null value.
            .fragment = &fragmentState,
        };

        mPipeline = wgpuDeviceCreateRenderPipeline(gpuContext.device, &pipelineDesc);

        wgpuPipelineLayoutRelease(pipelineLayout);
    }
}

TextureBlitRenderer::~TextureBlitRenderer()
{
    renderPipelineSafeRelease(mPipeline);
    mPipeline = nullptr;
    samplerSafeRelease(mSampler);
    mSampler = nullptr;
    textureViewSafeRelease(mTextureView);
    mTextureView = nullptr;
    textureSafeRelease(mTexture);
    mTexture = nullptr;
}

void TextureBlitRenderer::render(
    const GpuContext&   gpuContext,
    Gui&                gui,
    const WGPUSwapChain swapChain)
{
    // Non-standard Dawn way to ensure that Dawn ticks pending async operations.
    wgpuDeviceTick(gpuContext.device);

    const WGPUTextureView nextTexture = wgpuSwapChainGetCurrentTextureView(swapChain);
    if (!nextTexture)
    {
        // Getting the next texture can fail, if e.g. the window has been resized.
        std::fprintf(stderr, "Failed to get texture view from swap chain\n");
        return;
    }

    const WGPUCommandEncoder encoder = [&gpuContext]() {
        const WGPUCommandEncoderDescriptor cmdEncoderDesc{
            .nextInChain = nullptr,
            .label = "Command encoder",
        };
        return wgpuDeviceCreateCommandEncoder(gpuContext.device, &cmdEncoderDesc);
    }();

    const WGPURenderPassEncoder renderPassEncoder = [encoder,
                                                     nextTexture]() -> WGPURenderPassEncoder {
        const WGPURenderPassColorAttachment renderPassColorAttachment{
            .nextInChain = nullptr,
            .view = nextTexture,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, // depthSlice must be initialized with
                                                      // 'undefined' value for 2d color attachments.
            .resolveTarget = nullptr,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0},
        };

        const WGPURenderPassDescriptor renderPassDesc = {
            .nextInChain = nullptr,
            .label = "Render pass encoder",
            .colorAttachmentCount = 1,
            .colorAttachments = &renderPassColorAttachment,
            .depthStencilAttachment = nullptr,
            .occlusionQuerySet = nullptr,
            .timestampWrites = nullptr,
        };

        return wgpuCommandEncoderBeginRenderPass(encoder, &renderPassDesc);
    }();

    {
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, mPipeline);
        wgpuRenderPassEncoderSetBindGroup(
            renderPassEncoder, 0, mTextureBindGroup.ptr(), 0, nullptr);
        wgpuRenderPassEncoderSetVertexBuffer(
            renderPassEncoder, 0, mVertexBuffer.ptr(), 0, mVertexBuffer.byteSize());
        wgpuRenderPassEncoderDraw(renderPassEncoder, 6, 1, 0, 0);
    }

    gui.render(renderPassEncoder);

    wgpuRenderPassEncoderEnd(renderPassEncoder);

    const WGPUCommandBuffer cmdBuffer = [encoder]() {
        const WGPUCommandBufferDescriptor cmdBufferDesc{
            .nextInChain = nullptr,
            .label = "TextureBlitRenderer command buffer",
        };
        return wgpuCommandEncoderFinish(encoder, &cmdBufferDesc);
    }();
    wgpuQueueSubmit(gpuContext.queue, 1, &cmdBuffer);

    wgpuTextureViewRelease(nextTexture);
}

void TextureBlitRenderer::resize(const GpuContext& gpuContext, const Extent2u& newSize)
{
    textureSafeRelease(mTexture);
    textureViewSafeRelease(mTextureView);

    mTexture = nullptr;
    mTextureView = nullptr;

    {
        const WGPUTextureDescriptor textureDesc{
            .nextInChain = nullptr,
            .label = "Offscreen texture",
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
            .dimension = WGPUTextureDimension_2D,
            .size = {newSize.x, newSize.y, 1u},
            .format = Window::SWAP_CHAIN_FORMAT,
            .mipLevelCount = 1,
            .sampleCount = 1,
            .viewFormatCount = 1,
            .viewFormats = &Window::SWAP_CHAIN_FORMAT,
        };
        mTexture = wgpuDeviceCreateTexture(gpuContext.device, &textureDesc);
        NLRS_ASSERT(mTexture != nullptr);
    }

    {
        const WGPUTextureViewDescriptor textureViewDesc{
            .nextInChain = nullptr,
            .label = "Offscreen texture view",
            .format = Window::SWAP_CHAIN_FORMAT,
            .dimension = WGPUTextureViewDimension_2D,
            .baseMipLevel = 0,
            .mipLevelCount = 1,
            .baseArrayLayer = 0,
            .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All,
        };
        mTextureView = wgpuTextureCreateView(mTexture, &textureViewDesc);
        NLRS_ASSERT(mTextureView != nullptr);
    }

    {
        const std::array<WGPUBindGroupEntry, 2> textureBindGroupEntries{
            textureBindGroupEntry(0, mTextureView),
            samplerBindGroupEntry(1, mSampler),
        };
        mTextureBindGroup = GpuBindGroup{
            gpuContext.device,
            "Texture bind group",
            mTextureBindGroupLayout.ptr(),
            textureBindGroupEntries};
    }
}
} // namespace nlrs
