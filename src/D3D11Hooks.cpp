// UniversalTextureDownscaler: DX11 proxy DLL.
//
// Same math as the D3D12 proxy (Common.h), adapted to D3D11: no explicit
// barriers to remap, and CreateTexture2D can take the whole mip chain via
// pInitialData, and dropping the same number of entries off its front keeps
// each one lined up with the mip level it now describes. When mips stream
// in later instead, UpdateSubresource/CopySubresourceRegion get their
// subresource index remapped.
//
// Proxies d3d11.dll next to the game's exe, loading the real one from
// System32. Only the two commonly-used entry points are exported (pinned
// to their real ordinals, see generate_def.ps1); everything else is left
// unexported, which fails safely if anything looks for it.

#include "Common.h"

#include <d3d11.h>
#include <d3d11_1.h>

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

using namespace Common;

namespace {
    HMODULE g_real = nullptr;
    std::once_flag g_initOnce;

    template <typename T>
    T LoadReal(const char* name) {
        return g_real ? reinterpret_cast<T>(GetProcAddress(g_real, name)) : nullptr;
    }

    PFN_D3D11_CREATE_DEVICE g_realCreateDevice                     = nullptr;
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_realCreateDeviceAndSwapChain = nullptr;

    // Loaded lazily, never from DllMain, since loading a DLL while the loader
    // lock is held risks a deadlock.
    void Init() {
        std::call_once(g_initOnce, [] {
            const auto dir = SelfDirectory();
            OpenLog(L"UniversalTextureDownscaler_D3D11.log", dir);
            LoadConfig(dir);

            wchar_t system[MAX_PATH]{};
            GetSystemDirectoryW(system, MAX_PATH);
            const std::wstring realPath = std::wstring(system) + L"\\d3d11.dll";

            g_real = LoadLibraryW(realPath.c_str());
            if (!g_real) {
                LogLine("[hook] LoadLibraryW failed for the real d3d11.dll, every exported call will "
                        "return E_NOINTERFACE, the game will likely fail to start");
                return;
            }

            g_realCreateDevice = LoadReal<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
            g_realCreateDeviceAndSwapChain =
                LoadReal<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>("D3D11CreateDeviceAndSwapChain");
        });
    }

    // vtable slots, verified against the real d3d11.h/d3d11_1.h SDK headers
    // ID3D11Device = IUnknown(3) + own methods:
    constexpr std::size_t kSlot_CreateTexture2D          = 5;
    constexpr std::size_t kSlot_CreateShaderResourceView = 7;
    constexpr std::size_t kSlot_CreateDeferredContext    = 27;
    constexpr std::size_t kSlot_GetImmediateContext      = 40;
    // ID3D11DeviceContext = IUnknown(3) + ID3D11DeviceChild(4: GetDevice,
    // Get/SetPrivateData, SetPrivateDataInterface) + own methods (108 total,
    // VSSetConstantBuffers..UpdateSubresource..ExecuteCommandList..):
    constexpr std::size_t kSlot_CopySubresourceRegion = 46;
    constexpr std::size_t kSlot_UpdateSubresource     = 48;
    // ID3D11DeviceContext's own 108 methods end at slot 7+107=114; the *1
    // overloads are the first two methods ID3D11DeviceContext1 adds on top.
    constexpr std::size_t kSlot_CopySubresourceRegion1 = 115;
    constexpr std::size_t kSlot_UpdateSubresource1     = 116;

    // desc-shape filter: everything decidable from D3D11_TEXTURE2D_DESC
    // Mirrors the D3D12 proxy's filter: render-target/UAV, shared/generated-
    // mips, CPU-visible, and array/cubemap textures are all excluded. No
    // Enabled check here; the caller checks that separately.
    const char* RejectionReason(const D3D11_TEXTURE2D_DESC& desc) {
        if (desc.MipLevels <= 1) return "single-mip";
        if (desc.ArraySize != 1) return "array-or-cubemap";

        constexpr UINT kRejectedBind =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
        if (desc.BindFlags & kRejectedBind) return "render-target-or-uav";

        // Shared: an outside consumer describes the texture again and won't
        // see the reduction. Generate-mips: a writable surface, not authored
        // content. Tiled: a reserved/sparse resource with its own tile-based
        // population API (UpdateTileMappings/CopyTiles), structurally
        // different from a normal mip chain. Out of scope, same as D3D12
        // reserved resources.
        constexpr UINT kRejectedMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_TILED;
        if (desc.MiscFlags & kRejectedMisc) return "shared-generated-or-tiled";

        // A texture the CPU can still write to, or a dynamic/staging one,
        // gets rewritten at runtime by code that works out offsets from the
        // size it was created at.
        if (desc.CPUAccessFlags != 0) return "cpu-visible";
        if (desc.Usage != D3D11_USAGE_DEFAULT && desc.Usage != D3D11_USAGE_IMMUTABLE) return "dynamic-or-staging";

        return nullptr;
    }

    // tracking which resources were actually reduced, and by how much

    // id catches address reuse: a freed texture's memory can go straight back
    // to another thread's CreateTexture2D before Release drops the old entry.
    struct TrackedResource {
        std::uint32_t skip = 0;
        std::uint64_t id   = 0;
    };

    std::shared_mutex g_reducedMutex;
    std::unordered_map<void*, TrackedResource> g_reducedResources;  // ID3D11Texture2D* -> skip
    std::uint64_t g_nextTrackedId = 1;

    std::uint32_t SkipFor(void* resource) {
        if (!resource) return 0;
        const std::shared_lock lock(g_reducedMutex);
        const auto it = g_reducedResources.find(resource);
        return it != g_reducedResources.end() ? it->second.skip : 0;
    }

    void TrackReduced(void* resource, std::uint32_t skip) {
        const std::scoped_lock lock(g_reducedMutex);
        g_reducedResources[resource] = TrackedResource{skip, g_nextTrackedId++};
    }

    // logging

    void LogReduction(const D3D11_TEXTURE2D_DESC& original, std::uint32_t skip) {
        if (skip == 0) return;

        const auto originalBytes = EstimateBytes(original.Width, original.Height, original.MipLevels);
        const auto reducedBytes =
            EstimateBytes(original.Width >> skip, original.Height >> skip, original.MipLevels - skip);
        const auto savedBytes = originalBytes > reducedBytes ? originalBytes - reducedBytes : 0;
        RecordReduction(savedBytes);
        if (!g_verbose.load(std::memory_order_relaxed)) return;

        const std::scoped_lock lock(g_logMutex);
        if (!g_log) return;
        fprintf(g_log, "[texture2d] %ux%u skip=%u levels=%u format=%u ~%llu KB saved\n", original.Width,
                original.Height, skip, original.MipLevels, static_cast<unsigned>(original.Format),
                static_cast<unsigned long long>(savedBytes / 1024));
        fflush(g_log);
    }

    // IUnknown::Release hook, to drop resources from the map once freed

    using Release_t = ULONG(STDMETHODCALLTYPE*)(IUnknown*);

    std::mutex g_releaseHooksMutex;
    std::unordered_map<void*, Release_t> g_originalReleaseByVtable;  // vtable -> original Release

    ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* self) {
        Release_t original = nullptr;
        {
            const std::scoped_lock lock(g_releaseHooksMutex);
            const auto it = g_originalReleaseByVtable.find(VtableOf(self));
            if (it != g_originalReleaseByVtable.end()) original = it->second;
        }
        if (!original) {
            // Should be unreachable: EnsureResourceReleaseHooked holds the
            // same mutex across the patch and the map insert. Reported
            // rather than guessing a refcount, since that's how a
            // use-after-free starts.
            LogLine("[hook] Release hook has no original for this vtable, refcount left untouched");
            return 1;
        }

        std::uint64_t trackedId = 0;
        {
            const std::shared_lock lock(g_reducedMutex);
            const auto it = g_reducedResources.find(self);
            if (it != g_reducedResources.end()) trackedId = it->second.id;
        }

        const ULONG count = original(self);
        if (count != 0 || trackedId == 0) return count;

        const std::scoped_lock lock(g_reducedMutex);
        const auto it = g_reducedResources.find(self);
        if (it != g_reducedResources.end() && it->second.id == trackedId) g_reducedResources.erase(it);
        return count;
    }

    void EnsureResourceReleaseHooked(void* resource) {
        void* vtable = VtableOf(resource);

        const std::scoped_lock lock(g_releaseHooksMutex);
        if (g_originalReleaseByVtable.count(vtable)) return;

        void* original = nullptr;
        if (PatchSlot(resource, kSlot_Release, reinterpret_cast<void*>(&Hook_Release), &original))
            g_originalReleaseByVtable[vtable] = reinterpret_cast<Release_t>(original);
    }

    // ID3D11Device::CreateTexture2D

    using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

    CreateTexture2D_t g_originalCreateTexture2D = nullptr;

    HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(
        ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data,
        ID3D11Texture2D** out) {
        const char* structuralReason = desc ? RejectionReason(*desc) : "no-desc";
        if (structuralReason == nullptr) Heartbeat();

        const char* reason = g_enabled.load(std::memory_order_relaxed) ? structuralReason : "disabled";

        if (g_verbose.load(std::memory_order_relaxed) && desc && desc->MipLevels > 1) {
            const std::scoped_lock lock(g_logMutex);
            if (g_log) {
                fprintf(g_log, "[multi-mip] %ux%u mips=%u fmt=%u -> %s\n", desc->Width, desc->Height,
                        desc->MipLevels, static_cast<unsigned>(desc->Format), reason ? reason : "ACCEPTED");
                fflush(g_log);
            }
        }

        if (reason != nullptr) return g_originalCreateTexture2D(self, desc, data, out);

        const auto skip = ComputeSkip(desc->Width, desc->Height, desc->MipLevels, IsBlockCompressed(desc->Format),
                                       g_maxSize.load(std::memory_order_relaxed));
        if (skip == 0) {
            return g_originalCreateTexture2D(self, desc, data, out);
        }

        D3D11_TEXTURE2D_DESC reduced = *desc;
        reduced.Width     = desc->Width >> skip;
        reduced.Height    = desc->Height >> skip;
        reduced.MipLevels = desc->MipLevels - skip;

        // ArraySize==1 is guaranteed by RejectionReason, so the subresource
        // data array (if any) is exactly MipLevels entries in mip order -
        // dropping the first `skip` keeps every remaining entry lined up
        // with the mip level it now describes. Same technique the reference
        // SKSE plugin uses. When data is null, the app populates the
        // texture later via UpdateSubresource/CopySubresourceRegion, which
        // get remapped by the hooks below instead.
        const HRESULT hr =
            g_originalCreateTexture2D(self, &reduced, data ? data + skip : nullptr, out);

        if (FAILED(hr)) {
            // Something about this texture wasn't accounted for. Better a
            // full-size texture than none at all.
            const std::scoped_lock lock(g_logMutex);
            if (g_log) {
                fprintf(g_log, "D3D refused reduced %ux%u mips=%u (0x%08X), retrying at full size\n",
                        reduced.Width, reduced.Height, reduced.MipLevels, static_cast<unsigned>(hr));
                fflush(g_log);
            }
            return g_originalCreateTexture2D(self, desc, data, out);
        }

        LogReduction(*desc, skip);

        if (out && *out) {
            EnsureResourceReleaseHooked(*out);
            TrackReduced(*out, skip);
        }

        return hr;
    }

    // ID3D11Device::CreateShaderResourceView
    // Remaps MostDetailedMip/MipLevels the same way UpdateSubresource and
    // CopySubresourceRegion are remapped below: subtract skip, clamp to what
    // the reduced resource actually has.

    using CreateSRV_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);

    CreateSRV_t g_originalCreateSRV = nullptr;

    HRESULT STDMETHODCALLTYPE Hook_CreateShaderResourceView(
        ID3D11Device* self, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
        ID3D11ShaderResourceView** out) {
        const auto skip = SkipFor(resource);
        if (skip == 0 || !desc || desc->ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
            return g_originalCreateSRV(self, resource, desc, out);

        D3D11_SHADER_RESOURCE_VIEW_DESC clamped = *desc;
        auto& tex                                = clamped.Texture2D;

        const UINT originalMost = tex.MostDetailedMip;
        tex.MostDetailedMip     = originalMost >= skip ? originalMost - skip : 0;

        if (tex.MipLevels != static_cast<UINT>(-1)) {
            // 64-bit so MostDetailedMip+MipLevels can't wrap; the result is
            // bounded by the original level count and fits back in UINT.
            const std::uint64_t originalEnd = static_cast<std::uint64_t>(originalMost) + tex.MipLevels;
            const std::uint64_t newEnd      = originalEnd >= skip ? originalEnd - skip : 0;
            tex.MipLevels                   = newEnd > tex.MostDetailedMip
                                                  ? static_cast<UINT>(newEnd - tex.MostDetailedMip)
                                                  : 1;
        }

        const HRESULT hr = g_originalCreateSRV(self, resource, &clamped, out);
        if (FAILED(hr)) {
            const std::scoped_lock lock(g_logMutex);
            if (g_log) {
                fprintf(g_log, "D3D refused a clamped view (0x%08X)\n", static_cast<unsigned>(hr));
                fflush(g_log);
            }
        }
        return hr;
    }

    // ID3D11DeviceContext::UpdateSubresource / CopySubresourceRegion
    // ArraySize==1 is guaranteed by RejectionReason (same as D3D12's
    // DepthOrArraySize==1 precondition), so D3D11CalcSubresource's encoding
    // (MipSlice + ArraySlice*MipLevels) collapses to a pure mip index, no
    // multiplication needed, same simplification the D3D12 proxy relies on.

    using UpdateSubresource_t =
        void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
    using CopySubresourceRegion_t = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
    using UpdateSubresource1_t = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
    using CopySubresourceRegion1_t = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT);

    // Keyed by vtable, not one global each: the immediate and deferred
    // contexts are separate implementation classes and may not share a
    // vtable. A single global would get overwritten by whichever was
    // patched last, chaining calls on the other vtable into the wrong class.
    struct ContextOriginals {
        UpdateSubresource_t updateSubresource           = nullptr;
        CopySubresourceRegion_t copySubresourceRegion   = nullptr;
        UpdateSubresource1_t updateSubresource1         = nullptr;
        CopySubresourceRegion1_t copySubresourceRegion1 = nullptr;
    };

    std::mutex g_contextVtablesMutex;
    std::unordered_map<void*, ContextOriginals> g_contextOriginals;  // vtable -> originals

    // Returned by value: tiny, and it means no map reference outlives the lock.
    ContextOriginals OriginalsFor(void* context) {
        const std::scoped_lock lock(g_contextVtablesMutex);
        const auto it = g_contextOriginals.find(VtableOf(context));
        return it != g_contextOriginals.end() ? it->second : ContextOriginals{};
    }

    void STDMETHODCALLTYPE Hook_UpdateSubresource(
        ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSubresource, const D3D11_BOX* dstBox,
        const void* srcData, UINT srcRowPitch, UINT srcDepthPitch) {
        const auto original = OriginalsFor(self).updateSubresource;
        if (!original) {
            LogLine("[hook] UpdateSubresource: no original for this context vtable, upload dropped");
            return;
        }

        const auto skip = SkipFor(dst);
        if (skip > 0) {
            if (dstSubresource < skip) return;  // targets a mip that no longer exists
            dstSubresource -= skip;
        }
        original(self, dst, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
    }

    void STDMETHODCALLTYPE Hook_UpdateSubresource1(
        ID3D11DeviceContext1* self, ID3D11Resource* dst, UINT dstSubresource, const D3D11_BOX* dstBox,
        const void* srcData, UINT srcRowPitch, UINT srcDepthPitch, UINT copyFlags) {
        const auto original = OriginalsFor(self).updateSubresource1;
        if (!original) {
            LogLine("[hook] UpdateSubresource1: no original for this context vtable, upload dropped");
            return;
        }

        const auto skip = SkipFor(dst);
        if (skip > 0) {
            if (dstSubresource < skip) return;
            dstSubresource -= skip;
        }
        original(self, dst, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch, copyFlags);
    }

    // Returns false if the whole call should be dropped: either side named a
    // mip that no longer exists.
    bool RemapPair(ID3D11Resource* dstResource, UINT& dstSubresource, ID3D11Resource* srcResource,
                   UINT& srcSubresource) {
        const auto dstSkip = SkipFor(dstResource);
        if (dstSkip > 0) {
            if (dstSubresource < dstSkip) return false;
            dstSubresource -= dstSkip;
        }

        const auto srcSkip = SkipFor(srcResource);
        if (srcSkip > 0) {
            if (srcSubresource < srcSkip) return false;
            srcSubresource -= srcSkip;
        }

        return true;
    }

    void STDMETHODCALLTYPE Hook_CopySubresourceRegion(
        ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSubresource, UINT dstX, UINT dstY, UINT dstZ,
        ID3D11Resource* src, UINT srcSubresource, const D3D11_BOX* srcBox) {
        const auto original = OriginalsFor(self).copySubresourceRegion;
        if (!original) {
            LogLine("[hook] CopySubresourceRegion: no original for this context vtable, copy dropped");
            return;
        }
        if (!RemapPair(dst, dstSubresource, src, srcSubresource)) return;
        original(self, dst, dstSubresource, dstX, dstY, dstZ, src, srcSubresource, srcBox);
    }

    void STDMETHODCALLTYPE Hook_CopySubresourceRegion1(
        ID3D11DeviceContext1* self, ID3D11Resource* dst, UINT dstSubresource, UINT dstX, UINT dstY, UINT dstZ,
        ID3D11Resource* src, UINT srcSubresource, const D3D11_BOX* srcBox, UINT copyFlags) {
        const auto original = OriginalsFor(self).copySubresourceRegion1;
        if (!original) {
            LogLine("[hook] CopySubresourceRegion1: no original for this context vtable, copy dropped");
            return;
        }
        if (!RemapPair(dst, dstSubresource, src, srcSubresource)) return;
        original(self, dst, dstSubresource, dstX, dstY, dstZ, src, srcSubresource, srcBox, copyFlags);
    }

    // context-level install
    // Called for the immediate context and every deferred context a
    // multi-threaded renderer creates. Patches again for any vtable not
    // seen yet.

    void EnsureContextHooked(void* context) {
        if (!context) return;
        void* vtable = VtableOf(context);

        // Held across both the patching and the map insert, so a hook that is
        // already live in the vtable can never observe a missing original:
        // the hooks take this same mutex to look one up.
        const std::scoped_lock lock(g_contextVtablesMutex);
        if (g_contextOriginals.count(vtable)) return;

        ContextOriginals originals;
        void* original = nullptr;

        if (PatchSlot(context, kSlot_UpdateSubresource, reinterpret_cast<void*>(&Hook_UpdateSubresource),
                       &original))
            originals.updateSubresource = reinterpret_cast<UpdateSubresource_t>(original);
        else
            LogLine("[hook] UpdateSubresource FAILED to patch on a context vtable");

        if (PatchSlot(context, kSlot_CopySubresourceRegion,
                       reinterpret_cast<void*>(&Hook_CopySubresourceRegion), &original))
            originals.copySubresourceRegion = reinterpret_cast<CopySubresourceRegion_t>(original);
        else
            LogLine("[hook] CopySubresourceRegion FAILED to patch on a context vtable");

        // The *1 overloads only exist on ID3D11DeviceContext1+; probe via
        // QueryInterface instead of assuming the slots exist: patching past
        // the real vtable's end would corrupt adjacent memory.
        ID3D11DeviceContext1* context1 = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(context)->QueryInterface(IID_PPV_ARGS(&context1))) && context1) {
            if (PatchSlot(context1, kSlot_UpdateSubresource1,
                           reinterpret_cast<void*>(&Hook_UpdateSubresource1), &original))
                originals.updateSubresource1 = reinterpret_cast<UpdateSubresource1_t>(original);
            else
                LogLine("[hook] UpdateSubresource1 FAILED to patch on a context vtable");

            if (PatchSlot(context1, kSlot_CopySubresourceRegion1,
                           reinterpret_cast<void*>(&Hook_CopySubresourceRegion1), &original))
                originals.copySubresourceRegion1 = reinterpret_cast<CopySubresourceRegion1_t>(original);
            else
                LogLine("[hook] CopySubresourceRegion1 FAILED to patch on a context vtable");

            context1->Release();
        }

        g_contextOriginals[vtable] = originals;
    }

    // ID3D11Device::CreateDeferredContext / GetImmediateContext

    using CreateDeferredContext_t =
        HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
    using GetImmediateContext_t = void(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11DeviceContext**);

    CreateDeferredContext_t g_originalCreateDeferredContext = nullptr;
    GetImmediateContext_t g_originalGetImmediateContext     = nullptr;

    HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(
        ID3D11Device* self, UINT contextFlags, ID3D11DeviceContext** out) {
        const HRESULT hr = g_originalCreateDeferredContext(self, contextFlags, out);
        if (SUCCEEDED(hr) && out && *out) EnsureContextHooked(*out);
        return hr;
    }

    void STDMETHODCALLTYPE Hook_GetImmediateContext(ID3D11Device* self, ID3D11DeviceContext** out) {
        g_originalGetImmediateContext(self, out);
        if (out && *out) EnsureContextHooked(*out);
    }

    // device-level install

    std::atomic<bool> g_deviceHooked{false};

    // Every ID3D11Device instance from the same runtime/adapter shares one
    // vtable, so patching the first is enough, same reasoning as the D3D12
    // proxy's device singleton assumption.
    void HookDevice(ID3D11Device* device) {
        if (!device || g_deviceHooked.exchange(true)) return;

        void* original = nullptr;

        if (!PatchSlot(device, kSlot_CreateTexture2D, reinterpret_cast<void*>(&Hook_CreateTexture2D),
                        &original)) {
            LogLine("[hook] CreateTexture2D FAILED to patch, no reduction will happen");
            g_deviceHooked.store(false);
            return;
        }
        g_originalCreateTexture2D = reinterpret_cast<CreateTexture2D_t>(original);
        LogLine("[hook] CreateTexture2D patched");

        if (PatchSlot(device, kSlot_CreateShaderResourceView,
                       reinterpret_cast<void*>(&Hook_CreateShaderResourceView), &original))
            g_originalCreateSRV = reinterpret_cast<CreateSRV_t>(original);
        else
            LogLine("[hook] CreateShaderResourceView FAILED to patch, reduced textures won't get their views clamped");

        if (PatchSlot(device, kSlot_CreateDeferredContext,
                       reinterpret_cast<void*>(&Hook_CreateDeferredContext), &original))
            g_originalCreateDeferredContext = reinterpret_cast<CreateDeferredContext_t>(original);
        else
            LogLine("[hook] CreateDeferredContext FAILED to patch, deferred-context uploads/copies won't be remapped");

        if (PatchSlot(device, kSlot_GetImmediateContext,
                       reinterpret_cast<void*>(&Hook_GetImmediateContext), &original))
            g_originalGetImmediateContext = reinterpret_cast<GetImmediateContext_t>(original);
        else
            LogLine("[hook] GetImmediateContext FAILED to patch");
    }

    // Both exported entry points allow ppDevice == nullptr with only a
    // context requested, and the context can always name its own device.
    void HookDeviceAndContext(ID3D11Device** device, ID3D11DeviceContext** immediateContext) {
        if (device && *device) {
            HookDevice(*device);
        } else if (immediateContext && *immediateContext) {
            ID3D11Device* owner = nullptr;
            (*immediateContext)->GetDevice(&owner);
            if (owner) {
                HookDevice(owner);
                owner->Release();
            } else {
                LogLine("[hook] CreateDevice returned a context with no device, nothing to patch");
            }
        }

        if (immediateContext && *immediateContext) EnsureContextHooked(*immediateContext);
    }
}

extern "C" {

HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
    Init();
    if (!g_realCreateDevice) {
        LogLine("[hook] D3D11CreateDevice: no real function pointer (see the earlier LoadLibraryW line)");
        return E_NOINTERFACE;
    }

    const HRESULT hr = g_realCreateDevice(adapter, driverType, software, flags, featureLevels,
                                          numFeatureLevels, sdkVersion, device, featureLevel, immediateContext);

    if (SUCCEEDED(hr)) {
        HookDeviceAndContext(device, immediateContext);
        InstallVramSpoof(adapter);
    }

    return hr;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
    Init();
    if (!g_realCreateDeviceAndSwapChain) {
        LogLine("[hook] D3D11CreateDeviceAndSwapChain: no real function pointer (see the earlier "
                "LoadLibraryW line)");
        return E_NOINTERFACE;
    }

    const HRESULT hr = g_realCreateDeviceAndSwapChain(
        adapter, driverType, software, flags, featureLevels, numFeatureLevels, sdkVersion, swapChainDesc,
        swapChain, device, featureLevel, immediateContext);

    if (SUCCEEDED(hr)) {
        HookDeviceAndContext(device, immediateContext);
        InstallVramSpoof(adapter);
    }

    return hr;
}

}  // extern "C"

// DllMain for this target lives in D3D12Hooks.cpp, linked into the same
// binary (see Common.h for why it isn't shared via that header instead).
