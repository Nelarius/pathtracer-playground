#include "fly_camera_controller.hpp"
#include "gpu_context.hpp"
#include "gpu_limits.hpp"
#include "gui.hpp"
#include "deferred_renderer.hpp"
#include "reference_path_tracer.hpp"
#include "window.hpp"

#include <common/assert.hpp>
#include <common/bvh.hpp>
#include <common/file_stream.hpp>
#include <common/ray_intersection.hpp>
#include <common/triangle_attributes.hpp>
#include <pt-format/vertex_attributes.hpp>
#include <pt-format/pt_format.hpp>

#include <fmt/core.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <webgpu/webgpu.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <tuple>
#include <utility>

namespace fs = std::filesystem;

inline constexpr int defaultWindowWidth = 640;
inline constexpr int defaultWindowHeight = 480;

void printHelp() { std::printf("Usage:\n\tpt <input_pt_file>\n"); }

enum RendererType
{
    RendererType_PathTracer,
    RendererType_Deferred,
    RendererType_Debug,
};

struct UiState
{
    int   rendererType = RendererType_Deferred;
    float vfovDegrees = 70.0f;
    // sampling
    int numSamplesPerPixel = 64;
    int numBounces = 2;
    // sky
    float                sunZenithDegrees = 30.0f;
    float                sunAzimuthDegrees = 0.0f;
    float                skyTurbidity = 1.0f;
    std::array<float, 3> skyAlbedo = {1.0f, 1.0f, 1.0f};
    // tonemapping
    int exposureStops = 2;
};

struct AppState
{
    nlrs::FlyCameraController    cameraController;
    std::vector<nlrs::BvhNode>   bvhNodes;
    std::vector<nlrs::Positions> positions;
    UiState                      ui;
    bool                         focusPressed = false;
};

nlrs::Extent2i largestMonitorResolution()
{
    int           monitorCount;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);

    NLRS_ASSERT(monitorCount > 0);

    int            maxArea = 0;
    nlrs::Extent2i maxResolution;

    for (int i = 0; i < monitorCount; ++i)
    {
        GLFWmonitor* const monitor = monitors[i];

        float xscale, yscale;
        glfwGetMonitorContentScale(monitor, &xscale, &yscale);

        const GLFWvidmode* const mode = glfwGetVideoMode(monitor);

        const int xpixels = static_cast<int>(xscale * mode->width + 0.5f);
        const int ypixels = static_cast<int>(yscale * mode->height + 0.5f);
        const int area = xpixels * ypixels;

        if (area > maxArea)
        {
            maxArea = area;
            maxResolution = nlrs::Extent2i(xpixels, ypixels);
        }
    }

    return maxResolution;
}

int main(int argc, char** argv)
try
{
    if (argc != 2)
    {
        printHelp();
        return 0;
    }

    nlrs::GpuContext gpuContext{
        WGPURequiredLimits{.nextInChain = nullptr, .limits = nlrs::REQUIRED_LIMITS}};
    nlrs::Window window = [&gpuContext]() -> nlrs::Window {
        const nlrs::WindowDescriptor windowDesc{
            .windowSize = nlrs::Extent2i{defaultWindowWidth, defaultWindowHeight},
            .title = "pt-playground 🛝",
        };
        return nlrs::Window{windowDesc, gpuContext};
    }();

    nlrs::Gui gui(window.ptr(), gpuContext);
    auto [appState, referenceRenderer, deferredRenderer] = [&gpuContext, &window, argv]()
        -> std::tuple<AppState, nlrs::ReferencePathTracer, nlrs::DeferredRenderer> {
        nlrs::PtFormat ptFormat;
        {
            const fs::path path = argv[1];
            if (!fs::exists(path))
            {
                fmt::print(stderr, "File {} does not exist\n", path.string());
                std::exit(1);
            }
            nlrs::InputFileStream file(argv[1]);
            nlrs::deserialize(file, ptFormat);
        }

        const nlrs::Extent2i largestResolution = largestMonitorResolution();

        const nlrs::RendererDescriptor rendererDesc{
            nlrs::RenderParameters{
                nlrs::Extent2u(window.resolution()),
                nlrs::FlyCameraController{}.getCamera(),
                nlrs::SamplingParams(),
                nlrs::Sky(),
                1.0f},
            largestResolution,
        };

        nlrs::Scene scene{
            .bvhNodes = ptFormat.bvhNodes,
            .positionAttributes = ptFormat.trianglePositionAttributes,
            .vertexAttributes = ptFormat.triangleVertexAttributes,
            .baseColorTextures = ptFormat.baseColorTextures,
        };

        nlrs::ReferencePathTracer referenceRenderer{rendererDesc, gpuContext, std::move(scene)};

        nlrs::DeferredRenderer deferredRenderer{
            gpuContext,
            nlrs::DeferredRendererDescriptor{
                .framebufferSize = nlrs::Extent2u(window.resolution()),
                .maxFramebufferSize = nlrs::Extent2u(largestResolution),
                .modelPositions = ptFormat.modelVertexPositions,
                .modelNormals = ptFormat.modelVertexNormals,
                .modelTexCoords = ptFormat.modelVertexTexCoords,
                .modelIndices = ptFormat.modelVertexIndices,
                .modelBaseColorTextureIndices = ptFormat.modelBaseColorTextureIndices,
                .sceneBaseColorTextures = ptFormat.baseColorTextures,
                .sceneBvhNodes = ptFormat.bvhNodes,
                .scenePositionAttributes = ptFormat.trianglePositionAttributes,
                .sceneVertexAttributes = ptFormat.triangleVertexAttributes}};

        AppState app{
            .cameraController{},
            .bvhNodes = std::move(ptFormat.bvhNodes),
            .positions = std::move(ptFormat.bvhPositionAttributes),
            .ui = UiState{},
            .focusPressed = false,
        };

        return std::make_tuple(
            std::move(app), std::move(referenceRenderer), std::move(deferredRenderer));
    }();

    auto onNewFrame = [&gui]() -> void { gui.beginFrame(); };

    auto onUpdate = [&appState, &referenceRenderer, &deferredRenderer](
                        GLFWwindow* windowPtr, float deltaTime) -> void {
        {
            // Skip input if ImGui captured input
            if (!ImGui::GetIO().WantCaptureMouse)
            {
                appState.cameraController.update(windowPtr, deltaTime);
            }

            // Check if mouse button pressed
            if (glfwGetMouseButton(windowPtr, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS &&
                !appState.focusPressed)
            {
                appState.focusPressed = true;

                double x, y;
                glfwGetCursorPos(windowPtr, &x, &y);
                nlrs::Extent2i windowSize;
                glfwGetWindowSize(windowPtr, &windowSize.x, &windowSize.y);
                if ((x >= 0.0 && x < static_cast<double>(windowSize.x)) &&
                    (y >= 0.0 && y < static_cast<double>(windowSize.y)))
                {
                    const float u = static_cast<float>(x) / static_cast<float>(windowSize.x);
                    const float v = 1.f - static_cast<float>(y) / static_cast<float>(windowSize.y);

                    const auto camera = appState.cameraController.getCamera();
                    const auto ray = nlrs::generateCameraRay(camera, u, v);

                    nlrs::Intersection hitData;
                    if (nlrs::rayIntersectBvh(
                            ray, appState.bvhNodes, appState.positions, 1000.f, hitData, nullptr))
                    {
                        const glm::vec3 dir = hitData.p - appState.cameraController.position();
                        const glm::vec3 cameraForward =
                            appState.cameraController.orientation().forward;
                        const float focusDistance = glm::dot(dir, cameraForward);
                        appState.cameraController.focusDistance() = focusDistance;
                    }
                }
            }

            if (glfwGetMouseButton(windowPtr, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_RELEASE)
            {
                appState.focusPressed = false;
            }
        }
        {
            ImGui::Begin("pt");

            ImGui::Text("Renderer");
            ImGui::RadioButton("path tracer", &appState.ui.rendererType, RendererType_PathTracer);
            ImGui::SameLine();
            ImGui::RadioButton("deferred", &appState.ui.rendererType, RendererType_Deferred);
            ImGui::SameLine();
            ImGui::RadioButton("debug", &appState.ui.rendererType, RendererType_Debug);
            ImGui::Separator();

            ImGui::Text("Perf stats");
            {
                switch (appState.ui.rendererType)
                {
                case RendererType_PathTracer:
                {
                    const float renderAverageMs = referenceRenderer.averageRenderpassDurationMs();
                    const float progressPercentage = referenceRenderer.renderProgressPercentage();
                    ImGui::Text(
                        "render pass: %.2f ms (%.1f FPS)",
                        renderAverageMs,
                        1000.0f / renderAverageMs);
                    ImGui::Text("render progress: %.2f %%", progressPercentage);
                    break;
                }
                case RendererType_Deferred:
                {
                    const auto perfStats = deferredRenderer.getPerfStats();
                    ImGui::Text(
                        "gbuffer pass: %.2f ms (%.1f FPS)",
                        perfStats.averageGbufferPassDurationsMs,
                        1000.0f / perfStats.averageGbufferPassDurationsMs);
                    ImGui::Text(
                        "lighting pass: %.2f ms (%.1f FPS)",
                        perfStats.averageLightingPassDurationsMs,
                        1000.0f / perfStats.averageLightingPassDurationsMs);
                    ImGui::Text(
                        "resolve pass: %.2f ms (%.1f FPS)",
                        perfStats.averageResolvePassDurationsMs,
                        1000.0f / perfStats.averageResolvePassDurationsMs);
                    break;
                }
                default:
                    ImGui::Text("no perf stats available");
                }
            }

            ImGui::Separator();

            ImGui::Text("Parameters");

            ImGui::Text("num samples:");
            ImGui::SameLine();
            ImGui::RadioButton("8", &appState.ui.numSamplesPerPixel, 8);
            ImGui::SameLine();
            ImGui::RadioButton("64", &appState.ui.numSamplesPerPixel, 64);
            ImGui::SameLine();
            ImGui::RadioButton("512", &appState.ui.numSamplesPerPixel, 512);

            ImGui::Text("num bounces:");
            ImGui::SameLine();
            ImGui::RadioButton("2", &appState.ui.numBounces, 2);
            ImGui::SameLine();
            ImGui::RadioButton("4", &appState.ui.numBounces, 4);
            ImGui::SameLine();
            ImGui::RadioButton("8", &appState.ui.numBounces, 8);

            ImGui::SliderFloat("sun zenith", &appState.ui.sunZenithDegrees, 0.0f, 90.0f, "%.2f");
            ImGui::SliderFloat("sun azimuth", &appState.ui.sunAzimuthDegrees, 0.0f, 360.0f, "%.2f");
            ImGui::SliderFloat("sky turbidity", &appState.ui.skyTurbidity, 1.0f, 10.0f, "%.2f");

            ImGui::SliderFloat(
                "camera speed",
                &appState.cameraController.speed(),
                0.05f,
                100.0f,
                "%.2f",
                ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("camera vfov", &appState.ui.vfovDegrees, 10.0f, 120.0f);
            appState.cameraController.vfov() = nlrs::Angle::degrees(appState.ui.vfovDegrees);
            ImGui::SliderFloat(
                "camera focus distance",
                &appState.cameraController.focusDistance(),
                0.1f,
                50.0f,
                "%.2f",
                ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat(
                "camera lens radius", &appState.cameraController.aperture(), 0.0f, 0.5f, "%.2f");
            ImGui::SliderInt("exposure stops", &appState.ui.exposureStops, 0, 8);

            ImGui::Separator();
            ImGui::Text("Camera");
            {
                const glm::vec3 pos = appState.cameraController.position();
                const auto      yaw = appState.cameraController.yaw();
                const auto      pitch = appState.cameraController.pitch();
                ImGui::Text("position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
                ImGui::Text("yaw: %.2f", yaw.asDegrees());
                ImGui::Text("pitch: %.2f", pitch.asDegrees());
            }

            ImGui::End();
        }
    };

    auto onRender = [&appState, &gpuContext, &gui, &referenceRenderer, &deferredRenderer](
                        GLFWwindow* windowPtr, WGPUSwapChain swapChain) -> void {
        const WGPUTextureView targetTextureView = wgpuSwapChainGetCurrentTextureView(swapChain);
        if (!targetTextureView)
        {
            // Getting the next texture can fail, if e.g. the window has been resized.
            std::fprintf(stderr, "Failed to get texture view from swap chain\n");
            return;
        }

        nlrs::Extent2i windowResolution;
        glfwGetFramebufferSize(windowPtr, &windowResolution.x, &windowResolution.y);
        NLRS_ASSERT(appState.ui.exposureStops >= 0);
        const nlrs::RenderParameters renderParams{
            nlrs::Extent2u(windowResolution),
            appState.cameraController.getCamera(),
            nlrs::SamplingParams{
                static_cast<std::uint32_t>(appState.ui.numSamplesPerPixel),
                static_cast<std::uint32_t>(appState.ui.numBounces),
            },
            nlrs::Sky{
                appState.ui.skyTurbidity,
                appState.ui.skyAlbedo,
                appState.ui.sunZenithDegrees,
                appState.ui.sunAzimuthDegrees,
            },
            1.0f / std::exp2(static_cast<float>(appState.ui.exposureStops))};
        referenceRenderer.setRenderParameters(renderParams);

        switch (appState.ui.rendererType)
        {
        case RendererType_PathTracer:
            referenceRenderer.render(gpuContext, targetTextureView, gui);
            break;
        case RendererType_Deferred:
        {
            NLRS_ASSERT(appState.ui.exposureStops >= 0);
            const nlrs::RenderDescriptor renderDesc{
                appState.cameraController.viewReverseZProjectionMatrix(),
                appState.cameraController.position(),
                nlrs::Sky{
                    appState.ui.skyTurbidity,
                    appState.ui.skyAlbedo,
                    appState.ui.sunZenithDegrees,
                    appState.ui.sunAzimuthDegrees,
                },
                nlrs::Extent2u(windowResolution),
                1.0f / std::exp2(static_cast<float>(appState.ui.exposureStops)),
                targetTextureView,
            };
            deferredRenderer.render(gpuContext, renderDesc, gui);
            break;
        }
        case RendererType_Debug:
        {
            deferredRenderer.renderDebug(
                gpuContext,
                appState.cameraController.viewReverseZProjectionMatrix(),
                nlrs::Extent2f(windowResolution),
                targetTextureView,
                gui);
            break;
        }
        }

        wgpuTextureViewRelease(targetTextureView);
    };

    auto onResize = [&gpuContext, &deferredRenderer](const nlrs::FramebufferSize newSize) -> void {
        // TODO: this function is not really needed since I get the current framebuffer size on
        // each render anyway.
        const auto sz = nlrs::Extent2u(newSize);
        deferredRenderer.resize(gpuContext, sz);
    };

    window.run(
        gpuContext,
        std::move(onNewFrame),
        std::move(onUpdate),
        std::move(onRender),
        std::move(onResize));

    return 0;
}
catch (const std::exception& e)
{
    fmt::println(stderr, "Exception occurred. {}", e.what());
    return 1;
}
catch (...)
{
    fmt::println(stderr, "Unknown exception occurred.");
    return 1;
}
