using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;

namespace UniversalTextureDownscalerInstaller;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        // Headless self-check for the PE import-table detection, so it can be verified
        // against real game exes without clicking through the GUI.
        if (args.Length >= 2 && args[0] == "--detect")
        {
            // AttachConsole replaces the standard handles, so skip it when
            // output is already redirected.
            if (GetStdHandle(StdOutputHandle) == IntPtr.Zero) AttachConsole(-1);
            var exePath = ApiDetect.ResolveRealExecutable(args[1]);
            Console.WriteLine($"{exePath} -> {ApiDetect.Detect(exePath)}");
            return 0;
        }

        if (args.Length >= 5 && args[0] == "--install-vulkan")
            return RunVulkanWorker(() => Installer.Install(
                args[1], GraphicsApi.Vulkan,
                new InstallSettings(args[2] == "1", int.Parse(args[3]), args[4] == "1")));
        if (args.Length >= 2 && args[0] == "--uninstall-vulkan")
            return RunVulkanWorker(() => Installer.Uninstall(args[1]));

        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
        return 0;
    }

    private static int RunVulkanWorker(Action action)
    {
        try
        {
            action();
            return 0;
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "UniversalTextureDownscaler (Vulkan)",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    internal static bool IsElevated()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    internal static async Task<bool> RunElevatedAsync(string arguments)
    {
        try
        {
            using var process = Process.Start(new ProcessStartInfo
            {
                FileName = Environment.ProcessPath!,
                Arguments = arguments,
                UseShellExecute = true,
                Verb = "runas",
            });
            if (process is null) return false;
            await process.WaitForExitAsync();
            return process.ExitCode == 0;
        }
        catch (Win32Exception)
        {
            return false;
        }
    }

    private const int StdOutputHandle = -11;

    [DllImport("kernel32.dll")]
    private static extern bool AttachConsole(int dwProcessId);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetStdHandle(int nStdHandle);
}
