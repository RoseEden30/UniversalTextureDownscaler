// Shared by the D3D11, D3D12 and Vulkan targets: mip-skip math, COM vtable
// patching, config/logging, and the DXGI VRAM query. D3D11Hooks.cpp and
// D3D12Hooks.cpp compile into one binary (see CMakeLists.txt); each API's
// hooks only activate from that API's own CreateDevice export, so they can
// safely share this header's state. VulkanHooks.cpp is a separate binary.
#pragma once

#include <Windows.h>
#include <dxgi1_4.h>
#include <dxgiformat.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <string>

#pragma comment(lib, "dxgi.lib")

namespace Common {

inline HMODULE g_self = nullptr;

// GetModuleFileNameW truncates silently on a too-small buffer, so this grows
// until the whole path fits.
inline std::wstring DirectoryOf(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (written == 0) return L"";
        if (written < path.size()) {
            path.resize(written);
            break;
        }
        if (path.size() >= 32768) return L"";  // longest path Windows accepts at all
        path.resize(path.size() * 2);
    }

    const auto slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
}

inline std::wstring SelfDirectory() { return DirectoryOf(g_self); }
// The running game's own exe directory, not this DLL's (nullptr = the host process).
inline std::wstring HostDirectory() { return DirectoryOf(nullptr); }

// logging

inline constexpr char kVersion[] = "1.0.0";  // keep in sync with version.rc

inline std::mutex g_logMutex;
inline FILE* g_log = nullptr;

inline void OpenLog(const wchar_t* filename, const std::wstring& directory) {
    const std::scoped_lock lock(g_logMutex);
    if (g_log) return;

    const auto path = directory + filename;
    g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);  // share-read: lets the log be tailed live
    if (g_log) {
        fprintf(g_log, "UniversalTextureDownscaler v%s\n", kVersion);
        fflush(g_log);
    }
}

// A FreeLibrary (rather than process exit) runs DLL_PROCESS_DETACH with
// other threads still live, so the close has to be under the same lock
// LogLine takes.
inline void CloseLog() {
    const std::scoped_lock lock(g_logMutex);
    if (!g_log) return;
    fclose(g_log);
    g_log = nullptr;
}

inline void LogLine(const char* text) {
    const std::scoped_lock lock(g_logMutex);
    if (!g_log) return;
    fprintf(g_log, "%s\n", text);
    fflush(g_log);
}

// config

inline std::atomic<std::uint32_t> g_maxSize{2048};
inline std::atomic<bool> g_enabled{true};
// Logs every multi-mip candidate and why it was accepted/rejected. Off by
// default, real traffic can be thousands of calls/sec.
inline std::atomic<bool> g_verbose{false};
// Caps the VRAM budget the game itself sees (0 = report the real one), for
// testing how a game's streaming reacts to less VRAM than the card has.
inline std::atomic<std::uint32_t> g_fakeVramBudgetMB{0};

inline void LoadConfig(const std::wstring& directory) {
    const auto path = directory + L"UniversalTextureDownscaler.ini";
    g_maxSize.store(static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Settings", L"MaxSize", 2048, path.c_str())));
    g_enabled.store(GetPrivateProfileIntW(L"Settings", L"Enabled", 1, path.c_str()) != 0);
    g_verbose.store(GetPrivateProfileIntW(L"Settings", L"Verbose", 0, path.c_str()) != 0);
    g_fakeVramBudgetMB.store(static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Settings", L"FakeVramBudgetMB", 0, path.c_str())));
}

// generic COM vtable patching

// `original` is written before the hook goes live, so a call arriving on
// another thread the instant the slot is swapped always has something to
// chain into. Templated on the function pointer type so callers can hand it
// the very variable their hook reads, with no cast in between.
template <typename Fn>
inline bool PatchSlot(void* object, std::size_t slot, void* hook, Fn* original) {
    auto** vtable = *reinterpret_cast<void***>(object);
    void** entry  = &vtable[slot];

    DWORD previousProtection = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &previousProtection)) return false;

    void* previous = *entry;
    *original      = reinterpret_cast<Fn>(previous);

    void* swapped = InterlockedExchangePointer(static_cast<void* volatile*>(entry), hook);

    // Someone else patched the slot in between, so what we overwrote isn't
    // what we'd chain into. Put theirs back and stay out.
    const bool installed = swapped == previous && previous != nullptr;
    if (!installed) {
        InterlockedExchangePointer(static_cast<void* volatile*>(entry), swapped);
        *original = nullptr;
    }

    VirtualProtect(entry, sizeof(void*), previousProtection, &previousProtection);
    return installed;
}

inline void* VtableOf(void* object) { return *reinterpret_cast<void**>(object); }

// Universal for any COM interface, it's IUnknown's own layout.
constexpr std::size_t kSlot_Release = 2;

// BC-format / mip-skip math

constexpr std::uint32_t kMinDimension = 4;

inline bool IsBlockCompressed(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

// How many of the largest mip levels to drop. Takes a plain bool instead of
// a format enum since D3D and Vulkan don't share one.
inline std::uint32_t ComputeSkip(std::uint64_t width, std::uint32_t height, std::uint32_t mipLevels,
                                  bool blockCompressed, std::uint32_t maxSize) {
    if (maxSize == 0 || mipLevels <= 1) return 0;
    if (width <= maxSize && height <= maxSize) return 0;

    std::uint32_t skip = 0;
    std::uint64_t w    = width;
    std::uint32_t h    = height;

    while ((w > maxSize || h > maxSize) &&
           skip + 1 < mipLevels &&
           (w >> 1) >= kMinDimension && (h >> 1) >= kMinDimension) {
        w >>= 1;
        h >>= 1;
        ++skip;
    }

    // The level kept has to stay a multiple of 4 on both axes: a BC block
    // can't be split, and the pitch a driver computes for a mip always
    // describes it as a whole number of blocks.
    if (blockCompressed) {
        while (skip > 0 && (((width >> skip) & 3u) || ((height >> skip) & 3u)))
            --skip;
    }

    return skip;
}

// ~8bpp estimate, ignoring alignment/padding. Close enough for a log figure,
// never used for an actual decision.
inline std::uint64_t EstimateBytes(std::uint64_t width, std::uint32_t height, std::uint32_t mipLevels) {
    std::uint64_t total = 0;
    for (std::uint32_t level = 0; level < mipLevels; ++level) {
        const std::uint64_t w = (std::max)(std::uint64_t{1}, width >> level);
        const std::uint64_t h = (std::max)(std::uint32_t{1}, height >> level);
        total += (w * h) / 2;
    }
    return total;
}

// real VRAM, queried straight from DXGI (same number Task Manager shows)

inline IDXGIAdapter3* GetAdapter() {
    static IDXGIAdapter3* adapter = [] () -> IDXGIAdapter3* {
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

        IDXGIAdapter1* adapter1 = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter1))) {
            adapter1->QueryInterface(IID_PPV_ARGS(&adapter3));
            adapter1->Release();
        }

        factory->Release();
        return adapter3;
    }();
    return adapter;
}

// optional fake VRAM budget (D3D side)
// IDXGIAdapter3's own methods, verified against dxgi1_4.h: slot 14.
constexpr std::size_t kSlot_QueryVideoMemoryInfo = 14;

using QueryVideoMemoryInfo_t = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*);

inline std::mutex g_vramSpoofMutex;

// QueryLocalVram() calls through g_originalQueryVideoMemoryInfo directly so our
// own telemetry stays unspoofed; g_spoofedAdapterVtable guards that it's only
// chained into with an adapter of the same class it was captured from.
inline std::atomic<QueryVideoMemoryInfo_t> g_originalQueryVideoMemoryInfo{nullptr};
inline std::atomic<void*> g_spoofedAdapterVtable{nullptr};

inline HRESULT STDMETHODCALLTYPE Hook_QueryVideoMemoryInfo(
    IDXGIAdapter3* self, UINT nodeIndex, DXGI_MEMORY_SEGMENT_GROUP group,
    DXGI_QUERY_VIDEO_MEMORY_INFO* info) {
    const auto original = g_originalQueryVideoMemoryInfo.load(std::memory_order_acquire);
    const HRESULT hr     = original ? original(self, nodeIndex, group, info) : E_NOINTERFACE;

    const auto fakeMB = g_fakeVramBudgetMB.load(std::memory_order_relaxed);
    if (SUCCEEDED(hr) && info && fakeMB != 0 && group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL) {
        const auto fakeBytes = static_cast<std::uint64_t>(fakeMB) * 1024 * 1024;
        if (info->Budget > fakeBytes) info->Budget = fakeBytes;
    }
    return hr;
}

// Patches the adapter's vtable so the game's own budget queries see the fake
// cap. No-op if disabled in the .ini or already installed.
inline void InstallVramSpoof(IUnknown* adapterUnknown) {
    if (g_fakeVramBudgetMB.load(std::memory_order_relaxed) == 0) return;

    // Without this, two threads creating devices at once could both patch,
    // and the second would capture our own hook as the original.
    const std::scoped_lock lock(g_vramSpoofMutex);
    if (g_originalQueryVideoMemoryInfo.load(std::memory_order_relaxed)) return;

    IDXGIAdapter3* adapter3 = nullptr;
    if (adapterUnknown) adapterUnknown->QueryInterface(IID_PPV_ARGS(&adapter3));
    // D3D11 games commonly pass no adapter; fall back to the default one.
    if (!adapter3) {
        adapter3 = GetAdapter();
        if (adapter3) adapter3->AddRef();
    }
    if (!adapter3) {
        LogLine("[hook] IDXGIAdapter3 unsupported, FakeVramBudgetMB won't apply");
        return;
    }

    QueryVideoMemoryInfo_t original = nullptr;
    if (PatchSlot(adapter3, kSlot_QueryVideoMemoryInfo, reinterpret_cast<void*>(&Hook_QueryVideoMemoryInfo),
                  &original)) {
        g_spoofedAdapterVtable.store(VtableOf(adapter3), std::memory_order_relaxed);
        g_originalQueryVideoMemoryInfo.store(original, std::memory_order_release);
    } else {
        LogLine("[hook] QueryVideoMemoryInfo FAILED to patch, FakeVramBudgetMB won't apply");
    }

    adapter3->Release();
}

struct VramInfo {
    bool available          = false;
    std::uint64_t usageMB   = 0;
    std::uint64_t budgetMB  = 0;
};

inline VramInfo QueryLocalVram() {
    VramInfo result;
    auto* adapter = GetAdapter();
    if (!adapter) return result;

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    const auto original = g_originalQueryVideoMemoryInfo.load(std::memory_order_acquire);
    const bool chainable =
        original && VtableOf(adapter) == g_spoofedAdapterVtable.load(std::memory_order_relaxed);
    const HRESULT hr = chainable ? original(adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)
                                 : adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    if (FAILED(hr)) return result;

    result.available = true;
    result.usageMB    = info.CurrentUsage / (1024 * 1024);
    result.budgetMB   = info.Budget / (1024 * 1024);
    return result;
}

// tracked/reduced counters + periodic VRAM summary, regardless of Enabled

inline std::atomic<std::uint64_t> g_trackedCount{0};
inline std::atomic<std::uint64_t> g_reducedCount{0};
inline std::atomic<std::uint64_t> g_savedBytes{0};
constexpr std::uint64_t kSummaryInterval = 2000;

inline void RecordReduction(std::uint64_t savedBytes) {
    g_reducedCount.fetch_add(1, std::memory_order_relaxed);
    g_savedBytes.fetch_add(savedBytes, std::memory_order_relaxed);
}

inline void Heartbeat() {
    const auto count = g_trackedCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count % kSummaryInterval != 0) return;

    // DirectStorage writes textures by subresource index, unremapped here.
    // Checked on the summary tick rather than per texture: GetModuleHandleW
    // takes the loader lock, and this runs inside a graphics-API call.
    static std::atomic<bool> dstorageReported{false};
    if (!dstorageReported.load(std::memory_order_relaxed) && GetModuleHandleW(L"dstorage.dll") &&
        !dstorageReported.exchange(true, std::memory_order_relaxed))
        LogLine("[hook] dstorage.dll loaded: textures streamed through DirectStorage aren't remapped, "
                "a reduced one populated that way may come out wrong");

    const std::scoped_lock lock(g_logMutex);
    if (!g_log) return;

    const auto vram = QueryLocalVram();
    fprintf(g_log, "-- %llu tracked, %llu reduced, ~%.1f MB saved, VRAM ",
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(g_reducedCount.load(std::memory_order_relaxed)),
            static_cast<double>(g_savedBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0));
    if (vram.available)
        fprintf(g_log, "%llu/%llu MB --\n", static_cast<unsigned long long>(vram.usageMB),
                static_cast<unsigned long long>(vram.budgetMB));
    else
        fprintf(g_log, "unavailable --\n");
    fflush(g_log);
}

}  // namespace Common

// DllMain is deliberately NOT declared here. Marking it inline in this
// header still produced LNK2005 "already defined" when D3D11Hooks.cpp and
// D3D12Hooks.cpp link into one binary. MSVC's CRT startup resolves the DLL
// entry point by looking for one strong definition of the literal symbol
// "DllMain", and normal inline/COMDAT folding doesn't apply to it. Each
// binary defines its own DllMain directly in one .cpp file instead.
