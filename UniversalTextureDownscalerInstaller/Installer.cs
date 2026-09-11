using System.Diagnostics;
using System.Reflection;
using Microsoft.Win32;

namespace UniversalTextureDownscalerInstaller;

public sealed record InstallSettings(bool Enabled, int MaxSize, bool Verbose);

/// Drops the right proxy/layer file(s) next to a game's exe and writes the shared .ini.
/// Uninstall removes exactly what this writes, no separate install record needed.
public static class Installer
{
    private const string VulkanRegistryValueSuffix = @"Software\Khronos\Vulkan\ImplicitLayers";

    /// Matches the ProductName in src/version.rc, compiled into both binaries.
    private const string ProductMarker = "UniversalTextureDownscaler";

    /// The D3D proxy deploys under a name other mods use too, so a file existing
    /// there doesn't mean this tool put it there, overwriting or deleting someone
    /// else's file would silently break it.
    public static bool IsOurs(string path)
    {
        try
        {
            return File.Exists(path) && FileVersionInfo.GetVersionInfo(path).ProductName == ProductMarker;
        }
        catch
        {
            return false;  // unreadable/no version resource, assume it isn't ours
        }
    }

    /// File names Install would overwrite that already exist and are not ours.
    /// Empty means Install is safe to run unattended.
    public static IReadOnlyList<string> ConflictingFiles(string gameFolder, GraphicsApi api)
    {
        var targets = api switch
        {
            GraphicsApi.D3D11 => new[] { "d3d11.dll" },
            GraphicsApi.D3D12 => new[] { "d3d12.dll" },
            _ => [],
        };

        return targets.Where(name => !IsOurs(Path.Combine(gameFolder, name))
                                     && File.Exists(Path.Combine(gameFolder, name))).ToList();
    }

    private static string SharedVulkanDirectory() =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                     "UniversalTextureDownscaler");

    private static string InstalledGamesListPath() =>
        Path.Combine(SharedVulkanDirectory(), "InstalledGames.txt");

    public static bool IsVulkanInstalled(string gameFolder) => ReadInstalledGames().Contains(gameFolder);

    private static HashSet<string> ReadInstalledGames()
    {
        var path = InstalledGamesListPath();
        return File.Exists(path)
            ? new HashSet<string>(File.ReadAllLines(path).Where(l => l.Length > 0), StringComparer.OrdinalIgnoreCase)
            : new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    }

    public static void Install(string gameFolder, GraphicsApi api, InstallSettings settings)
    {
        switch (api)
        {
            // Same binary for both APIs (see CMakeLists.txt), just written out
            // under whichever name the game needs to load it.
            case GraphicsApi.D3D11:
                ExtractResource("d3d.dll", Path.Combine(gameFolder, "d3d11.dll"));
                break;
            case GraphicsApi.D3D12:
                ExtractResource("d3d.dll", Path.Combine(gameFolder, "d3d12.dll"));
                break;
            case GraphicsApi.Vulkan:
                var vulkanDir = SharedVulkanDirectory();
                Directory.CreateDirectory(vulkanDir);
                var dllPath = Path.Combine(vulkanDir, "UniversalTextureDownscaler_Vulkan.dll");
                var jsonPath = Path.Combine(vulkanDir, "UniversalTextureDownscaler_Vulkan.json");
                ExtractResource("UniversalTextureDownscaler_Vulkan.dll", dllPath);
                ExtractResource("UniversalTextureDownscaler_Vulkan.json", jsonPath);
                RegisterVulkanLayer(jsonPath);

                var games = ReadInstalledGames();
                games.Add(gameFolder);
                File.WriteAllLines(InstalledGamesListPath(), games);
                break;
            default:
                throw new InvalidOperationException("No graphics API selected.");
        }

        WriteIni(gameFolder, settings);
    }

    /// Removes everything Install can write. Returns D3D file names left in place
    /// because they belong to something else.
    public static IReadOnlyList<string> Uninstall(string gameFolder)
    {
        var leftAlone = new List<string>();
        foreach (var name in new[] { "d3d11.dll", "d3d12.dll" })
        {
            var path = Path.Combine(gameFolder, name);
            if (!File.Exists(path)) continue;
            if (IsOurs(path)) TryDelete(path);
            else leftAlone.Add(name);
        }
        TryDelete(Path.Combine(gameFolder, "UniversalTextureDownscaler.ini"));

        var games = ReadInstalledGames();
        if (games.Remove(gameFolder))
        {
            if (games.Count > 0)
            {
                File.WriteAllLines(InstalledGamesListPath(), games);
            }
            else
            {
                var vulkanDir = SharedVulkanDirectory();
                var jsonPath = Path.Combine(vulkanDir, "UniversalTextureDownscaler_Vulkan.json");
                UnregisterVulkanLayer(jsonPath);
                TryDelete(jsonPath);
                TryDelete(Path.Combine(vulkanDir, "UniversalTextureDownscaler_Vulkan.dll"));
                TryDelete(InstalledGamesListPath());
                try { Directory.Delete(vulkanDir); } catch { /* not empty or in use, leave it */ }
            }
        }

        return leftAlone;
    }

    private static void RegisterVulkanLayer(string manifestPath)
    {
        using var key = Registry.LocalMachine.CreateSubKey(VulkanRegistryValueSuffix);
        key.SetValue(manifestPath, 0, RegistryValueKind.DWord);
    }

    private static void UnregisterVulkanLayer(string manifestPath)
    {
        using (var key = Registry.LocalMachine.OpenSubKey(VulkanRegistryValueSuffix, writable: true))
            key?.DeleteValue(manifestPath, throwOnMissingValue: false);
        // clear the per-user key older builds used
        using (var legacy = Registry.CurrentUser.OpenSubKey(VulkanRegistryValueSuffix, writable: true))
            legacy?.DeleteValue(manifestPath, throwOnMissingValue: false);
    }

    private static void ExtractResource(string logicalName, string destinationPath)
    {
        using var resource = Assembly.GetExecutingAssembly().GetManifestResourceStream(logicalName)
            ?? throw new InvalidOperationException($"Embedded resource '{logicalName}' is missing from this build.");
        using var file = new FileStream(destinationPath, FileMode.Create, FileAccess.Write);
        resource.CopyTo(file);
    }

    private static void WriteIni(string gameFolder, InstallSettings settings)
    {
        var lines = new[]
        {
            "[Settings]",
            "; turn off to disable the mod without uninstalling",
            $"Enabled={(settings.Enabled ? 1 : 0)}",
            "; textures larger than this (pixels) get their top mip levels dropped",
            $"MaxSize={settings.MaxSize}",
            "; logs every texture candidate and why it was accepted/rejected, noisy, for troubleshooting",
            $"Verbose={(settings.Verbose ? 1 : 0)}",
            "; caps the VRAM budget the game itself sees, in MB, 0 disables this (testing only)",
            "FakeVramBudgetMB=0",
        };
        File.WriteAllLines(Path.Combine(gameFolder, "UniversalTextureDownscaler.ini"), lines);
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch { /* best-effort: file in use, permissions, etc., not fatal to the rest of uninstall */ }
    }
}
