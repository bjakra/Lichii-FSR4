// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4 through AMD's FidelityFX runtime. The runtime takes plain Vulkan images: the inputs
// in SHADER_READ_ONLY_OPTIMAL (FFX "compute read"), the output in GENERAL ("unordered access"),
// and records into our command buffer. Picking the FSR 4 provider by version follows
// CheezerWheezer's FSR 4 integration for CheezyEmu.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/container/static_vector.hpp>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ffx_runtime.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

// After vulkan.h, which sets the platform macros the wrapper's Win32 entry points need.
#ifndef _WIN32
#define __declspec(x)
#endif
#include "api/include/vk/ffx_api_vk.h"
#include "upscalers/include/ffx_upscale.h"
#ifndef _WIN32
#undef __declspec
#endif

namespace Vulkan {
namespace {
constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageSubresourceRange COLOR_RANGE{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

// The output check (adapted from CheezyEmu): an 8x8 grid of RGBA16F input and output pixels.
using CheckTexel = std::array<u16, 4>;
constexpr u32 CHECK_GRID = 8;
constexpr u32 CHECK_PIXELS = CHECK_GRID * CHECK_GRID;
constexpr VkDeviceSize CHECK_OUTPUT_OFFSET = CHECK_PIXELS * sizeof(CheckTexel);
constexpr VkDeviceSize CHECK_SIZE = 2 * CHECK_OUTPUT_OFFSET;
constexpr u32 CHECK_INTERVAL = 30;   ///< Frames between checks.
constexpr u32 CHECK_BLACK_LIMIT = 8; ///< Black checks in a row (about 4 to 8 s) to give up.
constexpr u32 CHECK_GOOD_LIMIT = 4;  ///< Good checks to stop checking.

std::array<VkBufferImageCopy, CHECK_PIXELS> CheckRegions(const VkImageSubresourceRange& range,
                                                         VkExtent2D size, VkDeviceSize base) {
    std::array<VkBufferImageCopy, CHECK_PIXELS> regions{};
    for (u32 y = 0; y < CHECK_GRID; ++y) {
        for (u32 x = 0; x < CHECK_GRID; ++x) {
            const u32 i = y * CHECK_GRID + x;
            regions[i] = VkBufferImageCopy{
                .bufferOffset = base + i * sizeof(CheckTexel),
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = range.baseMipLevel,
                    .baseArrayLayer = range.baseArrayLayer,
                    .layerCount = 1,
                },
                .imageOffset{static_cast<s32>((2 * x + 1) * size.width / (2 * CHECK_GRID)),
                             static_cast<s32>((2 * y + 1) * size.height / (2 * CHECK_GRID)), 0},
                .imageExtent{1, 1, 1},
            };
        }
    }
    return regions;
}

FfxApiResource Resource(VkImage image, VkFormat format, VkExtent2D size, u32 usage, u32 state) {
    FfxApiResource resource{};
    resource.resource = reinterpret_cast<void*>(image);
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = ffxApiGetSurfaceFormatVK(format);
    resource.description.width = size.width;
    resource.description.height = size.height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.flags = 0;
    resource.description.usage = usage;
    resource.state = state;
    return resource;
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageSubresourceRange range, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
}

VkImageMemoryBarrier Barrier(const Fsr4Upscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return Barrier(image.image, range, src_access, dst_access, old_layout, new_layout);
}

void Message(uint32_t type, const wchar_t* message) {
    std::string text;
    for (const wchar_t* c = message; c && *c; ++c) {
        text.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "FSR 4: {}", text);
    } else {
        LOG_INFO(Render_Vulkan, "FSR 4: {}", text);
    }
}

VkDeviceContext DeviceContext(const Device& device) {
    return VkDeviceContext{
        .vkDevice = *device.GetLogical(),
        .vkPhysicalDevice = static_cast<VkPhysicalDevice>(device.GetPhysical()),
        .vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr,
    };
}

s64 SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Provider {
    u64 id{};
    std::string name;
};

#if defined(__linux__) && !defined(__ANDROID__)
/// The fsr4-vk model CheckFsr4Port picked for the device's enabled features. fsr4-vk would pick
/// by what the GPU supports, which can include features this device doesn't enable.
std::optional<Provider> FindFsr4(const FfxRuntime::Api& api, const Device& device) {
    const VideoCommon::Fsr4PortSupport support =
        VideoCommon::CheckFsr4Port(device.Fsr4PortDevice());
    if (!support.model) {
        return std::nullopt;
    }
    const std::string_view suffix =
        *support.model == VideoCommon::Fsr4PortModel::Fp8 ? " FP8" : " INT8";
    VkDeviceContext vk_context = DeviceContext(device);
    u64 count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &vk_context;
    versions.outputCount = &count;
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
        LOG_WARNING(Render_Vulkan, "FSR 4: fsr4-vk offers no upscaler; using FSR 3.1");
        return std::nullopt;
    }
    std::vector<u64> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        return std::nullopt;
    }
    for (u64 i = 0; i < count; ++i) {
        const std::string_view name = names[i] ? names[i] : "";
        if (name.starts_with('4') && name.ends_with(suffix)) {
            LOG_INFO(Render_Vulkan,
                     "FSR 4 (fsr4-vk community Vulkan port): using its {} model on {}: {}", name,
                     device.GetModelName(), support.reason);
            return Provider{ids[i], std::string{name}};
        }
    }
    LOG_WARNING(Render_Vulkan, "FSR 4: fsr4-vk lacks its{} model; using FSR 3.1", suffix);
    return std::nullopt;
}
#else
/// The newest FSR 4 (or later) upscaler AMD's runtime offers for this device. The runtime also
/// offers FSR 3.1 on GPUs without FSR 4; that one is left to the built-in FSR 3.1.
std::optional<Provider> FindFsr4(const FfxRuntime::Api& api, const Device& device) {
    VkDeviceContext vk_context = DeviceContext(device);
    u64 count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &vk_context;
    versions.outputCount = &count;
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
        LOG_INFO(Render_Vulkan, "AMD FSR 4: the FidelityFX runtime offers no upscaler for {}",
                 device.GetModelName());
        return std::nullopt;
    }
    std::vector<u64> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        return std::nullopt;
    }
    std::string offered;
    std::optional<Provider> fsr4;
    for (u64 i = 0; i < count; ++i) {
        const std::string name = names[i] ? names[i] : "";
        offered += (offered.empty() ? "" : ", ") + name;
        // Newest first.
        if (!fsr4 && !name.empty() && name[0] >= '4' && name[0] <= '9') {
            fsr4 = Provider{ids[i], name};
        }
    }
    LOG_INFO(Render_Vulkan, "AMD FSR 4: upscalers offered for {}: {}{}", device.GetModelName(),
             offered, fsr4 ? "" : " (no FSR 4; using FSR 3.1)");
    return fsr4;
}
#endif
} // Anonymous namespace

bool Fsr4Upscaler::IsWanted(const Device& device) {
#if defined(__linux__) && !defined(__ANDROID__)
    if (!Settings::values.amd_fsr4.GetValue()) {
        return false;
    }
    const VideoCommon::Fsr4PortSupport support =
        VideoCommon::CheckFsr4Port(device.Fsr4PortDevice());
    if (!support.model) {
        LOG_INFO(Render_Vulkan,
                 "FSR 4 (fsr4-vk community Vulkan port) can't run on {} ({}): {}; using FSR 3.1",
                 device.GetModelName(), device.GetDriverName(), support.reason);
    }
    return support.model.has_value();
#else
    return Settings::values.amd_fsr4.GetValue() &&
           device.GetDriverID() == VK_DRIVER_ID_AMD_PROPRIETARY;
#endif
}

Fsr4Upscaler::Fsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      api{FfxRuntime::Acquire()} {
    const auto provider = api ? FindFsr4(*api, device) : std::nullopt;
    if (!provider) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    provider_id = provider->id;
    provider_name = provider->name;
    CreateExposure();
}

Fsr4Upscaler::~Fsr4Upscaler() {
    DestroyContext();
}

void Fsr4Upscaler::DestroyContext() {
    if (context == nullptr && !output_image) {
        return;
    }
    scheduler.Finish(); // recorded dispatches and passes sampling the output may still use them
    if (context != nullptr) {
        ffxContext ctx = context;
        api->destroy_context(&ctx, nullptr);
        context = nullptr;
    }
    output_view = vk::ImageView{};
    output_image = vk::Image{};
}

u32 Fsr4Upscaler::CreateContext(VkExtent2D render_size, VkExtent2D upscale_size) {
    DestroyContext();
    ffxCreateBackendVKDesc backend{};
#if defined(__linux__)
    // The pinned fsr4-vk community port recognizes the legacy Vulkan backend descriptor
    // type (3). The current SDK puts the same three device fields in a nested struct.
    static_assert(sizeof(ffxCreateBackendVKDesc) == sizeof(ffxCreateBackendVKDesc_DEPRECATED));
    static_assert(offsetof(ffxCreateBackendVKDesc, deviceContext) ==
                  offsetof(ffxCreateBackendVKDesc_DEPRECATED, vkDevice));
    static_assert(offsetof(ffxCreateBackendVKDesc, deviceContext) +
                      offsetof(VkDeviceContext, vkPhysicalDevice) ==
                  offsetof(ffxCreateBackendVKDesc_DEPRECATED, vkPhysicalDevice));
    static_assert(offsetof(ffxCreateBackendVKDesc, deviceContext) +
                      offsetof(VkDeviceContext, vkDeviceProcAddr) ==
                  offsetof(ffxCreateBackendVKDesc_DEPRECATED, vkDeviceProcAddr));
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
#else
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
#endif
    backend.header.pNext = nullptr;
    backend.deviceContext = DeviceContext(device);
    ffxOverrideVersion version_override{};
    version_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version_override.header.pNext = &backend.header;
    version_override.versionId = provider_id;
    ffxCreateContextDescUpscaleVersion api_version{};
    api_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    api_version.header.pNext = &version_override.header;
    api_version.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &api_version.header;
    // Tonemapped LDR colour, standard (not reversed) finite depth, exposure given (unit).
    create.flags = 0;
    create.maxRenderSize = {render_size.width, render_size.height};
    create.maxUpscaleSize = {upscale_size.width, upscale_size.height};
    create.fpMessage = Message;
    ffxContext ctx{};
    const ffxReturnCode_t result = api->create_context(&ctx, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "FSR 4: ffxCreateContext {}x{} -> {}x{} failed ({}); using FSR 3.1",
                  render_size.width, render_size.height, upscale_size.width, upscale_size.height,
                  result);
        return result;
    }
    context = ctx;
    render_extent = render_size;
    upscale_extent = upscale_size;
#if defined(__linux__) && !defined(__ANDROID__)
    LOG_INFO(Render_Vulkan, "FSR 4: fsr4-vk {} context {}x{} -> {}x{}", provider_name,
             render_size.width, render_size.height, upscale_size.width, upscale_size.height);
#else
    LOG_INFO(Render_Vulkan, "FSR 4: AMD FSR {} context {}x{} -> {}x{}", provider_name,
             render_size.width, render_size.height, upscale_size.width, upscale_size.height);
#endif
    return FFX_API_RETURN_OK;
}

void Fsr4Upscaler::CreateExposure() {
    exposure_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *exposure_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0,
                               Barrier(image, COLOR_RANGE, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        VkClearColorValue one{};
        one.float32[0] = 1.0f;
        cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, COLOR_RANGE);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               Barrier(image, COLOR_RANGE, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_ACCESS_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    });
}

void Fsr4Upscaler::CreateOutput(VkExtent2D upscale_size) {
    // CreateContext's DestroyContext has freed the old output.
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange = COLOR_RANGE,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               Barrier(image, COLOR_RANGE, 0,
                                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
    });
}

bool Fsr4Upscaler::CheckOutput() {
    if (!check_pending || !scheduler.IsFree(check_tick)) {
        return false;
    }
    check_pending = false;
    check_buffer.Invalidate();
    const std::span<const u8> data = check_buffer.Mapped();
    std::array<CheckTexel, CHECK_PIXELS> input;
    std::array<CheckTexel, CHECK_PIXELS> output;
    std::memcpy(input.data(), data.data(), sizeof(input));
    std::memcpy(output.data(), data.data() + CHECK_OUTPUT_OFFSET, sizeof(output));
    switch (VideoCommon::CheckFsr4Output(input, output)) {
    case VideoCommon::Fsr4OutputCheck::Unjudged:
        return false;
    case VideoCommon::Fsr4OutputCheck::Picture:
        black_checks = 0;
        if (++good_checks >= CHECK_GOOD_LIMIT) {
            check_done = true;
            check_buffer = vk::Buffer{};
            LOG_INFO(Render_Vulkan, "FSR 4 ({}): output check passed", provider_name);
        }
        return false;
    case VideoCommon::Fsr4OutputCheck::Black:
        ++black_checks;
        LOG_WARNING(Render_Vulkan, "FSR 4 ({}): black output over a varied picture ({} of {})",
                    provider_name, black_checks, CHECK_BLACK_LIMIT);
        return black_checks >= CHECK_BLACK_LIMIT;
    }
    return false;
}

bool Fsr4Upscaler::Failed() const noexcept {
    if (!failed.load()) {
        return false;
    }
    const s64 retry = retry_at_ns.load();
    return retry == 0 || SteadyNowNs() < retry;
}

bool Fsr4Upscaler::Upscale(const Inputs& in) {
    if (failed.load()) {
        if (Failed()) {
            return false;
        }
        // Time for another try at a context that couldn't be created.
        failed = false;
        retry_at_ns = 0;
    }
    if (!check_done && CheckOutput()) {
        LOG_ERROR(Render_Vulkan, "FSR 4 ({}) draws a black picture on {} ({}); using FSR 3.1",
                  provider_name, device.GetModelName(), device.GetDriverName());
        DestroyContext();
        check_done = true;
        check_buffer = vk::Buffer{};
        failed = true;
        return false;
    }
    const bool new_context = context == nullptr || in.render_size.width != render_extent.width ||
                             in.render_size.height != render_extent.height ||
                             in.upscale_size.width != upscale_extent.width ||
                             in.upscale_size.height != upscale_extent.height;
    if (new_context) {
        const u32 result = CreateContext(in.render_size, in.upscale_size);
        if (result != FFX_API_RETURN_OK) {
            // Out of video memory may pass (a scene loading is when it's shortest), so FSR 3.1
            // draws meanwhile and FSR 4 is tried again. It comes as a runtime or memory error, but
            // so do causes that won't pass: fsr4-vk returns a runtime error for every rejection
            // (the reason is only in its fsr4.log). Hence only two tries. A dispatch failure
            // found while the old context drained stays final.
            const bool may_pass = result == FFX_API_RETURN_ERROR_RUNTIME_ERROR ||
                                  result == FFX_API_RETURN_ERROR_MEMORY;
            const std::optional<u32> delay = may_pass && !failed.load()
                                                 ? VideoCommon::Fsr4RetryDelay(++create_failures)
                                                 : std::nullopt;
            if (delay) {
                LOG_WARNING(Render_Vulkan, "FSR 4: trying to start it again in {} s", *delay);
                retry_at_ns = SteadyNowNs() + s64{*delay} * 1'000'000'000;
            }
            failed = true;
            return false;
        }
        create_failures = 0;
        CreateOutput(in.upscale_size);
        // Check the new context's output from the start. A pending check read the old one, and
        // DestroyContext has waited for it.
        check_pending = false;
        check_done = false;
        check_frame = 0;
        good_checks = 0;
        black_checks = 0;
    }

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.header.pNext = nullptr;
    dispatch.color = Resource(in.color.image, in.color.format, in.render_size, 0,
                              FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = Resource(in.depth.image, in.depth.format, in.render_size,
                              FFX_API_RESOURCE_USAGE_DEPTHTARGET,
                              FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = Resource(in.motion_vectors.image, in.motion_vectors.format,
                                      in.render_size, 0, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.exposure = Resource(*exposure_image, VK_FORMAT_R32_SFLOAT, {1, 1}, 0,
                                 FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (in.reactive) {
        dispatch.reactive = Resource(in.reactive->image, in.reactive->format, in.render_size, 0,
                                     FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }
    dispatch.output = Resource(*output_image, OUTPUT_FORMAT, in.upscale_size,
                               FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {in.jitter[0], in.jitter[1]};
    dispatch.motionVectorScale = {1.0f, 1.0f};
    dispatch.renderSize = {in.render_size.width, in.render_size.height};
    dispatch.upscaleSize = {in.upscale_size.width, in.upscale_size.height};
    dispatch.enableSharpening = in.sharpen;
    dispatch.sharpness = in.sharpness;
    dispatch.frameTimeDelta = in.frame_time_ms;
    dispatch.preExposure = 1.0f;
    dispatch.reset = in.reset || new_context;
    dispatch.cameraNear = in.camera_near;
    dispatch.cameraFar = in.camera_far;
    dispatch.cameraFovAngleVertical = in.vertical_fov;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = 0;

    VkBuffer check = VK_NULL_HANDLE;
    if (!check_done && !check_pending && ++check_frame % CHECK_INTERVAL == 0) {
        if (!check_buffer) {
            check_buffer = memory_allocator.CreateBuffer(
                VkBufferCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = CHECK_SIZE,
                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                },
                MemoryUsage::Download);
        }
        check = *check_buffer;
        check_pending = true;
        check_tick = scheduler.CurrentTick();
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, dispatch, check, output = *output_image](vk::CommandBuffer cmdbuf) {
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs stay in GENERAL otherwise; FSR 4 reads them as sampled images.
        boost::container::static_vector<VkImageMemoryBarrier, 5> to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(output, COLOR_RANGE, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.reactive) {
            to_read.push_back(Barrier(*in.reactive, WRITES, VK_ACCESS_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        ffxDispatchDescUpscale recorded = dispatch;
        recorded.commandList = static_cast<void*>(*cmdbuf);
        ffxContext ctx = context;
        const ffxReturnCode_t result = api->dispatch(&ctx, &recorded.header);
        if (result != FFX_API_RETURN_OK && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "FSR 4: ffxDispatch failed ({}); using FSR 3.1", result);
        }

        boost::container::static_vector<VkImageMemoryBarrier, 5> back{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(output, COLOR_RANGE, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.reactive) {
            back.push_back(Barrier(*in.reactive, VK_ACCESS_SHADER_READ_BIT, 0,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_IMAGE_LAYOUT_GENERAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, back);
        if (check != VK_NULL_HANDLE) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   VkMemoryBarrier{
                                       .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                       .pNext = nullptr,
                                       .srcAccessMask = WRITES,
                                       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                   });
            cmdbuf.CopyImageToBuffer(in.color.image, VK_IMAGE_LAYOUT_GENERAL, check,
                                     CheckRegions(in.color.range, in.render_size, 0));
            cmdbuf.CopyImageToBuffer(
                output, VK_IMAGE_LAYOUT_GENERAL, check,
                CheckRegions(COLOR_RANGE, in.upscale_size, CHECK_OUTPUT_OFFSET));
            // Later passes that write the colour or output wait for the copies too.
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   0,
                                   VkMemoryBarrier{
                                       .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                       .pNext = nullptr,
                                       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                   });
        }
    });
    return true;
}

} // namespace Vulkan
