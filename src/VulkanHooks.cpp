// UniversalTextureDownscaler: Vulkan implicit layer.
//
// Not a vulkan-1.dll proxy like the D3D targets: Vulkan games and bundled
// SDKs commonly link directly against dozens of vulkan-1.dll exports by
// name, so a proxy forwarding only a small "commonly used" subset breaks
// them at load time. Vulkan's spec provides the correct mechanism instead:
// an implicit layer, loaded by the real loader alongside the untouched real
// vulkan-1.dll. The vkGetDeviceProcAddr/vkGetInstanceProcAddr dispatch, the
// vkCreateInstance/vkCreateDevice layer-chain bootstrap, and the
// dispatch-key-from-handle convention below all follow the Vulkan-Loader
// Loader-and-Layer-Interface spec, which every real layer implements
// identically.
//
// vkCreateImage gets a shrunk VkImageCreateInfo. Unlike D3D11, Vulkan images
// are never populated synchronously at creation, so every reduced image
// relies on vkCmdCopyBufferToImage/vkCmdCopyImage(2), vkCmdBlitImage(2),
// vkCmdPipelineBarrier(2) and vkCreateImageView getting their subresource
// indices/ranges remapped, the same approach as the D3D12 proxy's
// CopyTextureRegion/ResourceBarrier/Barrier7 hooks. vkGetImageMemoryRequirements(2)
// takes an already-created VkImage, so it automatically reflects the reduced
// image's real footprint with no separate hook needed.
//
// Ships as UniversalTextureDownscaler_Vulkan.dll plus a same-named .json manifest
// (the file name is ours to choose; "VK_LAYER_" is the convention for the
// logical layer name inside the manifest, not the file on disk). The
// installer registers the manifest's path under
// HKLM\Software\Khronos\Vulkan\ImplicitLayers; disable_environment
// (DISABLE_UNIVERSALTEXTUREDOWNSCALER=1) turns it off for one launch without
// unregistering.

#include "Common.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Common;

namespace {
    std::once_flag g_initOnce;

    // Only active when an ini sits next to this exe, so the global implicit layer stays inert in every Vulkan app the installer didn't set up.
    std::atomic<bool> g_active{false};

    void Init() {
        std::call_once(g_initOnce, [] {
            const auto dir = HostDirectory();
            if (GetFileAttributesW((dir + L"UniversalTextureDownscaler.ini").c_str()) == INVALID_FILE_ATTRIBUTES)
                return;
            g_active.store(true, std::memory_order_relaxed);
            OpenLog(L"UniversalTextureDownscaler_Vulkan.log", dir);
            LoadConfig(dir);
        });
    }

    // Reports a broken dispatch state once per process. Repeating it at
    // command-buffer rates would bloat the log for no new information.
    void LogOnce(std::atomic<bool>& reported, const char* text) {
        if (!reported.exchange(true, std::memory_order_relaxed)) LogLine(text);
    }

    // BC-format check (Vulkan's own VkFormat, unrelated to DXGI_FORMAT)

    bool IsBlockCompressed(VkFormat format) {
        switch (format) {
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
            case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
            case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
            case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
                return true;
            default:
                return false;
        }
    }

    // desc-shape filter: everything decidable from VkImageCreateInfo
    // Mirrors the D3D11/D3D12 filters where a direct Vulkan equivalent
    // exists. Vulkan has no per-image "heap type", memory visibility is
    // chosen later at vkAllocateMemory, so tiling==OPTIMAL plus SAMPLED_BIT
    // usage (no attachment/storage bits) stands in for "plain GPU-resident
    // sampled asset": LINEAR tiling and CPU access go hand in hand in
    // practice, and BC formats mostly aren't even supported with LINEAR
    // tiling on real drivers.
    // No Enabled check here; the caller checks that separately.
    const char* RejectionReason(const VkImageCreateInfo& info) {
        if (info.imageType != VK_IMAGE_TYPE_2D) return "not-image2d";
        if (info.mipLevels <= 1) return "single-mip";
        if (info.arrayLayers != 1) return "array-or-cubemap";
        if (info.samples != VK_SAMPLE_COUNT_1_BIT) return "multisampled";
        if (info.tiling != VK_IMAGE_TILING_OPTIMAL) return "non-optimal-tiling";

        constexpr VkImageUsageFlags kRejectedUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        if (info.usage & kRejectedUsage) return "render-target-or-storage";
        if (!(info.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return "not-sampled";

        // Sparse residency/aliased both require sparse binding too (spec),
        // so checking this one flag catches every sparse/reserved image -
        // the Vulkan equivalent of D3D12's unhooked CreateReservedResource.
        // VK_IMAGE_CREATE_ALIAS_BIT means another image describes the same
        // memory with its own extent, which shrinking only one side breaks -
        // the same reason the D3D backends reject shared resources.
        constexpr VkImageCreateFlags kRejectedFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT |
            VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_ALIAS_BIT;
        if (info.flags & kRejectedFlags) return "cubemap-sparse-or-aliased";

        // An image whose memory is exported to another API or process (the
        // direct analogue of D3D11's MISC_SHARED / D3D12's HEAP_FLAG_SHARED,
        // which both backends reject): the importing side describes it
        // independently and would still describe it at the original size.
        for (const auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next != nullptr;
             next            = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO ||
                next->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_NV)
                return "external-memory";
        }

        return nullptr;
    }

    // tracking which images were actually reduced, and by how much
    // No refcounting hook needed here (unlike COM's Release): Vulkan has an
    // explicit vkDestroyImage call, intercepted below.

    std::shared_mutex g_reducedMutex;
    std::unordered_map<VkImage, std::uint32_t> g_reducedImages;

    std::uint32_t SkipFor(VkImage image) {
        if (image == VK_NULL_HANDLE) return 0;
        const std::shared_lock lock(g_reducedMutex);
        const auto it = g_reducedImages.find(image);
        return it != g_reducedImages.end() ? it->second : 0;
    }

    // logging

    void LogReduction(const VkImageCreateInfo& original, std::uint32_t skip) {
        if (skip == 0) return;

        const auto originalBytes =
            EstimateBytes(original.extent.width, original.extent.height, original.mipLevels);
        const auto reducedBytes = EstimateBytes(original.extent.width >> skip, original.extent.height >> skip,
                                                 original.mipLevels - skip);
        const auto savedBytes = originalBytes > reducedBytes ? originalBytes - reducedBytes : 0;
        RecordReduction(savedBytes);
        if (!g_verbose.load(std::memory_order_relaxed)) return;

        const std::scoped_lock lock(g_logMutex);
        if (!g_log) return;
        fprintf(g_log, "[image] %ux%u skip=%u levels=%u format=%u ~%llu KB saved\n", original.extent.width,
                original.extent.height, skip, original.mipLevels, static_cast<unsigned>(original.format),
                static_cast<unsigned long long>(savedBytes / 1024));
        fflush(g_log);
    }

    // subresource remap helpers
    // Every image this tool ever reduces has arrayLayers==1 and only the
    // COLOR aspect (render-target/depth-stencil usage is rejected above),
    // so a mip index/range is the whole story, no layer/aspect
    // multiplication, same precondition the D3D proxies rely on.

    // For barriers: a genuinely finite range that no longer intersects the
    // reduced image at all can be dropped from the batch. Returns false
    // only in that case. VK_REMAINING_MIP_LEVELS is Vulkan's dynamic "to
    // the end of the image" sentinel, resolved against the REAL image at
    // execution time (unlike a frozen count), and shifting baseMipLevel while
    // leaving the sentinel in place keeps it correct against the smaller
    // chain with no other change, and never needs to be dropped.
    bool RemapBarrierRange(VkImageSubresourceRange& range, std::uint32_t skip) {
        if (skip == 0) return true;

        if (range.levelCount == VK_REMAINING_MIP_LEVELS) {
            range.baseMipLevel = range.baseMipLevel >= skip ? range.baseMipLevel - skip : 0;
            return true;
        }

        // 64-bit so base+count can't wrap: a wrapped end would compare below
        // skip and silently drop a barrier that should still apply. The final
        // levelCount is at most the original count, so it fits back in 32 bits.
        const std::uint32_t firstMip = range.baseMipLevel;
        const std::uint64_t endMip   = static_cast<std::uint64_t>(firstMip) + range.levelCount;  // exclusive
        if (endMip <= skip) return false;  // every targeted mip was removed

        const std::uint32_t newFirst = (std::max)(firstMip, skip) - skip;
        range.baseMipLevel = newFirst;
        range.levelCount   = static_cast<std::uint32_t>(endMip - skip - newFirst);
        return true;
    }

    // Same shift, but clamps to at least 1 level instead of signaling
    // "empty": a view creation call can't be dropped, it must return some
    // valid handle. Mirrors the D3D11/D3D12 CreateShaderResourceView clamp.
    void ClampViewRange(VkImageSubresourceRange& range, std::uint32_t skip) {
        if (skip == 0) return;

        const std::uint32_t originalBase = range.baseMipLevel;
        range.baseMipLevel               = originalBase >= skip ? originalBase - skip : 0;

        if (range.levelCount != VK_REMAINING_MIP_LEVELS) {
            // 64-bit for the same reason as RemapBarrierRange: base+count must
            // not wrap. The result is bounded by the original count.
            const std::uint64_t originalEnd = static_cast<std::uint64_t>(originalBase) + range.levelCount;
            const std::uint64_t newEnd      = originalEnd >= skip ? originalEnd - skip : 0;
            range.levelCount                = newEnd > range.baseMipLevel
                                                  ? static_cast<std::uint32_t>(newEnd - range.baseMipLevel)
                                                  : 1;
        }
    }

    // For copies: a single mip index, not a range, so drop the whole region
    // if it targets a mip that no longer exists, same as the D3D12 proxy's
    // RemapCopyLocation.
    bool RemapSubresourceLayers(VkImageSubresourceLayers& layers, std::uint32_t skip) {
        if (skip == 0) return true;
        if (layers.mipLevel < skip) return false;
        layers.mipLevel -= skip;
        return true;
    }

    // dispatch-key-based per-instance/per-device state
    // The Vulkan loader writes the dispatch-table pointer as the first field
    // of every dispatchable handle (documented in the Vulkan-Loader's
    // Loader-and-Layer-Interface spec). This is what lets a VkCommandBuffer
    // resolve to the same per-device state as its owning VkDevice, even
    // though vkCmd* calls never receive the device directly.
    void* DispatchKey(const void* dispatchableHandle) {
        return *reinterpret_cast<void* const*>(dispatchableHandle);
    }

    // Finds a VkLayerInstanceCreateInfo/VkLayerDeviceCreateInfo in a
    // pCreateInfo->pNext chain, the loader's standard way of handing each
    // layer its link to the next one.
    template <typename T>
    T* FindLayerInfo(const void* chain, VkStructureType type, VkLayerFunction function) {
        auto* next = reinterpret_cast<T*>(const_cast<void*>(chain));
        while (next != nullptr && !(next->sType == type && next->function == function))
            next = reinterpret_cast<T*>(const_cast<void*>(next->pNext));
        return next;
    }

    struct InstanceState {
        VkInstance handle                               = VK_NULL_HANDLE;
        PFN_vkGetInstanceProcAddr getInstanceProcAddr   = nullptr;  // next layer/loader in the chain
        PFN_vkGetPhysicalDeviceMemoryProperties2 getMemoryProperties2 = nullptr;
    };

    std::mutex g_instancesMutex;
    std::unordered_map<void*, InstanceState> g_instances;  // keyed by DispatchKey

    struct DeviceState {
        PFN_vkGetDeviceProcAddr getDeviceProcAddr           = nullptr;  // next layer/loader in the chain
        PFN_vkDestroyDevice realDestroyDevice               = nullptr;
        PFN_vkCreateImage realCreateImage                   = nullptr;
        PFN_vkDestroyImage realDestroyImage                 = nullptr;
        PFN_vkCreateImageView realCreateImageView           = nullptr;
        PFN_vkCmdCopyBufferToImage realCopyBufferToImage    = nullptr;
        PFN_vkCmdCopyBufferToImage2 realCopyBufferToImage2  = nullptr;
        PFN_vkCmdCopyImage realCopyImage                    = nullptr;
        PFN_vkCmdCopyImage2 realCopyImage2                  = nullptr;
        PFN_vkCmdCopyImageToBuffer realCopyImageToBuffer    = nullptr;
        PFN_vkCmdCopyImageToBuffer2 realCopyImageToBuffer2  = nullptr;
        PFN_vkCmdBlitImage realBlitImage                    = nullptr;
        PFN_vkCmdBlitImage2 realBlitImage2                  = nullptr;
        PFN_vkCmdPipelineBarrier realPipelineBarrier        = nullptr;
        PFN_vkCmdPipelineBarrier2 realPipelineBarrier2      = nullptr;
    };

    // shared_mutex, not mutex: written only on device create/destroy, read on
    // every intercepted vkCmd* call from every recording thread.
    std::shared_mutex g_devicesMutex;
    std::unordered_map<void*, DeviceState> g_devices;  // keyed by DispatchKey

    // Command buffers share their owning device's dispatch key, so this
    // resolves correctly for vkCmd* hooks too, not just vkCreateImage-style
    // ones that receive a VkDevice directly.
    DeviceState* StateFor(const void* dispatchableHandle) {
        if (!dispatchableHandle) return nullptr;
        const std::shared_lock lock(g_devicesMutex);
        const auto it = g_devices.find(DispatchKey(dispatchableHandle));
        return it != g_devices.end() ? &it->second : nullptr;
    }

    // vkCreateImage / vkDestroyImage

    VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateImage(
        VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
        VkImage* pImage) {
        auto* state = StateFor(device);
        if (!state || !state->realCreateImage) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCreateImage: no down-chain function for this device, image creation failed");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const char* structuralReason = pCreateInfo ? RejectionReason(*pCreateInfo) : "no-info";
        if (structuralReason == nullptr) Heartbeat();

        const char* reason = g_enabled.load(std::memory_order_relaxed) ? structuralReason : "disabled";

        if (g_verbose.load(std::memory_order_relaxed) && pCreateInfo && pCreateInfo->mipLevels > 1) {
            const std::scoped_lock lock(g_logMutex);
            if (g_log) {
                fprintf(g_log, "[multi-mip] %ux%u mips=%u fmt=%u -> %s\n", pCreateInfo->extent.width,
                        pCreateInfo->extent.height, pCreateInfo->mipLevels,
                        static_cast<unsigned>(pCreateInfo->format), reason ? reason : "ACCEPTED");
                fflush(g_log);
            }
        }

        if (reason != nullptr) return state->realCreateImage(device, pCreateInfo, pAllocator, pImage);

        const bool bc = IsBlockCompressed(pCreateInfo->format);
        const auto skip = ComputeSkip(pCreateInfo->extent.width, pCreateInfo->extent.height, pCreateInfo->mipLevels,
                                       bc, g_maxSize.load(std::memory_order_relaxed));
        if (skip == 0) {
            return state->realCreateImage(device, pCreateInfo, pAllocator, pImage);
        }

        VkImageCreateInfo reduced = *pCreateInfo;
        reduced.extent.width      = pCreateInfo->extent.width >> skip;
        reduced.extent.height     = pCreateInfo->extent.height >> skip;
        reduced.mipLevels         = pCreateInfo->mipLevels - skip;

        const VkResult result = state->realCreateImage(device, &reduced, pAllocator, pImage);

        if (result != VK_SUCCESS) {
            // Something about this image wasn't accounted for. Better a
            // full-size image than none at all.
            const std::scoped_lock lock(g_logMutex);
            if (g_log) {
                fprintf(g_log, "Vulkan refused reduced %ux%u mips=%u (VkResult=%d), retrying at full size\n",
                        reduced.extent.width, reduced.extent.height, reduced.mipLevels, static_cast<int>(result));
                fflush(g_log);
            }
            return state->realCreateImage(device, pCreateInfo, pAllocator, pImage);
        }

        LogReduction(*pCreateInfo, skip);

        if (pImage && *pImage != VK_NULL_HANDLE) {
            const std::scoped_lock lock(g_reducedMutex);
            g_reducedImages[*pImage] = skip;
        }

        return result;
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyImage(
        VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
        // Dropped before the destroy, not after: the driver is free to hand
        // the same handle value back to another thread's vkCreateImage as
        // soon as it returns, and erasing then would untrack that new image.
        if (image != VK_NULL_HANDLE) {
            const std::scoped_lock lock(g_reducedMutex);
            g_reducedImages.erase(image);
        }

        auto* state = StateFor(device);
        if (state && state->realDestroyImage) {
            state->realDestroyImage(device, image, pAllocator);
        } else {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkDestroyImage: no down-chain function for this device, image leaked");
        }
    }

    // vkCreateImageView

    VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateImageView(
        VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
        VkImageView* pView) {
        auto* state = StateFor(device);
        if (!state || !state->realCreateImageView) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCreateImageView: no down-chain function for this device, view creation failed");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto skip = pCreateInfo ? SkipFor(pCreateInfo->image) : 0;
        if (skip == 0 || !pCreateInfo) return state->realCreateImageView(device, pCreateInfo, pAllocator, pView);

        VkImageViewCreateInfo clamped = *pCreateInfo;
        ClampViewRange(clamped.subresourceRange, skip);

        const VkResult result = state->realCreateImageView(device, &clamped, pAllocator, pView);
        if (result != VK_SUCCESS) {
            // Same reporting as the D3D backends' "D3D refused a clamped
            // view": a refused view is where a wrong clamp would first show
            // up, and it must not be invisible.
            const std::scoped_lock lock(g_logMutex);
            if (g_log) {
                fprintf(g_log, "Vulkan refused a clamped view (base=%u count=%u, VkResult=%d)\n",
                        clamped.subresourceRange.baseMipLevel, clamped.subresourceRange.levelCount,
                        static_cast<int>(result));
                fflush(g_log);
            }
        }
        return result;
    }

    // vkCmdCopyBufferToImage(2) / vkCmdCopyImageToBuffer(2)
    // Only the image side carries a subresource, the buffer side is a flat
    // byte range with no mip concept.

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdCopyBufferToImage(
        VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout,
        uint32_t regionCount, const VkBufferImageCopy* pRegions) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realCopyBufferToImage) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdCopyBufferToImage: no down-chain function for this device, call dropped");
            return;
        }

        const auto skip = SkipFor(dstImage);
        if (skip == 0) {
            state->realCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
            return;
        }

        std::vector<VkBufferImageCopy> adjusted;
        adjusted.reserve(regionCount);
        for (uint32_t i = 0; i < regionCount; ++i) {
            VkBufferImageCopy region = pRegions[i];
            if (RemapSubresourceLayers(region.imageSubresource, skip)) adjusted.push_back(region);
        }

        if (!adjusted.empty())
            state->realCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout,
                                         static_cast<uint32_t>(adjusted.size()), adjusted.data());
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdCopyBufferToImage2(
        VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realCopyBufferToImage2) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdCopyBufferToImage2: no down-chain function for this device, call dropped");
            return;
        }
        if (!pCopyBufferToImageInfo) { state->realCopyBufferToImage2(commandBuffer, pCopyBufferToImageInfo); return; }

        const auto skip = SkipFor(pCopyBufferToImageInfo->dstImage);
        if (skip == 0) { state->realCopyBufferToImage2(commandBuffer, pCopyBufferToImageInfo); return; }

        std::vector<VkBufferImageCopy2> adjusted;
        adjusted.reserve(pCopyBufferToImageInfo->regionCount);
        for (uint32_t i = 0; i < pCopyBufferToImageInfo->regionCount; ++i) {
            VkBufferImageCopy2 region = pCopyBufferToImageInfo->pRegions[i];
            if (RemapSubresourceLayers(region.imageSubresource, skip)) adjusted.push_back(region);
        }
        if (adjusted.empty()) return;

        VkCopyBufferToImageInfo2 info = *pCopyBufferToImageInfo;
        info.regionCount              = static_cast<uint32_t>(adjusted.size());
        info.pRegions                 = adjusted.data();
        state->realCopyBufferToImage2(commandBuffer, &info);
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdCopyImageToBuffer(
        VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkBuffer dstBuffer,
        uint32_t regionCount, const VkBufferImageCopy* pRegions) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realCopyImageToBuffer) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdCopyImageToBuffer: no down-chain function for this device, call dropped");
            return;
        }

        const auto skip = SkipFor(srcImage);
        if (skip == 0) {
            state->realCopyImageToBuffer(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
            return;
        }

        std::vector<VkBufferImageCopy> adjusted;
        adjusted.reserve(regionCount);
        for (uint32_t i = 0; i < regionCount; ++i) {
            VkBufferImageCopy region = pRegions[i];
            if (RemapSubresourceLayers(region.imageSubresource, skip)) adjusted.push_back(region);
        }

        if (!adjusted.empty())
            state->realCopyImageToBuffer(commandBuffer, srcImage, srcImageLayout, dstBuffer,
                                         static_cast<uint32_t>(adjusted.size()), adjusted.data());
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdCopyImageToBuffer2(
        VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realCopyImageToBuffer2) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdCopyImageToBuffer2: no down-chain function for this device, call dropped");
            return;
        }
        if (!pCopyImageToBufferInfo) { state->realCopyImageToBuffer2(commandBuffer, pCopyImageToBufferInfo); return; }

        const auto skip = SkipFor(pCopyImageToBufferInfo->srcImage);
        if (skip == 0) { state->realCopyImageToBuffer2(commandBuffer, pCopyImageToBufferInfo); return; }

        std::vector<VkBufferImageCopy2> adjusted;
        adjusted.reserve(pCopyImageToBufferInfo->regionCount);
        for (uint32_t i = 0; i < pCopyImageToBufferInfo->regionCount; ++i) {
            VkBufferImageCopy2 region = pCopyImageToBufferInfo->pRegions[i];
            if (RemapSubresourceLayers(region.imageSubresource, skip)) adjusted.push_back(region);
        }
        if (adjusted.empty()) return;

        VkCopyImageToBufferInfo2 info = *pCopyImageToBufferInfo;
        info.regionCount              = static_cast<uint32_t>(adjusted.size());
        info.pRegions                 = adjusted.data();
        state->realCopyImageToBuffer2(commandBuffer, &info);
    }

    // vkCmdCopyImage(2)
    // Both sides carry a subresource; either side invalidated drops the
    // whole region, same as the D3D12 proxy's RemapCopyLocation.

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdCopyImage(
        VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage,
        VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy* pRegions) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realCopyImage) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdCopyImage: no down-chain function for this device, call dropped");
            return;
        }

        const auto srcSkip = SkipFor(srcImage);
        const auto dstSkip = SkipFor(dstImage);
        if (srcSkip == 0 && dstSkip == 0) {
            state->realCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount,
                                 pRegions);
            return;
        }

        std::vector<VkImageCopy> adjusted;
        adjusted.reserve(regionCount);
        for (uint32_t i = 0; i < regionCount; ++i) {
            VkImageCopy region = pRegions[i];
            if (RemapSubresourceLayers(region.srcSubresource, srcSkip) &&
                RemapSubresourceLayers(region.dstSubresource, dstSkip))
                adjusted.push_back(region);
        }

        if (!adjusted.empty())
            state->realCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout,
                                 static_cast<uint32_t>(adjusted.size()), adjusted.data());
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdCopyImage2(
        VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realCopyImage2) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdCopyImage2: no down-chain function for this device, call dropped");
            return;
        }
        if (!pCopyImageInfo) { state->realCopyImage2(commandBuffer, pCopyImageInfo); return; }

        const auto srcSkip = SkipFor(pCopyImageInfo->srcImage);
        const auto dstSkip = SkipFor(pCopyImageInfo->dstImage);
        if (srcSkip == 0 && dstSkip == 0) { state->realCopyImage2(commandBuffer, pCopyImageInfo); return; }

        std::vector<VkImageCopy2> adjusted;
        adjusted.reserve(pCopyImageInfo->regionCount);
        for (uint32_t i = 0; i < pCopyImageInfo->regionCount; ++i) {
            VkImageCopy2 region = pCopyImageInfo->pRegions[i];
            if (RemapSubresourceLayers(region.srcSubresource, srcSkip) &&
                RemapSubresourceLayers(region.dstSubresource, dstSkip))
                adjusted.push_back(region);
        }
        if (adjusted.empty()) return;

        VkCopyImageInfo2 info = *pCopyImageInfo;
        info.regionCount      = static_cast<uint32_t>(adjusted.size());
        info.pRegions         = adjusted.data();
        state->realCopyImage2(commandBuffer, &info);
    }

    // vkCmdBlitImage(2)
    // Offsets need no adjustment: level L's extent equals reduced level L-skip's,
    // so remapping the index alone lines them back up.

    // A self-blit mip chain seeds from the top level, which a reduction just
    // dropped. Can't be fixed here, only made diagnosable.
    void WarnOnSelfBlit(VkImage srcImage, VkImage dstImage) {
        if (srcImage != dstImage) return;
        static std::atomic<bool> reported{false};
        LogOnce(reported, "[hook] blit from an image to itself (runtime mip generation) on a reduced "
                          "image: expect corruption, raise MaxSize or set Enabled=0 for this game");
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdBlitImage(
        VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage,
        VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realBlitImage) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdBlitImage: no down-chain function for this device, call dropped");
            return;
        }

        const auto srcSkip = SkipFor(srcImage);
        const auto dstSkip = SkipFor(dstImage);
        if (srcSkip == 0 && dstSkip == 0) {
            state->realBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount,
                                 pRegions, filter);
            return;
        }
        WarnOnSelfBlit(srcImage, dstImage);

        std::vector<VkImageBlit> adjusted;
        adjusted.reserve(regionCount);
        for (uint32_t i = 0; i < regionCount; ++i) {
            VkImageBlit region = pRegions[i];
            if (RemapSubresourceLayers(region.srcSubresource, srcSkip) &&
                RemapSubresourceLayers(region.dstSubresource, dstSkip))
                adjusted.push_back(region);
        }

        if (!adjusted.empty())
            state->realBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout,
                                 static_cast<uint32_t>(adjusted.size()), adjusted.data(), filter);
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdBlitImage2(
        VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realBlitImage2) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdBlitImage2: no down-chain function for this device, call dropped");
            return;
        }
        if (!pBlitImageInfo) { state->realBlitImage2(commandBuffer, pBlitImageInfo); return; }

        const auto srcSkip = SkipFor(pBlitImageInfo->srcImage);
        const auto dstSkip = SkipFor(pBlitImageInfo->dstImage);
        if (srcSkip == 0 && dstSkip == 0) { state->realBlitImage2(commandBuffer, pBlitImageInfo); return; }
        WarnOnSelfBlit(pBlitImageInfo->srcImage, pBlitImageInfo->dstImage);

        std::vector<VkImageBlit2> adjusted;
        adjusted.reserve(pBlitImageInfo->regionCount);
        for (uint32_t i = 0; i < pBlitImageInfo->regionCount; ++i) {
            VkImageBlit2 region = pBlitImageInfo->pRegions[i];
            if (RemapSubresourceLayers(region.srcSubresource, srcSkip) &&
                RemapSubresourceLayers(region.dstSubresource, dstSkip))
                adjusted.push_back(region);
        }
        if (adjusted.empty()) return;

        VkBlitImageInfo2 info = *pBlitImageInfo;
        info.regionCount      = static_cast<uint32_t>(adjusted.size());
        info.pRegions         = adjusted.data();
        state->realBlitImage2(commandBuffer, &info);
    }

    // vkCmdPipelineBarrier / vkCmdPipelineBarrier2

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdPipelineBarrier(
        VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
        VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
        uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
        uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realPipelineBarrier) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdPipelineBarrier: no down-chain function for this device, call dropped");
            return;
        }

        bool anyRelevant = false;
        for (uint32_t i = 0; i < imageMemoryBarrierCount && !anyRelevant; ++i)
            if (SkipFor(pImageMemoryBarriers[i].image) > 0) anyRelevant = true;

        if (!anyRelevant) {
            state->realPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                                       memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount,
                                       pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
            return;
        }

        std::vector<VkImageMemoryBarrier> adjusted;
        adjusted.reserve(imageMemoryBarrierCount);
        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            VkImageMemoryBarrier barrier = pImageMemoryBarriers[i];
            const auto skip               = SkipFor(barrier.image);
            if (RemapBarrierRange(barrier.subresourceRange, skip)) adjusted.push_back(barrier);
        }

        state->realPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount,
                                   pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers,
                                   static_cast<uint32_t>(adjusted.size()),
                                   adjusted.empty() ? nullptr : adjusted.data());
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdPipelineBarrier2(
        VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo) {
        auto* state = StateFor(commandBuffer);
        if (!state || !state->realPipelineBarrier2) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkCmdPipelineBarrier2: no down-chain function for this device, call dropped");
            return;
        }
        if (!pDependencyInfo) { state->realPipelineBarrier2(commandBuffer, pDependencyInfo); return; }

        bool anyRelevant = false;
        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount && !anyRelevant; ++i)
            if (SkipFor(pDependencyInfo->pImageMemoryBarriers[i].image) > 0) anyRelevant = true;

        if (!anyRelevant) {
            state->realPipelineBarrier2(commandBuffer, pDependencyInfo);
            return;
        }

        std::vector<VkImageMemoryBarrier2> adjusted;
        adjusted.reserve(pDependencyInfo->imageMemoryBarrierCount);
        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i) {
            VkImageMemoryBarrier2 barrier = pDependencyInfo->pImageMemoryBarriers[i];
            const auto skip                = SkipFor(barrier.image);
            if (RemapBarrierRange(barrier.subresourceRange, skip)) adjusted.push_back(barrier);
        }

        VkDependencyInfo info        = *pDependencyInfo;
        info.imageMemoryBarrierCount = static_cast<uint32_t>(adjusted.size());
        info.pImageMemoryBarriers    = adjusted.empty() ? nullptr : adjusted.data();
        state->realPipelineBarrier2(commandBuffer, &info);
    }

    // vkDestroyDevice / vkDestroyInstance
    // Cleans up the per-device/per-instance state so a long play session
    // that recreates its device (e.g. on a display-mode change some engines
    // handle that way) doesn't accumulate stale entries.

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_vkGetDeviceProcAddr(VkDevice device, const char* pName);
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_vkGetInstanceProcAddr(VkInstance instance, const char* pName);

    VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
        PFN_vkDestroyDevice real = nullptr;
        {
            const std::scoped_lock lock(g_devicesMutex);
            const auto it = g_devices.find(DispatchKey(device));
            if (it != g_devices.end()) {
                real = it->second.realDestroyDevice;
                g_devices.erase(it);
            }
        }
        if (real) {
            real(device, pAllocator);
        } else {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkDestroyDevice: no down-chain function for this device, device leaked");
        }
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
        PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
        {
            const std::scoped_lock lock(g_instancesMutex);
            const auto it = g_instances.find(DispatchKey(instance));
            if (it != g_instances.end()) {
                getInstanceProcAddr = it->second.getInstanceProcAddr;
                g_instances.erase(it);
            }
        }
        if (getInstanceProcAddr) {
            auto real = reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr(instance, "vkDestroyInstance"));
            if (real) real(instance, pAllocator);
        }
    }

    // vkCreateDevice
    // Standard layer bootstrap: pull this layer's link off the pCreateInfo
    // chain, call down to the next layer/loader's real vkCreateDevice, then
    // resolve every intercepted function through the "next"
    // vkGetDeviceProcAddr from that link, never through a loaded
    // vulkan-1.dll directly.

    VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateDevice(
        VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
        Init();

        auto* linkInfo = pCreateInfo ? FindLayerInfo<VkLayerDeviceCreateInfo>(
                                            pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                            VK_LAYER_LINK_INFO)
                                      : nullptr;
        if (!linkInfo || !linkInfo->u.pLayerInfo) {
            LogLine("[hook] vkCreateDevice: no loader link info on pCreateInfo->pNext, not loaded as a "
                    "proper layer, refusing to continue");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const PFN_vkGetInstanceProcAddr getInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        const PFN_vkGetDeviceProcAddr getDeviceProcAddr     = linkInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        if (!getInstanceProcAddr || !getDeviceProcAddr) {
            LogLine("[hook] vkCreateDevice: loader link info is missing a next GetProcAddr pointer");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Advance the link for whichever layer comes after this one.
        linkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

        InstanceState instanceState;
        {
            const std::scoped_lock lock(g_instancesMutex);
            const auto it = g_instances.find(DispatchKey(physicalDevice));
            if (it == g_instances.end()) {
                LogLine("[hook] vkCreateDevice: physicalDevice doesn't belong to any instance this layer "
                        "saw vkCreateInstance for, no reduction will happen on this device");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            instanceState = it->second;
        }

        const auto createDevice =
            reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(instanceState.handle, "vkCreateDevice"));
        if (!createDevice) {
            LogLine("[hook] vkCreateDevice: next layer/loader has no real vkCreateDevice");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkResult result = createDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        if (result != VK_SUCCESS || !pDevice) return result;

        // Below Vulkan 1.3 the promoted commands only exist under their
        // original extension names, so each falls back to its KHR spelling.
        const auto resolve = [&] (const char* name, const char* alias = nullptr) {
            auto fn = getDeviceProcAddr(*pDevice, name);
            if (!fn && alias) fn = getDeviceProcAddr(*pDevice, alias);
            return fn;
        };

        DeviceState state;
        state.getDeviceProcAddr = getDeviceProcAddr;
        state.realDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(resolve("vkDestroyDevice"));
        state.realCreateImage   = reinterpret_cast<PFN_vkCreateImage>(resolve("vkCreateImage"));
        state.realDestroyImage  = reinterpret_cast<PFN_vkDestroyImage>(resolve("vkDestroyImage"));
        state.realCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(resolve("vkCreateImageView"));
        state.realCopyBufferToImage =
            reinterpret_cast<PFN_vkCmdCopyBufferToImage>(resolve("vkCmdCopyBufferToImage"));
        state.realCopyBufferToImage2 = reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(
            resolve("vkCmdCopyBufferToImage2", "vkCmdCopyBufferToImage2KHR"));
        state.realCopyImage  = reinterpret_cast<PFN_vkCmdCopyImage>(resolve("vkCmdCopyImage"));
        state.realCopyImage2 =
            reinterpret_cast<PFN_vkCmdCopyImage2>(resolve("vkCmdCopyImage2", "vkCmdCopyImage2KHR"));
        state.realCopyImageToBuffer =
            reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(resolve("vkCmdCopyImageToBuffer"));
        state.realCopyImageToBuffer2 = reinterpret_cast<PFN_vkCmdCopyImageToBuffer2>(
            resolve("vkCmdCopyImageToBuffer2", "vkCmdCopyImageToBuffer2KHR"));
        state.realBlitImage  = reinterpret_cast<PFN_vkCmdBlitImage>(resolve("vkCmdBlitImage"));
        state.realBlitImage2 =
            reinterpret_cast<PFN_vkCmdBlitImage2>(resolve("vkCmdBlitImage2", "vkCmdBlitImage2KHR"));
        state.realPipelineBarrier =
            reinterpret_cast<PFN_vkCmdPipelineBarrier>(resolve("vkCmdPipelineBarrier"));
        state.realPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
            resolve("vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR"));

        if (!state.realCreateImage) {
            LogLine("[hook] vkCreateDevice: next layer/loader has no real vkCreateImage, no reduction "
                    "will happen on this device");
        }

        {
            const std::pair<const char*, const void*> resolved[] = {
                {"vkCreateImage", state.realCreateImage},
                {"vkDestroyImage", state.realDestroyImage},
                {"vkCreateImageView", state.realCreateImageView},
                {"vkCmdCopyBufferToImage", state.realCopyBufferToImage},
                {"vkCmdCopyBufferToImage2", state.realCopyBufferToImage2},
                {"vkCmdCopyImageToBuffer", state.realCopyImageToBuffer},
                {"vkCmdCopyImageToBuffer2", state.realCopyImageToBuffer2},
                {"vkCmdCopyImage", state.realCopyImage},
                {"vkCmdCopyImage2", state.realCopyImage2},
                {"vkCmdBlitImage", state.realBlitImage},
                {"vkCmdBlitImage2", state.realBlitImage2},
                {"vkCmdPipelineBarrier", state.realPipelineBarrier},
                {"vkCmdPipelineBarrier2", state.realPipelineBarrier2},
            };

            std::string active, missing;
            for (const auto& [name, fn] : resolved) (fn ? active : missing).append(" ").append(name);
            LogLine(("[hook] active:" + active).c_str());
            if (!missing.empty()) LogLine(("[hook] unavailable:" + missing).c_str());
        }

        // Registered even if some pointers are null: vkGetDeviceProcAddr
        // below only hands out a hook whose down-chain pointer exists, so an
        // unregistered device would be strictly worse.
        const std::scoped_lock lock(g_devicesMutex);
        g_devices[DispatchKey(*pDevice)] = state;

        return result;
    }

    // vkCreateInstance

    VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateInstance(
        const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
        Init();

        auto* linkInfo = pCreateInfo ? FindLayerInfo<VkLayerInstanceCreateInfo>(
                                            pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                            VK_LAYER_LINK_INFO)
                                      : nullptr;
        if (!linkInfo || !linkInfo->u.pLayerInfo) {
            LogLine("[hook] vkCreateInstance: no loader link info on pCreateInfo->pNext, not loaded as a "
                    "proper layer, refusing to continue");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const PFN_vkGetInstanceProcAddr getInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        if (!getInstanceProcAddr) {
            LogLine("[hook] vkCreateInstance: loader link info is missing the next GetInstanceProcAddr pointer");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Advance the link for whichever layer comes after this one.
        linkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

        const auto createInstance =
            reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!createInstance) {
            LogLine("[hook] vkCreateInstance: next layer/loader has no real vkCreateInstance");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkResult result = createInstance(pCreateInfo, pAllocator, pInstance);
        if (result != VK_SUCCESS || !pInstance) return result;

        InstanceState state;
        state.handle              = *pInstance;
        state.getInstanceProcAddr = getInstanceProcAddr;
        state.getMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            getInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties2"));
        if (!state.getMemoryProperties2)
            state.getMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
                getInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties2KHR"));

        const std::scoped_lock lock(g_instancesMutex);
        g_instances[DispatchKey(*pInstance)] = state;

        return result;
    }

    // vkGetPhysicalDeviceMemoryProperties2 (VK_EXT_memory_budget)
    // Caps heapBudget for device-local heaps when the caller chained the budget
    // extension's struct. Vulkan equivalent of the D3D spoof above.

    // Only handed out by Hook_vkGetInstanceProcAddr once the real function resolves.
    VKAPI_ATTR void VKAPI_CALL Hook_vkGetPhysicalDeviceMemoryProperties2(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
        PFN_vkGetPhysicalDeviceMemoryProperties2 real = nullptr;
        {
            const std::scoped_lock lock(g_instancesMutex);
            const auto it = g_instances.find(DispatchKey(physicalDevice));
            if (it != g_instances.end()) real = it->second.getMemoryProperties2;
        }
        if (!pMemoryProperties) return;
        if (!real) {
            static std::atomic<bool> reported{false};
            LogOnce(reported, "[hook] vkGetPhysicalDeviceMemoryProperties2: no down-chain function, call dropped");
            return;
        }

        real(physicalDevice, pMemoryProperties);

        const auto fakeMB = g_fakeVramBudgetMB.load(std::memory_order_relaxed);
        if (fakeMB == 0) return;
        const auto fakeBytes = static_cast<VkDeviceSize>(fakeMB) * 1024 * 1024;

        // memoryHeapCount, not VK_MAX_MEMORY_HEAPS: only that many entries of
        // memoryHeaps carry meaningful flags.
        const uint32_t heapCount = pMemoryProperties->memoryProperties.memoryHeapCount;
        for (auto* next = static_cast<VkBaseOutStructure*>(pMemoryProperties->pNext); next != nullptr;
             next        = next->pNext) {
            if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
            auto* budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);
            for (uint32_t i = 0; i < heapCount && i < VK_MAX_MEMORY_HEAPS; ++i)
                if (pMemoryProperties->memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    budget->heapBudget[i] = (std::min)(budget->heapBudget[i], fakeBytes);
        }
    }

    // vkGetInstanceProcAddr / vkGetDeviceProcAddr dispatch
    // Names in the two tables below route to our own wrappers; everything
    // else forwards to the next layer/loader's answer unchanged. The two
    // GetProcAddr functions are themselves in the first table so a layer
    // further down the chain that fetches them through here also stays
    // part of the chain.

    // The chain/instance-level half: these must always resolve to our own
    // wrappers, whatever per-device state exists (or doesn't) at the time.
    PFN_vkVoidFunction LookupChainFunction(const char* name) {
        struct Entry { const char* name; PFN_vkVoidFunction fn; };
        static const Entry table[] = {
            {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetDeviceProcAddr)},
            {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetInstanceProcAddr)},
            {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateInstance)},
            {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyInstance)},
            {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateDevice)},
        };

        for (const auto& entry : table)
            if (std::strcmp(name, entry.name) == 0) return entry.fn;
        return nullptr;
    }

    // The device-level half, paired with the DeviceState member each hook
    // forwards through. Only handed out when that pointer was captured,
    // otherwise replacing the function would silently drop calls instead of
    // leaving them untouched. `alias` is the pre-1.3 KHR spelling of a
    // promoted command, which an older driver may ask for instead.
    struct DeviceHook {
        const char* name;
        const char* alias;
        PFN_vkVoidFunction fn;
        bool (*available)(const DeviceState&);
    };

    const DeviceHook kDeviceHooks[] = {
        {"vkDestroyDevice", nullptr, reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyDevice),
         [](const DeviceState& s) { return s.realDestroyDevice != nullptr; }},
        {"vkCreateImage", nullptr, reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateImage),
         [](const DeviceState& s) { return s.realCreateImage != nullptr; }},
        {"vkDestroyImage", nullptr, reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyImage),
         [](const DeviceState& s) { return s.realDestroyImage != nullptr; }},
        {"vkCreateImageView", nullptr, reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateImageView),
         [](const DeviceState& s) { return s.realCreateImageView != nullptr; }},
        {"vkCmdCopyBufferToImage", nullptr,
         reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdCopyBufferToImage),
         [](const DeviceState& s) { return s.realCopyBufferToImage != nullptr; }},
        {"vkCmdCopyBufferToImage2", "vkCmdCopyBufferToImage2KHR",
         reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdCopyBufferToImage2),
         [](const DeviceState& s) { return s.realCopyBufferToImage2 != nullptr; }},
        {"vkCmdCopyImageToBuffer", nullptr,
         reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdCopyImageToBuffer),
         [](const DeviceState& s) { return s.realCopyImageToBuffer != nullptr; }},
        {"vkCmdCopyImageToBuffer2", "vkCmdCopyImageToBuffer2KHR",
         reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdCopyImageToBuffer2),
         [](const DeviceState& s) { return s.realCopyImageToBuffer2 != nullptr; }},
        {"vkCmdCopyImage", nullptr, reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdCopyImage),
         [](const DeviceState& s) { return s.realCopyImage != nullptr; }},
        {"vkCmdCopyImage2", "vkCmdCopyImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdCopyImage2),
         [](const DeviceState& s) { return s.realCopyImage2 != nullptr; }},
        {"vkCmdBlitImage", nullptr, reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBlitImage),
         [](const DeviceState& s) { return s.realBlitImage != nullptr; }},
        {"vkCmdBlitImage2", "vkCmdBlitImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBlitImage2),
         [](const DeviceState& s) { return s.realBlitImage2 != nullptr; }},
        {"vkCmdPipelineBarrier", nullptr, reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdPipelineBarrier),
         [](const DeviceState& s) { return s.realPipelineBarrier != nullptr; }},
        {"vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR",
         reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdPipelineBarrier2),
         [](const DeviceState& s) { return s.realPipelineBarrier2 != nullptr; }},
    };

    bool NameMatches(const DeviceHook& hook, const char* name) {
        return std::strcmp(name, hook.name) == 0 || (hook.alias && std::strcmp(name, hook.alias) == 0);
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
        if (!pName) return nullptr;
        if (auto* chain = LookupChainFunction(pName)) return chain;

        // The down-chain pointer is copied out before the lock is dropped, so
        // the next layer's GetProcAddr is never called with g_devicesMutex
        // held, since it runs arbitrary code from another layer.
        PFN_vkGetDeviceProcAddr down = nullptr;
        {
            const std::shared_lock lock(g_devicesMutex);
            const auto it = g_devices.find(DispatchKey(device));
            if (it == g_devices.end()) return nullptr;

            // An entry we can't forward through falls through to the real
            // function below rather than being reported as absent: leaving the
            // call untouched is the whole point of the availability check.
            if (g_active.load(std::memory_order_relaxed))
                for (const auto& hook : kDeviceHooks)
                    if (NameMatches(hook, pName) && hook.available(it->second)) return hook.fn;

            down = it->second.getDeviceProcAddr;
        }
        return down ? down(device, pName) : nullptr;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
        if (!pName) return nullptr;
        if (auto* chain = LookupChainFunction(pName)) return chain;

        // With no instance, only the global commands above are answerable;
        // everything else must resolve to null per the spec.
        if (!instance) return nullptr;

        const bool active = g_active.load(std::memory_order_relaxed);

        // A device-level name can legitimately be asked for here before any
        // device exists, so there's no per-device state to check availability
        // against. Hand out the wrapper and let it resolve at call time.
        if (active)
            for (const auto& hook : kDeviceHooks)
                if (NameMatches(hook, pName)) return hook.fn;

        PFN_vkGetInstanceProcAddr down = nullptr;
        bool hasMemoryProperties2      = false;
        {
            const std::scoped_lock lock(g_instancesMutex);
            const auto it = g_instances.find(DispatchKey(instance));
            if (it != g_instances.end()) {
                down                 = it->second.getInstanceProcAddr;
                hasMemoryProperties2 = it->second.getMemoryProperties2 != nullptr;
            }
        }

        // Unlike kDeviceHooks above, availability can be checked here.
        if (active && hasMemoryProperties2 &&
            (std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
             std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0))
            return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetPhysicalDeviceMemoryProperties2);

        return down ? down(instance, pName) : nullptr;
    }
}  // namespace

// Exported via the static VulkanLayer.def listing these 5 names, no
// ordinal pinning needed, unlike the D3D proxies, since the loader always
// resolves a layer's entry points by name. __declspec(dllexport) isn't used:
// vulkan_core.h/vk_layer.h already declare these names at global scope, and
// redefining them with different linkage is a hard compile error (MSVC
// C2375). The .def marks them exported without touching their linkage.
extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    Init();

    if (pVersionStruct == nullptr || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;

    pVersionStruct->loaderLayerInterfaceVersion = 2;  // version 2 added this negotiate function
    pVersionStruct->pfnGetInstanceProcAddr      = Hook_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr        = Hook_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    return VK_SUCCESS;
}

// These four are exported too, matching every real Vulkan layer, since some
// layers further down the chain resolve the next one's entry points this
// way instead of through vkNegotiateLoaderLayerInterfaceVersion's returned
// pointers.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* pName) {
    Init();
    return Hook_vkGetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char* pName) {
    Init();
    return Hook_vkGetDeviceProcAddr(device, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    return Hook_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    return Hook_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

}  // extern "C"

// The one DllMain for this binary (see Common.h for why it isn't shared via
// that header).
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_self = module;
            DisableThreadLibraryCalls(module);
            break;
        case DLL_PROCESS_DETACH:
            CloseLog();
            break;
    }
    return TRUE;
}
