using System.Runtime.InteropServices;

namespace UniversalTextureDownscalerInstaller;

public sealed class MainForm : Form
{
    private static readonly Color Face = Color.FromArgb(198, 198, 198);
    private static readonly Color Navy = Color.FromArgb(0, 0, 128);
    private static readonly Color Ink = Color.Black;
    private static readonly Color Good = Color.FromArgb(0, 100, 0);
    private static readonly Color Bad = Color.FromArgb(160, 0, 0);
    private static readonly Font Ui = new("MS Sans Serif", 8.25f);
    private static readonly Font UiBold = new("MS Sans Serif", 8.25f, FontStyle.Bold);

    private static readonly int[] MaxSizeChoices = [512, 1024, 2048, 4096];
    private const int DefaultMaxSize = 2048;  // matches the mod's own default

    private readonly Panel _titleBar = new() { BackColor = Navy };
    private readonly TableLayoutPanel _rows = new()
    {
        AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 2,
        Padding = new Padding(12, 10, 12, 8), BackColor = Face,
    };

    private readonly TextBox _exePathBox = new()
    {
        ReadOnly = true, BorderStyle = BorderStyle.Fixed3D, BackColor = Color.White, Font = Ui,
    };
    private readonly Label _detectedLabel = Caption("");
    private readonly Label _statusLabel = Caption("");

    private readonly RadioButton _rbAuto = Radio("auto", true);
    private readonly RadioButton _rbD3D11 = Radio("d3d11", false);
    private readonly RadioButton _rbD3D12 = Radio("d3d12", false);
    private readonly RadioButton _rbVulkan = Radio("vulkan", false);
    private readonly ComboBox _maxSizeBox = new()
    {
        FlatStyle = FlatStyle.System, BackColor = Color.White, Font = Ui,
        DropDownStyle = ComboBoxStyle.DropDownList,
    };
    private readonly CheckBox _enabledBox = Check("Enabled", true);
    private readonly CheckBox _verboseBox = Check("Verbose logging", false);

    private readonly RetroButton _install = new() { Text = "Install" };
    private readonly RetroButton _remove = new() { Text = "Remove" };

    private GraphicsApi _detectedApi = GraphicsApi.Unknown;

    public MainForm()
    {
        Text = "UniversalTextureDownscaler";
        Icon = LoadIcon();
        BackColor = Face;
        ForeColor = Ink;
        Font = Ui;
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.CenterScreen;
        DoubleBuffered = true;
        // Sizes come from measured text, not pixel constants, so the window fits
        // its content at any DPI.
        AutoScaleMode = AutoScaleMode.Dpi;

        var em = TextRenderer.MeasureText("0", Ui).Width;
        _exePathBox.Width = em * 52;
        _maxSizeBox.Width = em * 12;
        _statusLabel.MaximumSize = new Size(em * 64, 0);

        var browse = new RetroButton { Text = "...", Size = new Size(em * 6, _exePathBox.Height) };
        browse.Click += (_, _) => BrowseForExe();

        _install.Size = new Size(em * 16, 26);
        _install.Click += async (_, _) => await DoInstall();
        _remove.Size = new Size(em * 16, 26);
        _remove.Click += async (_, _) => await DoUninstall();
        var about = new RetroButton { Text = "About", Size = new Size(em * 14, 26) };
        about.Click += (_, _) => ShowAbout();
        var exit = new RetroButton { Text = "Exit", Size = new Size(em * 12, 26) };
        exit.Click += (_, _) => Close();

        SelectMaxSize(DefaultMaxSize);
        SetDetected(GraphicsApi.Unknown);

        AddRow("Game .exe :", Line(_exePathBox, browse));
        AddRow("", _detectedLabel);
        AddRow("API :", Line(_rbAuto, _rbD3D11, _rbD3D12, _rbVulkan));
        AddRow("MaxSize :", _maxSizeBox);
        AddRow("Options :", Line(_enabledBox, _verboseBox));
        AddSpan(new Panel { Height = 2, Dock = DockStyle.Fill, BackColor = Face });
        AddRow("", Line(_install, _remove, about, exit));
        AddRow("", _statusLabel);

        BuildTitleBar();
        Controls.Add(_rows);
        _rows.PerformLayout();

        var body = _rows.PreferredSize;
        _titleBar.Size = new Size(body.Width, TextRenderer.MeasureText("X", UiBold).Height + 8);
        _titleBar.Location = new Point(4, 4);
        _rows.Location = new Point(4, _titleBar.Bottom);
        ClientSize = new Size(body.Width + 8, _titleBar.Height + body.Height + 8);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        ControlPaint.DrawBorder3D(e.Graphics, ClientRectangle, Border3DStyle.Raised);
    }

    private void BuildTitleBar()
    {
        var title = new Label
        {
            Text = "  UniversalTextureDownscaler", ForeColor = Color.White, Font = UiBold,
            AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft,
        };
        _titleBar.Controls.Add(title);
        _titleBar.MouseDown += DragWindow;
        title.MouseDown += DragWindow;
        Controls.Add(_titleBar);
    }

    // layout

    private void AddSpan(Control content)
    {
        _rows.Controls.Add(content, 0, _rows.RowCount);
        _rows.SetColumnSpan(content, 2);
        _rows.RowCount++;
    }

    private void AddRow(string label, Control content)
    {
        var caption = Caption(label);
        caption.Margin = new Padding(0, 6, 10, 4);
        caption.Anchor = AnchorStyles.Left;
        content.Margin = new Padding(0, 4, 0, 4);
        content.Anchor = AnchorStyles.Left;

        _rows.Controls.Add(caption, 0, _rows.RowCount);
        _rows.Controls.Add(content, 1, _rows.RowCount);
        _rows.RowCount++;
    }

    private static FlowLayoutPanel Line(params Control[] controls)
    {
        var line = new FlowLayoutPanel
        {
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, BackColor = Face,
        };
        foreach (var control in controls)
        {
            control.Margin = new Padding(0, 0, 6, 0);
            line.Controls.Add(control);
        }
        return line;
    }

    private static Icon? LoadIcon()
    {
        try
        {
            using var stream = System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("app.ico");
            return stream is null ? null : new Icon(stream);
        }
        catch
        {
            return null;
        }
    }

    private static Label Caption(string text) => new()
    {
        Text = text, AutoSize = true, ForeColor = Ink, BackColor = Face, Font = Ui,
    };

    private static RadioButton Radio(string text, bool @checked) => new()
    {
        Text = text, Checked = @checked, AutoSize = true, ForeColor = Ink, BackColor = Face,
        FlatStyle = FlatStyle.System, Font = Ui,
    };

    private static CheckBox Check(string text, bool @checked) => new()
    {
        Text = text, Checked = @checked, AutoSize = true, ForeColor = Ink, BackColor = Face,
        FlatStyle = FlatStyle.System, Font = Ui,
    };

    // behaviour

    private void ShowAbout()
    {
        var version = Application.ProductVersion.Split('+')[0];
        var text = new Label
        {
            AutoSize = true, ForeColor = Ink, BackColor = Face, Font = Ui, Location = new Point(14, 12),
            Text = $"""
                   UniversalTextureDownscaler {version}
                   MIT licensed

                   Credits
                     TextureDownscaler   github.com/RoseEden30/TextureDownscaler
                     ReShade             github.com/crosire/reshade
                   """,
        };
        var close = new RetroButton { Text = "Close", Size = new Size(90, 26) };

        using var dialog = new Form
        {
            Text = "About", Font = Ui, BackColor = Face, ForeColor = Ink,
            FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false,
            ShowInTaskbar = false, Icon = Icon,
            StartPosition = FormStartPosition.CenterParent, AutoScaleMode = AutoScaleMode.Dpi,
        };
        close.Click += (_, _) => dialog.Close();
        dialog.CancelButton = close;

        var body = text.PreferredSize;
        close.Location = new Point(14, 12 + body.Height + 14);
        dialog.ClientSize = new Size(body.Width + 28, close.Bottom + 14);
        dialog.Controls.Add(text);
        dialog.Controls.Add(close);
        dialog.ShowDialog(this);
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
        LoadExistingState();
    }

    // Without this, reinstalling would reset a game's settings to the defaults.
    private void LoadExistingState()
    {
        var folder = GameFolder();
        if (folder is null) return;

        if (Installer.ReadSettings(folder) is { } settings)
        {
            _enabledBox.Checked = settings.Enabled;
            _verboseBox.Checked = settings.Verbose;
            SelectMaxSize(settings.MaxSize);
        }

        var installed = Installer.InstalledApi(folder);
        ShowStatus(installed is null
            ? "Not set up here yet."
            : $"Already installed for {ApiName(installed.Value)}, showing its current settings.");
    }

    private void SelectMaxSize(int value)
    {
        var choices = MaxSizeChoices.Contains(value) ? MaxSizeChoices : [.. MaxSizeChoices, value];
        _maxSizeBox.Items.Clear();
        foreach (var choice in choices.Order()) _maxSizeBox.Items.Add(choice.ToString());
        _maxSizeBox.SelectedItem = value.ToString();
    }

    private void SetDetected(GraphicsApi api)
    {
        _detectedApi = api;
        _detectedLabel.Text = api == GraphicsApi.Unknown
            ? "Detected: unknown, pick one below"
            : $"Detected: {ApiName(api)}";
        _rbAuto.Text = api == GraphicsApi.Unknown ? "auto" : $"auto ({ApiName(api)})";
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

    // A trailing backslash would escape the closing quote; doubling it makes the
    // argument parser read one literal backslash.
    private static string Quoted(string path) =>
        path.EndsWith('\\') ? $"\"{path}\\\"" : $"\"{path}\"";

    /// Keeps the window responsive while the elevated worker and its UAC prompt run.
    private async Task<bool> RunElevated(string arguments)
    {
        _install.Enabled = _remove.Enabled = false;
        try { return await Program.RunElevatedAsync(arguments); }
        finally { _install.Enabled = _remove.Enabled = true; }
    }

    private async Task DoInstall()
    {
        var folder = GameFolder();
        if (folder is null) { ShowStatus("Pick a game .exe first.", failed: true); return; }

        var api = SelectedApi();
        if (api == GraphicsApi.Unknown)
        {
            ShowStatus("Couldn't detect the API, pick DirectX 11/12 or Vulkan above.", failed: true);
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
                var args = $"--install-vulkan {Quoted(folder)} {(settings.Enabled ? 1 : 0)} "
                           + $"{settings.MaxSize} {(settings.Verbose ? 1 : 0)}";
                if (await RunElevated(args)) ShowStatus($"Installed for Vulkan in \"{folder}\".");
                else ShowStatus("Vulkan install needs administrator, it was cancelled or failed.", failed: true);
                return;
            }

            Installer.Install(folder, api, settings);
            ShowStatus($"Installed for {ApiName(api)} in \"{folder}\".");
        }
        catch (Exception ex)
        {
            ShowStatus($"Install failed: {ex.Message}", failed: true);
        }
    }

    private async Task DoUninstall()
    {
        var folder = GameFolder();
        if (folder is null) { ShowStatus("Pick a game .exe first.", failed: true); return; }

        try
        {
            if (Installer.IsVulkanInstalled(folder) && !Program.IsElevated())
            {
                if (await RunElevated($"--uninstall-vulkan {Quoted(folder)}")) ShowStatus($"Removed from \"{folder}\".");
                else ShowStatus("Vulkan uninstall needs administrator, it was cancelled or failed.", failed: true);
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
            ShowStatus($"Uninstall failed: {ex.Message}", failed: true);
        }
    }

    private void ShowStatus(string message, bool failed = false)
    {
        _statusLabel.ForeColor = failed ? Bad : Good;
        _statusLabel.Text = message;
    }

    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr wParam, IntPtr lParam);

    private void DragWindow(object? sender, MouseEventArgs e)
    {
        if (e.Button != MouseButtons.Left) return;
        ReleaseCapture();
        SendMessage(Handle, 0xA1, 0x2, 0);  // WM_NCLBUTTONDOWN, HTCAPTION
    }

    /// A classic Win9x raised/pushed button, drawn by hand so it keeps its bevel on any Windows theme.
    private sealed class RetroButton : Button
    {
        private bool _down;

        public RetroButton()
        {
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            BackColor = Face;
            Font = UiBold;
            SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
        }

        protected override void OnMouseDown(MouseEventArgs e) { _down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { _down = false; Invalidate(); base.OnMouseUp(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(Face);
            ControlPaint.DrawButton(g, ClientRectangle, _down ? ButtonState.Pushed : ButtonState.Normal);

            var text = ClientRectangle;
            if (_down) text.Offset(1, 1);
            TextRenderer.DrawText(g, Text, Font, text, Enabled ? Ink : SystemColors.GrayText,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
    }
}
