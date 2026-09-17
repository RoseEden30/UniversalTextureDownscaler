// UniversalTextureDownscaler: DX12 proxy DLL.
//
// Hooks ID3D12Device's resource-creation methods directly at the vtable
// level, plus ResourceBarrier/CopyTextureRegion on the command list, so a
// reduced resource's subresource indices stay consistent everywhere the
// game references them, including in an app's own barriers, which no
// higher-level interception point sees before they've already executed.
//
// Proxies d3d12.dll next to the game's exe, loading the real one from
// System32. Only the commonly-used exports are implemented (pinned to
// their real ordinals, since some importers bind by ordinal, not name, see
// generate_def.ps1); everything else is left unexported, which fails
// safely if anything looks for it.

#include "Common.h"

#include <d3d12.h>

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Common;

namespace {
    HMODULE g_real = nullptr;
    std::once_flag g_initOnce;

    template <typename T>
    T LoadReal(const char* name) {
        return g_real ? reinterpret_cast<T>(GetProcAddress(g_real, name)) : nullptr;
    }

    PFN_D3D12_CREATE_DEVICE g_realCreateDevice = nullptr;
    PFN_D3D12_GET_DEBUG_INTERFACE g_realGetDebugInterface = nullptr;
    // How an Agility SDK app selects its redistributable runtime version
    // (D3D12SDKConfiguration), so a game using it still starts behind this proxy.
    PFN_D3D12_GET_INTERFACE g_realGetInterface = nullptr;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE g_realSerializeRootSignature = nullptr;
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE g_realSerializeVersionedRootSignature = nullptr;
    PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER g_realCreateRootSignatureDeserializer = nullptr;
    PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER g_realCreateVersionedRootSignatureDeserializer = nullptr;

    // No PFN_ typedef ships for this one in the SDK.
    using PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
    PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL g_realEnableExperimentalFeatures = nullptr;

    // Called lazily from the first exported call, never from DllMain, since
    // loading another DLL while the loader lock is held risks a deadlock.
    void Init() {
        std::call_once(g_initOnce, [] {
            const auto dir = SelfDirectory();
            OpenLog(L"UniversalTextureDownscaler_D3D12.log", dir);
            LoadConfig(dir);

            wchar_t system[MAX_PATH]{};
            GetSystemDirectoryW(system, MAX_PATH);
            const std::wstring realPath = std::wstring(system) + L"\\d3d12.dll";

            g_real = LoadLibraryW(realPath.c_str());
            if (!g_real) {
                LogLine("[hook] LoadLibraryW failed for the real d3d12.dll, every exported call will "
                        "return E_NOINTERFACE, the game will likely fail to start");
                return;
            }

            g_realCreateDevice = LoadReal<PFN_D3D12_CREATE_DEVICE>("D3D12CreateDevice");
            g_realGetDebugInterface = LoadReal<PFN_D3D12_GET_DEBUG_INTERFACE>("D3D12GetDebugInterface");
            g_realGetInterface = LoadReal<PFN_D3D12_GET_INTERFACE>("D3D12GetInterface");
            g_realSerializeRootSignature =
                LoadReal<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>("D3D12SerializeRootSignature");
            g_realSerializeVersionedRootSignature =
                LoadReal<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>("D3D12SerializeVersionedRootSignature");
            g_realCreateRootSignatureDeserializer =
                LoadReal<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER>("D3D12CreateRootSignatureDeserializer");
            g_realCreateVersionedRootSignatureDeserializer =
                LoadReal<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER>(
                    "D3D12CreateVersionedRootSignatureDeserializer");
            g_realEnableExperimentalFeatures =
                LoadReal<PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL>("D3D12EnableExperimentalFeatures");
        });
    }

    // vtable slots, verified against the real d3d12.h SDK header
    // ID3D12Device   = IUnknown(3) + ID3D12Object(4) + own methods:
    constexpr std::size_t kSlot_CreateCommandList          = 12;
    constexpr std::size_t kSlot_CreateShaderResourceView   = 18;
    // 25 (GetResourceAllocationInfo) is deliberately left unhooked, see the
    // note further down for why.
    constexpr std::size_t kSlot_CreateCommittedResource    = 27;
    constexpr std::size_t kSlot_CreatePlacedResource       = 29;
    // ID3D12Device's own 37 methods (GetNodeCount..GetAdapterLuid) end at
    // slot 7+36=43; each later interface only appends, so CreateCommandList1's
    // slot is that base plus every method added by 1 through 3:
    // Device1 +3 (CreatePipelineLibrary..SetResidencyPriority) -> 46,
    // Device2 +1 (CreatePipelineState) -> 47,
    // Device3 +3 (OpenExistingHeapFromAddress..EnqueueMakeResident) -> 50,
    // then ID3D12Device4::CreateCommandList1 -> 51.
    constexpr std::size_t kSlot_CreateCommandList1 = 51;
    // Same counting, further along: Device4 also adds CreateProtectedResourceSession
    // (52) then CreateCommittedResource1 (53).
    constexpr std::size_t kSlot_CreateCommittedResource1 = 53;
    // Device8's own additions; 68 is GetResourceAllocationInfo2.
    constexpr std::size_t kSlot_CreateCommittedResource2 = 69;
    constexpr std::size_t kSlot_CreatePlacedResource1    = 70;
    // Device10's, after Device9's three (ShaderCache/CommandQueue1).
    constexpr std::size_t kSlot_CreateCommittedResource3 = 76;
    constexpr std::size_t kSlot_CreatePlacedResource2    = 77;
    // ID3D12GraphicsCommandList = IUnknown(3) + ID3D12Object(4) +
    // ID3D12DeviceChild(1, GetDevice) + ID3D12CommandList(1, GetType) + own:
    constexpr std::size_t kSlot_CopyTextureRegion = 16;
    constexpr std::size_t kSlot_ResourceBarrier   = 26;
    // ID3D12GraphicsCommandList's own 51 methods (Close..ExecuteIndirect) end
    // at slot 9+50=59; each later interface only ever appends methods, so
    // Barrier's slot is that base plus every method added by 1 through 6:
    // CommandList1 +6 (AtomicCopyBufferUINT..SetViewInstanceMask) -> 65,
    // CommandList2 +1 (WriteBufferImmediate) -> 66,
    // CommandList3 +1 (SetProtectedResourceSession) -> 67,
    // CommandList4 +9 (BeginRenderPass..DispatchRays) -> 76,
    // CommandList5 +2 (RSSetShadingRate, RSSetShadingRateImage) -> 78,
    // CommandList6 +1 (DispatchMesh) -> 79, then CommandList7::Barrier -> 80.
    constexpr std::size_t kSlot_Barrier7 = 80;

    // desc-shape filter: everything decidable from D3D12_RESOURCE_DESC
    // Streamed textures typically arrive through CreatePlacedResource, not
    // CreateCommittedResource, and both are hooked below.
    // True for a heap the CPU cannot touch: plain DEFAULT, or CUSTOM set up
    // the same way. Some engines (UE5's RHI among them) spell "GPU-only" as
    // CUSTOM + NOT_AVAILABLE instead of the DEFAULT shorthand, so this checks
    // the CPU page property rather than matching only DEFAULT.
    bool IsGpuOnlyHeap(const D3D12_HEAP_PROPERTIES& heapProps) {
        if (heapProps.Type == D3D12_HEAP_TYPE_DEFAULT) return true;
        if (heapProps.Type == D3D12_HEAP_TYPE_CUSTOM)
            return heapProps.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        return false;  // UPLOAD, READBACK, GPU_UPLOAD are all CPU-visible by definition
    }

    // Desc-only half of the filter: everything decidable without knowing
    // which heap the resource will end up in. Used on its own by
    // CreatePlacedResource's caller-visible desc (heap type checked
    // separately below) and reused as the first stage by the full check, so
    // the two always agree on the same resource.
    //
    // Templated over D3D12_RESOURCE_DESC and D3D12_RESOURCE_DESC1: every field
    // read here is spelled the same in both, and DESC1 only adds
    // SamplerFeedbackMipRegion on the end.
    template <typename Desc>
    const char* DescOnlyRejectionReason(const Desc& desc) {
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return "not-texture2d";
        if (desc.MipLevels <= 1) return "single-mip";
        if (desc.DepthOrArraySize != 1) return "array-or-cubemap";

        constexpr D3D12_RESOURCE_FLAGS kRejectedFlags =
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (desc.Flags & kRejectedFlags) return "render-target-or-uav";
        if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) return "deny-shader-resource";
        // A second adapter's device describes this same resource itself and
        // would still describe it at the original size, the D3D12 analogue
        // of the shared-heap and D3D11 shared-resource rejections.
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) return "cross-adapter";

        // UNKNOWN is the normal, driver-opaque texture layout. The other
        // values (ROW_MAJOR, 64KB_STANDARD_SWIZZLE) are the ones whose byte
        // layout the API defines and the app is therefore free to compute
        // offsets into from the dimensions it asked for, same reasoning as
        // rejecting CPU-visible heaps.
        if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN) return "explicit-layout";

        // Sampler feedback: the region is expressed in the paired texture's
        // mips, so shrinking only this one desyncs the pair.
        if constexpr (requires { desc.SamplerFeedbackMipRegion; })
            if (desc.SamplerFeedbackMipRegion.Width != 0 || desc.SamplerFeedbackMipRegion.Height != 0)
                return "sampler-feedback";

        return nullptr;
    }

    // No Enabled check here; the caller checks that separately.
    template <typename Desc>
    const char* RejectionReason(const Desc& desc, const D3D12_HEAP_PROPERTIES& heapProps,
                                D3D12_HEAP_FLAGS heapFlags) {
        if (const char* reason = DescOnlyRejectionReason(desc)) return reason;

        // Only a plain GPU-resident asset: CPU-writable heaps get rewritten
        // at runtime by code that works out offsets from the created size.
        if (!IsGpuOnlyHeap(heapProps)) return "cpu-visible-heap";
        if (heapFlags & D3D12_HEAP_FLAG_SHARED) return "shared-heap";

        return nullptr;
    }

    // tracking which resources were actually reduced, and by how much

    // id catches address reuse: a freed resource's memory can go straight back
    // to another thread's create call before Release drops the old entry.
    struct TrackedResource {
        std::uint32_t skip = 0;
        std::uint64_t id   = 0;
    };

    std::shared_mutex g_reducedMutex;
    std::unordered_map<void*, TrackedResource> g_reducedResources;  // ID3D12Resource* -> skip
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

    // `origin` distinguishes CreateCommittedResource (own dedicated
    // allocation) from CreatePlacedResource (`heap` non-null: shares that
    // heap with whatever else is placed in it), which lets a shared-pool pattern
    // (many small textures reusing the same heap pointer, consistent with a
    // virtual-texturing physical tile cache) be told apart from many
    // independent small assets, from the log alone.
    template <typename Desc>
    void LogReduction(const Desc& original, std::uint32_t skip, const char* origin,
                      ID3D12Heap* heap) {
        if (skip == 0) return;

        // Pure arithmetic estimate (~8bpp, ignoring alignment/padding) -
        // close enough for a log figure, not used for any real decision.
        // GetResourceAllocationInfo is deliberately never called here, see
        // the comment near CreatePlacedResource below.
        const auto originalBytes = EstimateBytes(original.Width, original.Height, original.MipLevels);
        const auto reducedBytes =
            EstimateBytes(original.Width >> skip, original.Height >> skip, original.MipLevels - skip);
        const auto savedBytes = originalBytes > reducedBytes ? originalBytes - reducedBytes : 0;
        RecordReduction(savedBytes);
        if (!g_verbose.load(std::memory_order_relaxed)) return;

        const std::scoped_lock lock(g_logMutex);
        if (!g_log) return;
        fprintf(g_log, "[%s] %llux%u skip=%u levels=%u format=%u heap=%p ~%llu KB saved\n", origin,
                static_cast<unsigned long long>(original.Width), original.Height, skip, original.MipLevels,
                static_cast<unsigned>(original.Format), static_cast<void*>(heap),
                static_cast<unsigned long long>(savedBytes / 1024));
        fflush(g_log);
    }

    // IUnknown::Release hook, to drop resources from the map once freed
    // Patched once per distinct vtable (every ID3D12Resource from the same
    // runtime shares one implementation class, but this doesn't assume
    // that, it patches again for any vtable it hasn't seen yet).

    using Release_t = ULONG(STDMETHODCALLTYPE*)(IUnknown*);

    // shared_mutex, not mutex: written once per distinct vtable, read on every
    // Release of every resource sharing it.
    std::shared_mutex g_releaseHooksMutex;
    std::unordered_map<void*, Release_t> g_originalReleaseByVtable;  // vtable -> original Release

    ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* self) {
        Release_t original = nullptr;
        {
            const std::shared_lock lock(g_releaseHooksMutex);
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

        Release_t original = nullptr;
        if (PatchSlot(resource, kSlot_Release, reinterpret_cast<void*>(&Hook_Release), &original))
            g_originalReleaseByVtable[vtable] = original;
    }

    // ID3D12Device::CreateCommittedResource / CreatePlacedResource

    using CreateCommittedResource_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
        const D3D12_CLEAR_VALUE*, REFIID, void**);

    using CreatePlacedResource_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);

    // The later overloads an engine may use instead. 1 takes the same desc plus
    // a protected session; 2/3 and Placed1/2 take D3D12_RESOURCE_DESC1, which
    // only appends SamplerFeedbackMipRegion. Everything they add beyond the
    // desc is independent of the mip count and is forwarded untouched.
    using CreateCommittedResource1_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
    using CreateCommittedResource2_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
    using CreateCommittedResource3_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*,
        D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32,
        const DXGI_FORMAT*, REFIID, void**);
    using CreatePlacedResource1_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
        const D3D12_CLEAR_VALUE*, REFIID, void**);
    using CreatePlacedResource2_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT,
        const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);

    CreateCommittedResource_t g_originalCreateCommittedResource   = nullptr;
    CreatePlacedResource_t g_originalCreatePlacedResource         = nullptr;
    CreateCommittedResource1_t g_originalCreateCommittedResource1 = nullptr;
    CreateCommittedResource2_t g_originalCreateCommittedResource2 = nullptr;
    CreateCommittedResource3_t g_originalCreateCommittedResource3 = nullptr;
    CreatePlacedResource1_t g_originalCreatePlacedResource1       = nullptr;
    CreatePlacedResource2_t g_originalCreatePlacedResource2       = nullptr;

    // Shared body for every create entry point: `call(desc)` runs the
    // down-chain call with whichever desc it is handed.
    template <typename Desc, typename Call>
    HRESULT CreateReduced(const Desc* desc, const char* origin, ID3D12Heap* heap, void** out, Call&& call) {
        const auto skip = ComputeSkip(desc->Width, desc->Height, desc->MipLevels,
                                      IsBlockCompressed(desc->Format),
                                      g_maxSize.load(std::memory_order_relaxed));
        if (skip == 0) return call(desc);

        Desc reduced      = *desc;
        reduced.Width     = desc->Width >> skip;
        reduced.Height    = static_cast<UINT>(desc->Height >> skip);
        reduced.MipLevels = static_cast<UINT16>(desc->MipLevels - skip);
        // Alignment depends on size (a small texture qualifies for 4KB tiles
        // instead of the default 64KB), so 0 lets the runtime pick.
        reduced.Alignment = 0;

        const HRESULT hr = call(&reduced);
        if (FAILED(hr)) {
            // Better a full-size texture than none at all.
            const std::scoped_lock lock(g_logMutex);
            if (g_log) {
                fprintf(g_log, "D3D refused reduced %s %llux%u mips=%u (0x%08X), retrying at full size\n",
                        origin, static_cast<unsigned long long>(reduced.Width), reduced.Height,
                        reduced.MipLevels, static_cast<unsigned>(hr));
                fflush(g_log);
            }
            return call(desc);
        }

        LogReduction(*desc, skip, origin, heap);

        if (out && *out) {
            EnsureResourceReleaseHooked(*out);
            TrackReduced(*out, skip);
        }

        return hr;
    }

    template <typename Desc>
    void LogCandidate(const char* origin, const Desc* desc, const char* reason) {
        if (!g_verbose.load(std::memory_order_relaxed) || !desc ||
            desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc->MipLevels <= 1)
            return;

        const std::scoped_lock lock(g_logMutex);
        if (!g_log) return;
        fprintf(g_log, "[multi-mip:%s] %llux%u mips=%u fmt=%u -> %s\n", origin,
                static_cast<unsigned long long>(desc->Width), desc->Height, desc->MipLevels,
                static_cast<unsigned>(desc->Format), reason ? reason : "ACCEPTED");
        fflush(g_log);
    }

    // Committed side: the heap is described by the parameters themselves.
    template <typename Desc, typename Call>
    HRESULT CreateCommitted(const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
                            const Desc* desc, const char* origin, void** out, Call&& call) {
        const char* structural = (desc && heapProps) ? RejectionReason(*desc, *heapProps, heapFlags) : "no-desc";
        if (structural == nullptr) Heartbeat();

        const char* reason = g_enabled.load(std::memory_order_relaxed) ? structural : "disabled";
        LogCandidate(origin, desc, reason);

        if (reason != nullptr) return call(desc);
        return CreateReduced(desc, origin, nullptr, out, std::forward<Call>(call));
    }

    // Placed side: the heap's real type has to be queried off the heap itself.
    template <typename Desc, typename Call>
    HRESULT CreatePlaced(ID3D12Heap* heap, const Desc* desc, const char* origin, void** out, Call&& call) {
        if (!desc || !heap || DescOnlyRejectionReason(*desc) != nullptr) {
            LogCandidate(origin, desc, "rejected-on-desc");
            return call(desc);
        }

        const D3D12_HEAP_DESC heapDesc = heap->GetDesc();
        if (!IsGpuOnlyHeap(heapDesc.Properties) || (heapDesc.Flags & D3D12_HEAP_FLAG_SHARED)) {
            LogCandidate(origin, desc, "cpu-visible-or-shared-heap");
            return call(desc);
        }

        Heartbeat();
        if (!g_enabled.load(std::memory_order_relaxed)) {
            LogCandidate(origin, desc, "disabled");
            return call(desc);
        }

        LogCandidate(origin, desc, nullptr);
        return CreateReduced(desc, origin, heap, out, std::forward<Call>(call));
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(
        ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
        return CreateCommitted(heapProps, heapFlags, desc, "committed", out,
            [&] (const D3D12_RESOURCE_DESC* d) {
                return g_originalCreateCommittedResource(self, heapProps, heapFlags, d, state, clear, riid, out);
            });
    }

    // Streamed textures typically go through here, not CreateCommittedResource.
    HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(
        ID3D12Device* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
        return CreatePlaced(heap, desc, "placed", out,
            [&] (const D3D12_RESOURCE_DESC* d) {
                return g_originalCreatePlacedResource(self, heap, offset, d, state, clear, riid, out);
            });
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource1(
        ID3D12Device4* self, const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
        ID3D12ProtectedResourceSession* session, REFIID riid, void** out) {
        return CreateCommitted(heapProps, heapFlags, desc, "committed1", out,
            [&] (const D3D12_RESOURCE_DESC* d) {
                return g_originalCreateCommittedResource1(self, heapProps, heapFlags, d, state, clear,
                                                          session, riid, out);
            });
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource2(
        ID3D12Device8* self, const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
        const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
        ID3D12ProtectedResourceSession* session, REFIID riid, void** out) {
        return CreateCommitted(heapProps, heapFlags, desc, "committed2", out,
            [&] (const D3D12_RESOURCE_DESC1* d) {
                return g_originalCreateCommittedResource2(self, heapProps, heapFlags, d, state, clear,
                                                          session, riid, out);
            });
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource3(
        ID3D12Device10* self, const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
        const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear,
        ID3D12ProtectedResourceSession* session, UINT32 numCastableFormats,
        const DXGI_FORMAT* castableFormats, REFIID riid, void** out) {
        return CreateCommitted(heapProps, heapFlags, desc, "committed3", out,
            [&] (const D3D12_RESOURCE_DESC1* d) {
                return g_originalCreateCommittedResource3(self, heapProps, heapFlags, d, layout, clear,
                                                          session, numCastableFormats, castableFormats,
                                                          riid, out);
            });
    }

    HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource1(
        ID3D12Device8* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
        return CreatePlaced(heap, desc, "placed1", out,
            [&] (const D3D12_RESOURCE_DESC1* d) {
                return g_originalCreatePlacedResource1(self, heap, offset, d, state, clear, riid, out);
            });
    }

    HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource2(
        ID3D12Device10* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
        D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, UINT32 numCastableFormats,
        const DXGI_FORMAT* castableFormats, REFIID riid, void** out) {
        return CreatePlaced(heap, desc, "placed2", out,
            [&] (const D3D12_RESOURCE_DESC1* d) {
                return g_originalCreatePlacedResource2(self, heap, offset, d, layout, clear,
                                                       numCastableFormats, castableFormats, riid, out);
            });
    }

    // GetResourceAllocationInfo is deliberately left unhooked: even a pure
    // passthrough hook on this one method crashed on a real game. It's the
    // only hooked-candidate method returning a struct >8 bytes
    // (D3D12_RESOURCE_ALLOCATION_INFO) through the COM hidden-pointer-return
    // ABI, unlike every other method here (HRESULT/void/ULONG). Cost of
    // leaving it alone: a placed resource's heap is sized for the original,
    // unreduced footprint, wasted heap space, not incorrect, since placing
    // a smaller resource in a larger heap is always valid.

    // ID3D12Device::CreateShaderResourceView
    // Remaps MostDetailedMip/MipLevels the same way barriers and copies are
    // remapped below: subtract skip, clamp to what the reduced resource
    // actually has.

    using CreateSRV_t = void(STDMETHODCALLTYPE*)(
        ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);

    CreateSRV_t g_originalCreateSRV = nullptr;

    // Clamps to at least one level rather than signalling "empty": a view
    // creation call has to produce something valid.
    void ClampViewMips(UINT& mostDetailedMip, UINT& mipLevels, float& minLodClamp, std::uint32_t skip) {
        const UINT originalMost = mostDetailedMip;
        mostDetailedMip         = originalMost >= skip ? originalMost - skip : 0;

        if (mipLevels != static_cast<UINT>(-1)) {
            // 64-bit so MostDetailedMip+MipLevels can't wrap; the result is
            // bounded by the original level count and fits back in UINT.
            const std::uint64_t originalEnd = static_cast<std::uint64_t>(originalMost) + mipLevels;
            const std::uint64_t newEnd      = originalEnd >= skip ? originalEnd - skip : 0;
            mipLevels = newEnd > mostDetailedMip ? static_cast<UINT>(newEnd - mostDetailedMip) : 1;
        }

        // ResourceMinLODClamp is a floating-point mip index into the same
        // level numbering MostDetailedMip uses (D3D12_TEX2D_SRV field docs),
        // so it has to move by the same skip. Left alone, a clamp of 3.0
        // would keep sampling three levels further down a chain that already
        // lost its top `skip` levels.
        minLodClamp = minLodClamp > static_cast<float>(skip) ? minLodClamp - static_cast<float>(skip) : 0.0f;
    }

    void STDMETHODCALLTYPE Hook_CreateShaderResourceView(
        ID3D12Device* self, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dest) {
        const auto skip = SkipFor(resource);
        if (skip == 0 || !desc) {
            g_originalCreateSRV(self, resource, desc, dest);
            return;
        }

        // TEXTURE2DARRAY too, not just TEXTURE2D: a reduced resource always has
        // DepthOrArraySize==1, but engines that describe every 2D texture
        // through the array dimension are common enough.
        D3D12_SHADER_RESOURCE_VIEW_DESC clamped = *desc;
        switch (desc->ViewDimension) {
            case D3D12_SRV_DIMENSION_TEXTURE2D:
                ClampViewMips(clamped.Texture2D.MostDetailedMip, clamped.Texture2D.MipLevels,
                              clamped.Texture2D.ResourceMinLODClamp, skip);
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
                ClampViewMips(clamped.Texture2DArray.MostDetailedMip, clamped.Texture2DArray.MipLevels,
                              clamped.Texture2DArray.ResourceMinLODClamp, skip);
                break;
            default:
                g_originalCreateSRV(self, resource, desc, dest);
                return;
        }

        g_originalCreateSRV(self, resource, &clamped, dest);
    }

    // ID3D12GraphicsCommandList::ResourceBarrier / CopyTextureRegion

    using ResourceBarrier_t =
        void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    using CopyTextureRegion_t = void(STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
        const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);

    using Barrier7_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT32, const D3D12_BARRIER_GROUP*);

    // Keyed by vtable, NOT one global each: direct, compute, copy and bundle
    // command lists are separate implementation classes in the D3D12 runtime
    // and are not guaranteed to share a vtable (EnsureCommandListHooked
    // already patches per distinct vtable for exactly that reason). A single
    // global would be overwritten by whichever vtable was patched last, and
    // calls arriving on the other one would then chain into a different
    // class's method with a foreign `this`.
    struct CommandListOriginals {
        ResourceBarrier_t resourceBarrier     = nullptr;
        CopyTextureRegion_t copyTextureRegion = nullptr;
        Barrier7_t barrier7                   = nullptr;
    };

    // shared_mutex, not mutex: written once per distinct vtable, read on every
    // barrier and copy recorded on any command list.
    std::shared_mutex g_commandListVtablesMutex;
    std::unordered_map<void*, CommandListOriginals> g_commandListOriginals;  // vtable -> originals

    // Returned by value: tiny, and it means no map reference outlives the lock.
    CommandListOriginals OriginalsFor(void* commandList) {
        const std::shared_lock lock(g_commandListVtablesMutex);
        const auto it = g_commandListOriginals.find(VtableOf(commandList));
        return it != g_commandListOriginals.end() ? it->second : CommandListOriginals{};
    }

    void STDMETHODCALLTYPE Hook_ResourceBarrier(
        ID3D12GraphicsCommandList* self, UINT numBarriers, const D3D12_RESOURCE_BARRIER* barriers) {
        const auto original = OriginalsFor(self).resourceBarrier;
        if (!original) {
            LogLine("[hook] ResourceBarrier: no original for this command list vtable, barrier dropped");
            return;
        }

        bool anyRelevant = false;
        for (UINT i = 0; i < numBarriers && !anyRelevant; ++i)
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                SkipFor(barriers[i].Transition.pResource) > 0)
                anyRelevant = true;

        if (!anyRelevant) {
            original(self, numBarriers, barriers);
            return;
        }

        // Remap or drop only the entries that need it; anything else passes
        // through with its original struct, unread past this point.
        std::vector<D3D12_RESOURCE_BARRIER> adjusted;
        adjusted.reserve(numBarriers);

        for (UINT i = 0; i < numBarriers; ++i) {
            D3D12_RESOURCE_BARRIER barrier = barriers[i];

            if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                const auto skip = SkipFor(barrier.Transition.pResource);
                if (skip > 0 && barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
                    if (barrier.Transition.Subresource < skip)
                        continue;  // targets a mip level that no longer exists
                    barrier.Transition.Subresource -= skip;
                }
            }

            adjusted.push_back(barrier);
        }

        if (!adjusted.empty())
            original(self, static_cast<UINT>(adjusted.size()), adjusted.data());
    }

    // Remaps a single copy location if it targets a reduced resource by
    // subresource index. Returns false if the whole copy should be dropped
    // (targets a mip that no longer exists).
    bool RemapCopyLocation(const D3D12_TEXTURE_COPY_LOCATION* in, D3D12_TEXTURE_COPY_LOCATION& out,
                           bool& changed) {
        changed = false;
        if (!in || in->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) return true;

        const auto skip = SkipFor(in->pResource);
        if (skip == 0) return true;
        if (in->SubresourceIndex < skip) return false;  // drop: mip no longer exists

        out             = *in;
        out.SubresourceIndex -= skip;
        changed = true;
        return true;
    }

    void STDMETHODCALLTYPE Hook_CopyTextureRegion(
        ID3D12GraphicsCommandList* self, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY,
        UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* srcBox) {
        const auto original = OriginalsFor(self).copyTextureRegion;
        if (!original) {
            LogLine("[hook] CopyTextureRegion: no original for this command list vtable, copy dropped");
            return;
        }

        D3D12_TEXTURE_COPY_LOCATION adjustedDst{};
        D3D12_TEXTURE_COPY_LOCATION adjustedSrc{};
        bool dstChanged = false;
        bool srcChanged = false;

        if (!RemapCopyLocation(dst, adjustedDst, dstChanged)) return;  // dst mip no longer exists
        if (!RemapCopyLocation(src, adjustedSrc, srcChanged)) return;  // src mip no longer exists

        original(self, dstChanged ? &adjustedDst : dst, dstX, dstY, dstZ,
                 srcChanged ? &adjustedSrc : src, srcBox);
    }

    // ID3D12GraphicsCommandList7::Barrier (Enhanced Barriers)
    // A structurally separate barrier API (Windows 11 24H2+ / Agility SDK
    // 1.613+): D3D12_TEXTURE_BARRIER addresses subresources through a
    // D3D12_BARRIER_SUBRESOURCE_RANGE, not the single index the legacy
    // ResourceBarrier above uses. An engine submitting through this API
    // instead of (or alongside) the legacy one would otherwise get reduced
    // resources with unremapped barriers, a silent state desync.
    //
    // Per the official field docs (ns-d3d12-d3d12_barrier_subresource_range):
    // NumMipLevels==0 means IndexOrFirstMipLevel is a legacy-style single
    // subresource index (0xffffffff = all subresources); otherwise it's a
    // [IndexOrFirstMipLevel, +NumMipLevels) range. Every resource this tool
    // reduces has DepthOrArraySize==1 and a single plane, so an index/range
    // is a pure mip index/range, no array/plane multiplication needed.

    // Returns false if the barrier should be dropped entirely (every mip it
    // targeted no longer exists after the skip).
    bool RemapTextureBarrier(D3D12_TEXTURE_BARRIER& barrier) {
        const auto skip = SkipFor(barrier.pResource);
        if (skip == 0) return true;

        auto& range = barrier.Subresources;
        if (range.NumMipLevels == 0) {
            if (range.IndexOrFirstMipLevel == 0xffffffffu) return true;  // all subresources
            if (range.IndexOrFirstMipLevel < skip) return false;
            range.IndexOrFirstMipLevel -= skip;
            return true;
        }

        // 64-bit so first+count can't wrap: a wrapped end would compare below
        // skip and silently drop a barrier that should still apply. The final
        // NumMipLevels is at most the original count, so it fits back in UINT.
        const UINT firstMip        = range.IndexOrFirstMipLevel;
        const std::uint64_t endMip = static_cast<std::uint64_t>(firstMip) + range.NumMipLevels;  // exclusive
        if (endMip <= skip) return false;  // every targeted mip was removed

        const UINT newFirst        = (std::max)(firstMip, skip) - skip;
        range.IndexOrFirstMipLevel = newFirst;
        range.NumMipLevels         = static_cast<UINT>(endMip - skip - newFirst);
        return true;
    }

    void STDMETHODCALLTYPE Hook_Barrier7(
        ID3D12GraphicsCommandList7* self, UINT32 numGroups, const D3D12_BARRIER_GROUP* groups) {
        const auto original = OriginalsFor(self).barrier7;
        if (!original) {
            LogLine("[hook] Barrier: no original for this command list vtable, barrier dropped");
            return;
        }

        bool anyRelevant = false;
        for (UINT32 i = 0; i < numGroups && !anyRelevant; ++i) {
            if (groups[i].Type != D3D12_BARRIER_TYPE_TEXTURE) continue;
            for (UINT32 j = 0; j < groups[i].NumBarriers; ++j)
                if (SkipFor(groups[i].pTextureBarriers[j].pResource) > 0) { anyRelevant = true; break; }
        }

        if (!anyRelevant) {
            original(self, numGroups, groups);
            return;
        }

        // Remap or drop only what needs it; groups/barriers untouched here
        // pass through with their original structs, unread past this point.
        std::vector<D3D12_BARRIER_GROUP> adjustedGroups;
        adjustedGroups.reserve(numGroups);
        // The reserve is load-bearing, not an optimisation: adjustedGroups
        // holds raw pointers into these inner vectors, so the outer vector
        // must never reallocate. At most one entry is added per group, so
        // reserving numGroups guarantees it can't.
        std::vector<std::vector<D3D12_TEXTURE_BARRIER>> textureStorage;
        textureStorage.reserve(numGroups);

        for (UINT32 i = 0; i < numGroups; ++i) {
            if (groups[i].Type != D3D12_BARRIER_TYPE_TEXTURE) {
                adjustedGroups.push_back(groups[i]);
                continue;
            }

            textureStorage.emplace_back();
            auto& remapped = textureStorage.back();
            remapped.reserve(groups[i].NumBarriers);
            for (UINT32 j = 0; j < groups[i].NumBarriers; ++j) {
                D3D12_TEXTURE_BARRIER barrier = groups[i].pTextureBarriers[j];
                if (RemapTextureBarrier(barrier)) remapped.push_back(barrier);
            }
            if (remapped.empty()) continue;  // every barrier in this group was dropped

            D3D12_BARRIER_GROUP group = groups[i];
            group.NumBarriers      = static_cast<UINT32>(remapped.size());
            group.pTextureBarriers = remapped.data();
            adjustedGroups.push_back(group);
        }

        if (!adjustedGroups.empty())
            original(self, static_cast<UINT32>(adjustedGroups.size()), adjustedGroups.data());
    }

    // ID3D12Device::CreateCommandList / ID3D12Device4::CreateCommandList1
    // Both are hooked: an engine that only creates lists through
    // CreateCommandList1 (returns them pre-closed, no allocator/PSO needed
    // up front) would otherwise get unremapped barriers/copies. Patches
    // again for any vtable not seen yet, since direct, bundle and compute lists
    // could plausibly differ.

    using CreateCommandList_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*,
        REFIID, void**);
    using CreateCommandList1_t = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);

    CreateCommandList_t g_originalCreateCommandList   = nullptr;
    CreateCommandList1_t g_originalCreateCommandList1 = nullptr;

    void EnsureCommandListHooked(void* commandList) {
        void* vtable = VtableOf(commandList);

        // Held across both the patching and the map insert, so a hook that is
        // already live in the vtable can never observe a missing original:
        // the hooks take this same mutex to look one up.
        const std::scoped_lock lock(g_commandListVtablesMutex);
        if (g_commandListOriginals.count(vtable)) return;

        CommandListOriginals originals;

        if (!PatchSlot(commandList, kSlot_ResourceBarrier,
                       reinterpret_cast<void*>(&Hook_ResourceBarrier), &originals.resourceBarrier))
            LogLine("[hook] ResourceBarrier FAILED to patch on a command list vtable");

        if (!PatchSlot(commandList, kSlot_CopyTextureRegion,
                       reinterpret_cast<void*>(&Hook_CopyTextureRegion), &originals.copyTextureRegion))
            LogLine("[hook] CopyTextureRegion FAILED to patch on a command list vtable");

        // Enhanced Barriers only exist on ID3D12GraphicsCommandList7+; probe
        // via QueryInterface rather than assuming slot 80 exists: patching
        // a slot beyond the real vtable's end on an older runtime/driver
        // would corrupt adjacent memory instead of just failing cleanly.
        ID3D12GraphicsCommandList7* commandList7 = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(commandList)->QueryInterface(IID_PPV_ARGS(&commandList7))) &&
            commandList7) {
            if (!PatchSlot(commandList7, kSlot_Barrier7, reinterpret_cast<void*>(&Hook_Barrier7),
                           &originals.barrier7))
                LogLine("[hook] Barrier (Enhanced Barriers) FAILED to patch on a command list vtable");
            commandList7->Release();
        } else {
            LogLine("[hook] ID3D12GraphicsCommandList7 unsupported, Enhanced Barriers not in use, legacy ResourceBarrier hook only");
        }

        g_commandListOriginals[vtable] = originals;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
        ID3D12Device* self, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initialState, REFIID riid, void** out) {
        const HRESULT hr =
            g_originalCreateCommandList(self, nodeMask, type, allocator, initialState, riid, out);

        if (SUCCEEDED(hr) && out && *out) EnsureCommandListHooked(*out);

        return hr;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateCommandList1(
        ID3D12Device4* self, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid, void** out) {
        const HRESULT hr = g_originalCreateCommandList1(self, nodeMask, type, flags, riid, out);

        if (SUCCEEDED(hr) && out && *out) EnsureCommandListHooked(*out);

        return hr;
    }

    // device-level install

    std::atomic<bool> g_deviceHooked{false};

    // D3D12 devices are singletons per adapter (D3D12CreateDevice returns the
    // existing one on a repeat call for the same LUID), and every instance
    // for a given adapter shares one vtable, so patching the first is enough.
    void HookDevice(ID3D12Device* device) {
        if (!device || g_deviceHooked.exchange(true)) return;

        if (!PatchSlot(device, kSlot_CreateCommittedResource,
                        reinterpret_cast<void*>(&Hook_CreateCommittedResource),
                        &g_originalCreateCommittedResource)) {
            LogLine("[hook] CreateCommittedResource FAILED to patch, no reduction will happen");
            g_deviceHooked.store(false);
            return;
        }
        LogLine("[hook] CreateCommittedResource patched");

        if (!PatchSlot(device, kSlot_CreatePlacedResource,
                       reinterpret_cast<void*>(&Hook_CreatePlacedResource),
                       &g_originalCreatePlacedResource))
            LogLine("[hook] CreatePlacedResource FAILED to patch");

        if (!PatchSlot(device, kSlot_CreateShaderResourceView,
                       reinterpret_cast<void*>(&Hook_CreateShaderResourceView), &g_originalCreateSRV))
            LogLine("[hook] CreateShaderResourceView FAILED to patch, reduced textures won't get their views clamped");

        if (!PatchSlot(device, kSlot_CreateCommandList,
                       reinterpret_cast<void*>(&Hook_CreateCommandList), &g_originalCreateCommandList))
            LogLine("[hook] CreateCommandList FAILED to patch, barriers/copies won't be remapped");

        // CreateCommandList1 only exists on ID3D12Device4+; probe via
        // QueryInterface rather than assuming slot 51 exists: patching a
        // slot beyond the real vtable's end on an older runtime would corrupt
        // adjacent memory instead of just failing cleanly.
        ID3D12Device4* device4 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4))) && device4) {
            if (!PatchSlot(device4, kSlot_CreateCommandList1,
                           reinterpret_cast<void*>(&Hook_CreateCommandList1),
                           &g_originalCreateCommandList1))
                LogLine("[hook] CreateCommandList1 FAILED to patch, lists created that way won't have "
                        "their barriers/copies remapped");

            if (!PatchSlot(device4, kSlot_CreateCommittedResource1,
                           reinterpret_cast<void*>(&Hook_CreateCommittedResource1),
                           &g_originalCreateCommittedResource1))
                LogLine("[hook] CreateCommittedResource1 FAILED to patch");
            device4->Release();
        } else {
            LogLine("[hook] ID3D12Device4 unsupported, CreateCommandList1 not available, "
                    "CreateCommandList hook only");
        }

        // The DESC1 overloads, each probed the same way: patching a slot past
        // the real vtable's end would corrupt adjacent memory.
        ID3D12Device8* device8 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8))) && device8) {
            if (!PatchSlot(device8, kSlot_CreateCommittedResource2,
                           reinterpret_cast<void*>(&Hook_CreateCommittedResource2),
                           &g_originalCreateCommittedResource2))
                LogLine("[hook] CreateCommittedResource2 FAILED to patch");

            if (!PatchSlot(device8, kSlot_CreatePlacedResource1,
                           reinterpret_cast<void*>(&Hook_CreatePlacedResource1),
                           &g_originalCreatePlacedResource1))
                LogLine("[hook] CreatePlacedResource1 FAILED to patch");
            device8->Release();
        }

        // What an engine using Enhanced Barriers creates through, the counterpart
        // to the Barrier hook above.
        ID3D12Device10* device10 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10))) && device10) {
            if (!PatchSlot(device10, kSlot_CreateCommittedResource3,
                           reinterpret_cast<void*>(&Hook_CreateCommittedResource3),
                           &g_originalCreateCommittedResource3))
                LogLine("[hook] CreateCommittedResource3 FAILED to patch");

            if (!PatchSlot(device10, kSlot_CreatePlacedResource2,
                           reinterpret_cast<void*>(&Hook_CreatePlacedResource2),
                           &g_originalCreatePlacedResource2))
                LogLine("[hook] CreatePlacedResource2 FAILED to patch");
            device10->Release();
        }
    }
}

extern "C" {

HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minLevel, REFIID riid, void** device) {
    Init();
    if (!g_realCreateDevice) {
        LogLine("[hook] D3D12CreateDevice: no real function pointer (see the earlier LoadLibraryW line)");
        return E_NOINTERFACE;
    }

    const HRESULT hr = g_realCreateDevice(adapter, minLevel, riid, device);

    // Resolved by QueryInterface, NOT by comparing riid against ID3D12Device:
    // engines routinely ask straight for a derived interface (ID3D12Device5
    // for DXR, ID3D12Device8/10 for newer features), and an riid equality test
    // would then quietly skip every hook and reduce nothing at all, with no
    // trace in the log. Every ID3D12DeviceN derives from ID3D12Device, so the
    // slots patched below are valid on all of them.
    if (SUCCEEDED(hr) && device && *device) {
        ID3D12Device* base = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*device)->QueryInterface(IID_PPV_ARGS(&base))) && base) {
            HookDevice(base);
            base->Release();
        } else {
            LogLine("[hook] D3D12CreateDevice succeeded but the object doesn't expose ID3D12Device, "
                    "no reduction will happen");
        }
    }
    if (SUCCEEDED(hr)) InstallVramSpoof(adapter);

    return hr;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** debug) {
    Init();
    return g_realGetDebugInterface ? g_realGetDebugInterface(riid, debug) : E_NOINTERFACE;
}

HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void** out) {
    Init();
    return g_realGetInterface ? g_realGetInterface(rclsid, riid, out) : E_NOINTERFACE;
}

HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* rootSig, D3D_ROOT_SIGNATURE_VERSION version,
    ID3DBlob** blob, ID3DBlob** error) {
    Init();
    return g_realSerializeRootSignature ? g_realSerializeRootSignature(rootSig, version, blob, error)
                                         : E_NOINTERFACE;
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rootSig, ID3DBlob** blob, ID3DBlob** error) {
    Init();
    return g_realSerializeVersionedRootSignature ? g_realSerializeVersionedRootSignature(rootSig, blob, error)
                                                  : E_NOINTERFACE;
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID data, SIZE_T size, REFIID riid, void** out) {
    Init();
    return g_realCreateRootSignatureDeserializer ? g_realCreateRootSignatureDeserializer(data, size, riid, out)
                                                  : E_NOINTERFACE;
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID data, SIZE_T size, REFIID riid, void** out) {
    Init();
    return g_realCreateVersionedRootSignatureDeserializer
               ? g_realCreateVersionedRootSignatureDeserializer(data, size, riid, out)
               : E_NOINTERFACE;
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT numFeatures, const IID* iids, void* configs, UINT* configSizes) {
    Init();
    return g_realEnableExperimentalFeatures
               ? g_realEnableExperimentalFeatures(numFeatures, iids, configs, configSizes)
               : E_NOINTERFACE;
}

}  // extern "C"

// The one DllMain for this binary (also covers D3D11Hooks.cpp, linked into
// the same target, see Common.h for why it isn't shared via that header).
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
