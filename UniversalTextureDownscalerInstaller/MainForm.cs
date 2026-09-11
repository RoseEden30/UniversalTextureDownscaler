namespace UniversalTextureDownscalerInstaller;

public sealed class MainForm : Form
{
    private readonly TextBox _exePathBox = new() { ReadOnly = true, Location = new Point(12, 34), Size = new Size(336, 23) };
    private readonly Label _detectedLabel = new() { Location = new Point(12, 68), AutoSize = true, Font = new Font(Control.DefaultFont, FontStyle.Bold) };

    private readonly RadioButton _rbAuto = new() { Text = "Auto (detected)", Location = new Point(12, 94), AutoSize = true, Checked = true };
    private readonly RadioButton _rbD3D11 = new() { Text = "DirectX 11", Location = new Point(140, 94), AutoSize = true };
    private readonly RadioButton _rbD3D12 = new() { Text = "DirectX 12", Location = new Point(240, 94), AutoSize = true };
    private readonly RadioButton _rbVulkan = new() { Text = "Vulkan", Location = new Point(340, 94), AutoSize = true };

    private readonly ComboBox _maxSizeBox = new()
    {
        Location = new Point(110, 126), Size = new Size(90, 23), DropDownStyle = ComboBoxStyle.DropDownList,
    };
    private readonly CheckBox _enabledBox = new() { Text = "Enabled", Location = new Point(12, 158), AutoSize = true, Checked = true };
    private readonly CheckBox _verboseBox = new() { Text = "Verbose logging (large log files)", Location = new Point(110, 158), AutoSize = true };

    private readonly Button _installButton = new() { Text = "Install", Location = new Point(12, 184), Size = new Size(100, 28) };
    private readonly Button _uninstallButton = new() { Text = "Uninstall", Location = new Point(120, 184), Size = new Size(100, 28) };
    private readonly Label _statusLabel = new() { Location = new Point(12, 220), Size = new Size(430, 40), AutoSize = false };

    private GraphicsApi _detectedApi = GraphicsApi.Unknown;

    public MainForm()
    {
        Text = "UniversalTextureDownscaler Installer";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        ClientSize = new Size(460, 270);
        StartPosition = FormStartPosition.CenterScreen;

        var exeLabel = new Label { Text = "Game .exe:", Location = new Point(12, 16), AutoSize = true };
        var browseButton = new Button { Text = "Browse...", Location = new Point(356, 33), Size = new Size(92, 25) };
        browseButton.Click += (_, _) => BrowseForExe();

        var maxSizeLabel = new Label { Text = "MaxSize:", Location = new Point(12, 129), AutoSize = true };
        _maxSizeBox.Items.AddRange(["512", "1024", "2048", "4096"]);
        _maxSizeBox.SelectedItem = "1024";

        _installButton.Click += (_, _) => DoInstall();
        _uninstallButton.Click += (_, _) => DoUninstall();

        Controls.AddRange([
            exeLabel, _exePathBox, browseButton, _detectedLabel,
            _rbAuto, _rbD3D11, _rbD3D12, _rbVulkan,
            maxSizeLabel, _maxSizeBox, _enabledBox, _verboseBox,
            _installButton, _uninstallButton, _statusLabel,
        ]);

        SetDetected(GraphicsApi.Unknown);
    }

    private void BrowseForExe()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "Game executable (*.exe)|*.exe",
            Title = "Select the game's .exe",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        var exePath = ApiDetect.ResolveRealExecutable(dialog.FileName);
        _exePathBox.Text = exePath;
        SetDetected(ApiDetect.Detect(exePath));
    }

    private void SetDetected(GraphicsApi api)
    {
        _detectedApi = api;
        _detectedLabel.Text = api == GraphicsApi.Unknown
            ? "Detected API: unknown, pick one below"
            : $"Detected API: {ApiName(api)}";
        _rbAuto.Text = api == GraphicsApi.Unknown ? "Auto (unknown)" : $"Auto ({ApiName(api)})";
    }

    private GraphicsApi SelectedApi() =>
        _rbD3D11.Checked ? GraphicsApi.D3D11 :
        _rbD3D12.Checked ? GraphicsApi.D3D12 :
        _rbVulkan.Checked ? GraphicsApi.Vulkan :
        _detectedApi;

    private static string ApiName(GraphicsApi api) => api switch
    {
        GraphicsApi.D3D11 => "DirectX 11",
        GraphicsApi.D3D12 => "DirectX 12",
        GraphicsApi.Vulkan => "Vulkan",
        _ => "unknown",
    };

    private string? GameFolder() =>
        string.IsNullOrWhiteSpace(_exePathBox.Text) ? null : Path.GetDirectoryName(_exePathBox.Text);

    private void DoInstall()
    {
        var folder = GameFolder();
        if (folder is null) { ShowStatus("Pick a game .exe first."); return; }

        var api = SelectedApi();
        if (api == GraphicsApi.Unknown)
        {
            ShowStatus("Couldn't detect the API automatically, pick DirectX 11/12 or Vulkan above.");
            return;
        }

        try
        {
            // A d3d11.dll/d3d12.dll already in the folder is very often another
            // mod's, overwriting it silently would break that mod.
            var conflicts = Installer.ConflictingFiles(folder, api);
            if (conflicts.Count > 0)
            {
                var answer = MessageBox.Show(
                    this,
                    $"\"{string.Join("\", \"", conflicts)}\" already exists here and wasn't installed by this "
                    + "tool. It most likely belongs to another mod.\n\n"
                    + "Overwrite it anyway?",
                    "File already in place", MessageBoxButtons.YesNo, MessageBoxIcon.Warning,
                    MessageBoxDefaultButton.Button2);
                if (answer != DialogResult.Yes) { ShowStatus("Install cancelled."); return; }
            }

            var settings = new InstallSettings(
                _enabledBox.Checked, int.Parse((string)_maxSizeBox.SelectedItem!), _verboseBox.Checked);

            if (api == GraphicsApi.Vulkan && !Program.IsElevated())
            {
                var args = $"--install-vulkan \"{folder}\" {(settings.Enabled ? 1 : 0)} "
                           + $"{settings.MaxSize} {(settings.Verbose ? 1 : 0)}";
                ShowStatus(Program.RunElevated(args)
                    ? $"Installed for Vulkan in \"{folder}\"."
                    : "Vulkan install needs administrator, it was cancelled or failed.");
                return;
            }

            Installer.Install(folder, api, settings);
            ShowStatus($"Installed for {ApiName(api)} in \"{folder}\".");
        }
        catch (Exception ex)
        {
            ShowStatus($"Install failed: {ex.Message}");
        }
    }

    private void DoUninstall()
    {
        var folder = GameFolder();
        if (folder is null) { ShowStatus("Pick a game .exe first."); return; }

        try
        {
            if (Installer.IsVulkanInstalled(folder) && !Program.IsElevated())
            {
                ShowStatus(Program.RunElevated($"--uninstall-vulkan \"{folder}\"")
                    ? $"Removed from \"{folder}\"."
                    : "Vulkan uninstall needs administrator, it was cancelled or failed.");
                return;
            }

            var leftAlone = Installer.Uninstall(folder);
            ShowStatus(leftAlone.Count == 0
                ? $"Removed from \"{folder}\"."
                : $"Removed from \"{folder}\". Left in place (not installed by this tool): "
                  + string.Join(", ", leftAlone));
        }
        catch (Exception ex)
        {
            ShowStatus($"Uninstall failed: {ex.Message}");
        }
    }

    private void ShowStatus(string message) => _statusLabel.Text = message;
}
