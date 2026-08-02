using System.Diagnostics;
using System.Drawing.Drawing2D;

namespace McsmVitaDataBuilder;

public sealed class MainForm : Form
{
    private static readonly Color Page = Color.FromArgb(10, 15, 28);
    private static readonly Color Card = Color.FromArgb(22, 31, 50);
    private static readonly Color Field = Color.FromArgb(11, 18, 32);
    private static readonly Color Border = Color.FromArgb(50, 64, 86);
    private static readonly Color Primary = Color.FromArgb(52, 211, 153);
    private static readonly Color PrimaryDark = Color.FromArgb(6, 95, 70);
    private static readonly Color TextMain = Color.FromArgb(241, 245, 249);
    private static readonly Color TextSoft = Color.FromArgb(148, 163, 184);
    private static readonly Color Info = Color.FromArgb(96, 165, 250);
    private static readonly Color Warning = Color.FromArgb(251, 191, 36);

    private readonly FlowLayoutPanel _page = new();
    private readonly TextBox _apkBox = new();
    private readonly TextBox _obbBox = new();
    private readonly TextBox _patchObbBox = new();
    private readonly TextBox _outputBox = new();
    private readonly Label _apkValidationLabel = new();
    private readonly Label _mainObbValidationLabel = new();
    private readonly Label _patchObbValidationLabel = new();
    private readonly Label _baseNoteLabel = new();
    private readonly Label _extrasSummaryLabel = new();
    private readonly ListBox _chapterList = new();
    private readonly ComboBox _profileBox = new();
    private readonly ComboBox _languageBox = new();
    private readonly Label _statusLabel = new();
    private readonly Label _progressDetail = new();
    private readonly SlimProgressBar _progress = new();
    private readonly Button _buildButton = new();
    private readonly Button _cancelButton = new();
    private readonly Button _openButton = new();
    private readonly List<Panel> _widePanels = [];
    private readonly List<Control> _inputs = [];
    private readonly DataBuilderService _builder = new();
    private readonly ToolTip _toolTip = new();
    private ButtonFixBundle? _buttonFixBundle;
    private string? _buttonFixError;
    private BundledAsset? _choiceDataBundle;
    private string? _choiceDataError;
    private string? _selectedButtonFixPath;
    private readonly List<DataAddonSource> _dataAddons = [];
    private string? _validatedApkPath;
    private string? _validatedMainObbPath;
    private string? _validatedPatchObbPath;
    private CancellationTokenSource? _buildCancellation;
    private BuildResult? _lastResult;

    public MainForm()
    {
        Text = "MCSM Vita Data Builder  •  v1.3";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(900, 760);
        Size = new Size(1040, 950);
        BackColor = Page;
        ForeColor = TextMain;
        Font = new Font("Segoe UI", 9.5f);
        AllowDrop = true;
        DoubleBuffered = true;
        HandleCreated += (_, _) => WindowStyling.ApplyDarkTitleBar(this);
        DragEnter += OnDragEnter;
        DragDrop += OnDragDrop;

        try
        {
            _buttonFixBundle = DataBuilderService.InspectBundledButtonFix();
        }
        catch (Exception exception)
        {
            _buttonFixError = exception.Message;
        }
        try
        {
            _choiceDataBundle = DataBuilderService.InspectBundledChoiceData();
        }
        catch (Exception exception)
        {
            _choiceDataError = exception.Message;
        }

        ConfigurePage();
        BuildHeader();
        BuildSourceCard();
        BuildChapterCard();
        BuildOutputCard();
        BuildActionCard();

        string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        _outputBox.Text = Path.Combine(desktop, "MCSM Vita Data", "mcsm");
        _profileBox.SelectedIndex = 0;
        _languageBox.SelectedIndex = 0;
        UpdateBaseNote();
        UpdateOptionalSummary();
        UpdateReadyState();

        Resize += (_, _) => ResizeWidePanels();
        Shown += (_, _) =>
        {
            ResizeWidePanels();
            WindowStyling.ApplyDarkTitleBar(this);
        };
        FormClosing += OnFormClosing;
    }

    private void ConfigurePage()
    {
        _page.Dock = DockStyle.Fill;
        _page.FlowDirection = FlowDirection.TopDown;
        _page.WrapContents = false;
        _page.AutoScroll = true;
        _page.Padding = new Padding(30, 18, 30, 18);
        _page.BackColor = Page;
        Controls.Add(_page);
    }

    private void BuildHeader()
    {
        Panel header = new()
        {
            Height = 88,
            Margin = new Padding(0, 0, 0, 8),
            BackColor = Page
        };
        _widePanels.Add(header);

        Label badge = new()
        {
            AutoSize = true,
            Text = "  MCSM VITA BUILDER 1.3  ",
            Font = new Font("Segoe UI Semibold", 8.5f),
            ForeColor = Primary,
            BackColor = Color.FromArgb(18, 64, 54),
            Location = new Point(2, 2),
            Padding = new Padding(5, 4, 5, 4)
        };
        Label title = new()
        {
            AutoSize = true,
            Text = "Build Vita data",
            Font = new Font("Segoe UI Semibold", 22f),
            ForeColor = TextMain,
            Location = new Point(0, 28)
        };
        Label subtitle = new()
        {
            AutoSize = true,
            Text = "Choose the APK and two OBBs. The tool checks them and creates the complete mcsm folder.",
            Font = new Font("Segoe UI", 10f),
            ForeColor = TextSoft,
            Location = new Point(3, 68)
        };
        Label localOnly = new()
        {
            AutoSize = true,
            Text = "OFFLINE  •  NOTHING IS UPLOADED",
            Font = new Font("Segoe UI Semibold", 8f),
            ForeColor = Info,
            Location = new Point(626, 8),
            Anchor = AnchorStyles.Top | AnchorStyles.Right
        };
        header.SizeChanged += (_, _) => localOnly.Left = Math.Max(420, header.ClientSize.Width - localOnly.Width - 2);
        header.Controls.AddRange([badge, title, subtitle, localOnly]);
        _page.Controls.Add(header);
    }

    private void BuildSourceCard()
    {
        Panel card = CreateCard(
            "1",
            "Game files",
            "Only the exact PowerVR v1.37 game set is accepted. Mali and Adreno files are blocked.",
            236);

        Label apkLabel = CreateFieldLabel("POWERVR APK  ·  v1.37 ONLY", 24, 68);
        ConfigureInputState(_apkValidationLabel, 250, 66, "NOT SELECTED");
        ConfigurePathBox(_apkBox, 24, 84, "Choose or drop the PowerVR .apk");
        Button apkButton = CreateSecondaryButton("Choose APK", 0, 82, 116);
        apkButton.Click += async (_, _) => await BrowseApkAsync();

        Label obbLabel = CreateFieldLabel("MAIN OBB", 24, 112);
        ConfigureInputState(_mainObbValidationLabel, 250, 110, "NOT SELECTED");
        ConfigurePathBox(_obbBox, 24, 128, "Choose or drop the main .obb");
        Button obbButton = CreateSecondaryButton("Choose OBB", 0, 126, 116);
        obbButton.Click += async (_, _) => await BrowseMainObbAsync();

        Label patchObbLabel = CreateFieldLabel("PATCH OBB", 24, 156);
        ConfigureInputState(_patchObbValidationLabel, 250, 154, "NOT SELECTED");
        ConfigurePathBox(_patchObbBox, 24, 172, "Auto-detected beside the main OBB, or choose it");
        Button patchObbButton = CreateSecondaryButton("Choose OBB", 0, 170, 116);
        patchObbButton.Click += async (_, _) => await BrowsePatchObbAsync();

        _baseNoteLabel.AutoSize = false;
        _baseNoteLabel.Font = new Font("Segoe UI", 8.5f);
        _baseNoteLabel.ForeColor = TextSoft;
        _baseNoteLabel.Location = new Point(24, 208);
        _baseNoteLabel.Height = 20;

        void ResizeFields()
        {
            int buttonX = card.ClientSize.Width - 140;
            int boxWidth = Math.Max(300, buttonX - 42);
            _apkBox.Width = boxWidth;
            _obbBox.Width = boxWidth;
            _patchObbBox.Width = boxWidth;
            apkButton.Left = buttonX;
            obbButton.Left = buttonX;
            patchObbButton.Left = buttonX;
            int stateWidth = Math.Max(180, card.ClientSize.Width - 274);
            _apkValidationLabel.Width = stateWidth;
            _mainObbValidationLabel.Width = stateWidth;
            _patchObbValidationLabel.Width = stateWidth;
            _baseNoteLabel.Width = Math.Max(300, card.ClientSize.Width - 48);
        }
        card.SizeChanged += (_, _) => ResizeFields();

        _apkBox.TextChanged += (_, _) =>
        {
            if (!PathsMatch(_validatedApkPath, _apkBox.Text))
            {
                _validatedApkPath = null;
            }
            UpdateReadyState();
        };
        _obbBox.TextChanged += (_, _) =>
        {
            if (!PathsMatch(_validatedMainObbPath, _obbBox.Text))
            {
                _validatedMainObbPath = null;
            }
            UpdateBaseNote();
            UpdateReadyState();
        };
        _patchObbBox.TextChanged += (_, _) =>
        {
            if (!PathsMatch(_validatedPatchObbPath, _patchObbBox.Text))
            {
                _validatedPatchObbPath = null;
            }
            UpdateBaseNote();
            UpdateReadyState();
        };
        _inputs.AddRange([_apkBox, _obbBox, _patchObbBox, apkButton, obbButton, patchObbButton]);
        card.Controls.AddRange([
            apkLabel, _apkValidationLabel, _apkBox, apkButton,
            obbLabel, _mainObbValidationLabel, _obbBox, obbButton,
            patchObbLabel, _patchObbValidationLabel, _patchObbBox, patchObbButton,
            _baseNoteLabel
        ]);
        ResizeFields();
    }

    private void BuildChapterCard()
    {
        Panel card = CreateCard(
            "2",
            "Optional content",
            "Skip this for Episode 1. Add Episodes 2–8 only if you own their Android files.",
            176);

        _chapterList.Location = new Point(24, 76);
        _chapterList.Height = 68;
        _chapterList.BackColor = Field;
        _chapterList.ForeColor = TextMain;
        _chapterList.BorderStyle = BorderStyle.FixedSingle;
        _chapterList.Font = new Font("Segoe UI", 9.5f);
        _chapterList.IntegralHeight = false;

        Button addFolder = CreateSecondaryButton("Episode folder", 0, 76, 116);
        Button addZip = CreateSecondaryButton("Episode ZIP", 0, 112, 116);
        Button remove = CreateGhostButton("Remove", 24, 146, 76);
        addFolder.Click += async (_, _) => await BrowseChapterFolderAsync();
        addZip.Click += async (_, _) => await BrowseChapterZipAsync();
        remove.Click += (_, _) =>
        {
            if (_chapterList.SelectedItem is not null)
            {
                _chapterList.Items.Remove(_chapterList.SelectedItem);
                UpdateOptionalSummary();
                UpdateReadyState();
            }
        };

        _extrasSummaryLabel.AutoSize = false;
        _extrasSummaryLabel.Text = "Episode 1 only";
        _extrasSummaryLabel.ForeColor = TextSoft;
        _extrasSummaryLabel.Font = new Font("Segoe UI", 8.5f);
        _extrasSummaryLabel.Location = new Point(108, 150);
        _extrasSummaryLabel.Height = 18;

        Button extras = CreateGhostButton("Fixes / mods", 0, 146, 116);
        extras.ForeColor = Warning;
        extras.Click += (_, _) => OpenExtrasDialog();

        void ResizeFields()
        {
            int buttonX = card.ClientSize.Width - 140;
            _chapterList.Width = Math.Max(300, buttonX - 42);
            addFolder.Left = buttonX;
            addZip.Left = buttonX;
            extras.Left = buttonX;
            _extrasSummaryLabel.Width = Math.Max(180, buttonX - 126);
        }
        card.SizeChanged += (_, _) => ResizeFields();

        _inputs.AddRange([_chapterList, addFolder, addZip, remove, extras]);
        card.Controls.AddRange([_chapterList, addFolder, addZip, remove, extras, _extrasSummaryLabel]);
        ResizeFields();
    }

    private void BuildOutputCard()
    {
        Panel card = CreateCard(
            "3",
            "Output and defaults",
            "The finished folder is always named mcsm. Copy it to ux0:data\\ on your Vita.",
            190);

        Label outputLabel = CreateFieldLabel("OUTPUT FOLDER", 24, 77);
        ConfigurePathBox(_outputBox, 24, 99, "Choose where the finished mcsm folder should be created");
        Button outputButton = CreateSecondaryButton("Choose folder", 0, 97, 116);
        outputButton.Click += (_, _) => BrowseOutput();

        Label profileLabel = CreateFieldLabel("STARTING PROFILE", 24, 138);
        ConfigureCombo(_profileBox, 24, 158, 250);
        _profileBox.Items.AddRange([
            new Choice("Performance — recommended", "performance"),
            new Choice("Balanced", "balanced"),
            new Choice("Quality", "quality"),
            new Choice("Battery", "battery")
        ]);

        Label languageLabel = CreateFieldLabel("TEXT LANGUAGE", 302, 138);
        ConfigureCombo(_languageBox, 302, 158, 180);
        _languageBox.Items.AddRange([
            new Choice("English", "en"),
            new Choice("French", "fr"),
            new Choice("German", "de"),
            new Choice("Spanish", "es"),
            new Choice("Portuguese", "pt"),
            new Choice("Russian", "ru"),
            new Choice("Chinese", "zh")
        ]);

        void ResizeFields()
        {
            int buttonX = card.ClientSize.Width - 140;
            _outputBox.Width = Math.Max(300, buttonX - 42);
            outputButton.Left = buttonX;
        }
        card.SizeChanged += (_, _) => ResizeFields();

        _outputBox.TextChanged += (_, _) => UpdateReadyState();
        _inputs.AddRange([_outputBox, outputButton, _profileBox, _languageBox]);
        card.Controls.AddRange([
            outputLabel, _outputBox, outputButton,
            profileLabel, _profileBox, languageLabel, _languageBox
        ]);
        ResizeFields();
    }

    private void BuildActionCard()
    {
        Panel card = CreateCard("✓", "Build", "Your selected APK and OBB files are never modified.", 146);

        _statusLabel.AutoSize = false;
        _statusLabel.Location = new Point(24, 74);
        _statusLabel.Size = new Size(480, 26);
        _statusLabel.Font = new Font("Segoe UI Semibold", 10f);
        _statusLabel.ForeColor = TextSoft;

        _progress.Location = new Point(24, 104);
        _progress.Height = 6;
        _progress.Value = 0;

        _progressDetail.AutoSize = false;
        _progressDetail.Location = new Point(24, 115);
        _progressDetail.Size = new Size(500, 22);
        _progressDetail.Font = new Font("Segoe UI", 8.5f);
        _progressDetail.ForeColor = TextSoft;

        ConfigurePrimaryButton(_buildButton, "BUILD DATA FOLDER", 0, 78, 210);
        _buildButton.Click += async (_, _) => await BuildDataAsync();
        ConfigureGhostButton(_cancelButton, "Cancel", 0, 116, 98);
        _cancelButton.Enabled = false;
        _cancelButton.Visible = false;
        _cancelButton.Click += (_, _) => _buildCancellation?.Cancel();
        ConfigureGhostButton(_openButton, "Open result", 0, 116, 104);
        _openButton.Enabled = false;
        _openButton.Visible = false;
        _openButton.Click += (_, _) => OpenLastResult();

        void ResizeFields()
        {
            int actionX = card.ClientSize.Width - 234;
            int leftWidth = Math.Max(360, actionX - 42);
            _statusLabel.Width = leftWidth;
            _progress.Width = leftWidth;
            _progressDetail.Width = leftWidth;
            _buildButton.Left = actionX;
            _cancelButton.Left = actionX;
            _openButton.Left = actionX + 106;
        }
        card.SizeChanged += (_, _) => ResizeFields();

        card.Controls.AddRange([
            _statusLabel, _progress, _progressDetail,
            _buildButton, _cancelButton, _openButton
        ]);
        ResizeFields();
    }

    private Panel CreateCard(string number, string title, string subtitle, int height)
    {
        Panel card = new()
        {
            Height = height,
            BackColor = Card,
            Margin = new Padding(0, 0, 0, 8),
            Padding = new Padding(1)
        };
        card.Paint += (_, e) =>
        {
            using Pen pen = new(Border, 1);
            e.Graphics.DrawRectangle(pen, 0, 0, card.ClientSize.Width - 1, card.ClientSize.Height - 1);
        };

        Label badge = new()
        {
            Text = number,
            TextAlign = ContentAlignment.MiddleCenter,
            Font = new Font("Segoe UI Semibold", 12f),
            ForeColor = Primary,
            BackColor = PrimaryDark,
            Location = new Point(24, 21),
            Size = new Size(36, 36)
        };
        Label heading = new()
        {
            AutoSize = true,
            Text = title,
            Font = new Font("Segoe UI Semibold", 14f),
            ForeColor = TextMain,
            Location = new Point(72, 17)
        };
        Label description = new()
        {
            AutoSize = false,
            Text = subtitle,
            Font = new Font("Segoe UI", 9f),
            ForeColor = TextSoft,
            Location = new Point(74, 45),
            Height = 28,
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
        card.SizeChanged += (_, _) => description.Width = Math.Max(100, card.ClientSize.Width - 98);
        card.Controls.AddRange([badge, heading, description]);
        _widePanels.Add(card);
        _page.Controls.Add(card);
        return card;
    }

    private static Label CreateFieldLabel(string text, int x, int y) => new()
    {
        AutoSize = true,
        Text = text,
        Font = new Font("Segoe UI Semibold", 8.2f),
        ForeColor = TextSoft,
        Location = new Point(x, y)
    };

    private static void ConfigurePathBox(TextBox box, int x, int y, string placeholder = "")
    {
        box.Location = new Point(x, y);
        box.Height = 28;
        box.BackColor = Field;
        box.ForeColor = TextMain;
        box.BorderStyle = BorderStyle.FixedSingle;
        box.Font = new Font("Segoe UI", 9f);
        box.ReadOnly = true;
        box.PlaceholderText = placeholder;
        box.Cursor = Cursors.Hand;
    }

    private static void ConfigureInputState(Label label, int x, int y, string text)
    {
        label.AutoSize = false;
        label.Location = new Point(x, y);
        label.Height = 18;
        label.TextAlign = ContentAlignment.MiddleRight;
        label.Font = new Font("Segoe UI Semibold", 8f);
        label.ForeColor = TextSoft;
        label.Text = text;
    }

    private static void ConfigureCombo(ComboBox box, int x, int y, int width)
    {
        box.Location = new Point(x, y);
        box.Width = width;
        box.DropDownStyle = ComboBoxStyle.DropDownList;
        box.FlatStyle = FlatStyle.Flat;
        box.BackColor = Field;
        box.ForeColor = TextMain;
        box.Font = new Font("Segoe UI", 9f);
        box.DrawMode = DrawMode.OwnerDrawFixed;
        box.ItemHeight = 24;
        box.DrawItem += (_, e) =>
        {
            Color background = (e.State & DrawItemState.Selected) != 0
                ? Color.FromArgb(30, 64, 78)
                : Field;
            using SolidBrush backgroundBrush = new(background);
            e.Graphics.FillRectangle(backgroundBrush, e.Bounds);
            if (e.Index >= 0)
            {
                using SolidBrush textBrush = new(TextMain);
                e.Graphics.DrawString(
                    box.Items[e.Index]?.ToString() ?? string.Empty,
                    box.Font,
                    textBrush,
                    e.Bounds.Left + 5,
                    e.Bounds.Top + 3);
            }
            e.DrawFocusRectangle();
        };
    }

    private static Button CreateSecondaryButton(string text, int x, int y, int width)
    {
        Button button = new();
        ConfigureSecondaryButton(button, text, x, y, width);
        return button;
    }

    private static Button CreateGhostButton(string text, int x, int y, int width)
    {
        Button button = new();
        ConfigureGhostButton(button, text, x, y, width);
        return button;
    }

    private static void ConfigureSecondaryButton(Button button, string text, int x, int y, int width)
    {
        button.Text = text;
        button.Location = new Point(x, y);
        button.Size = new Size(width, 31);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderColor = Border;
        button.FlatAppearance.MouseOverBackColor = Color.FromArgb(42, 55, 78);
        button.BackColor = Color.FromArgb(30, 41, 59);
        button.ForeColor = TextMain;
        button.Font = new Font("Segoe UI Semibold", 9f);
        button.Cursor = Cursors.Hand;
    }

    private static void ConfigurePrimaryButton(Button button, string text, int x, int y, int width)
    {
        button.Text = text;
        button.Location = new Point(x, y);
        button.Size = new Size(width, 34);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = 0;
        button.FlatAppearance.MouseOverBackColor = Color.FromArgb(110, 231, 183);
        button.BackColor = Primary;
        button.ForeColor = Color.FromArgb(5, 46, 22);
        button.Font = new Font("Segoe UI Semibold", 9.5f);
        button.Cursor = Cursors.Hand;
    }

    private static void ConfigureGhostButton(Button button, string text, int x, int y, int width)
    {
        button.Text = text;
        button.Location = new Point(x, y);
        button.Size = new Size(width, 28);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = 0;
        button.FlatAppearance.MouseOverBackColor = Color.FromArgb(42, 55, 78);
        button.BackColor = Card;
        button.ForeColor = TextSoft;
        button.Font = new Font("Segoe UI Semibold", 8.5f);
        button.Cursor = Cursors.Hand;
    }

    private void ResizeWidePanels()
    {
        int width = Math.Max(780, _page.ClientSize.Width - _page.Padding.Horizontal - 18);
        foreach (Panel panel in _widePanels)
        {
            panel.Width = width;
        }
    }

    private async Task BrowseApkAsync()
    {
        using OpenFileDialog dialog = new()
        {
            Title = "Choose your Minecraft: Story Mode APK",
            Filter = "Android APK (*.apk)|*.apk",
            DefaultExt = "apk",
            AddExtension = false,
            CheckFileExists = true,
            RestoreDirectory = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            await SelectApkAsync(dialog.FileName);
        }
    }

    private async Task BrowseMainObbAsync()
    {
        using OpenFileDialog dialog = new()
        {
            Title = "Choose the main Minecraft: Story Mode OBB",
            Filter = "Android OBB (*.obb)|*.obb",
            DefaultExt = "obb",
            AddExtension = false,
            CheckFileExists = true,
            RestoreDirectory = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            await SelectMainObbAsync(dialog.FileName);
        }
    }

    private async Task BrowsePatchObbAsync()
    {
        using OpenFileDialog dialog = new()
        {
            Title = "Choose the patch Minecraft: Story Mode OBB",
            Filter = "Android OBB (*.obb)|*.obb",
            DefaultExt = "obb",
            AddExtension = false,
            CheckFileExists = true,
            RestoreDirectory = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            await SelectPatchObbAsync(dialog.FileName);
        }
    }

    private async Task SelectApkAsync(string path)
    {
        string fullPath = Path.GetFullPath(path);
        _validatedApkPath = null;
        _apkBox.Text = fullPath;
        SetInputState(_apkValidationLabel, "Checking version…", Info, "…");
        UpdateReadyState();

        try
        {
            ApkLayout layout = await Task.Run(() => DataBuilderService.InspectApk(fullPath));
            if (!PathsMatch(fullPath, _apkBox.Text))
            {
                return;
            }
            _validatedApkPath = fullPath;
            SetInputState(
                _apkValidationLabel,
                $"PowerVR verified  •  v{layout.VersionName} ({layout.VersionCode})",
                Primary,
                "✓");
            _toolTip.SetToolTip(
                _apkValidationLabel,
                $"Exact supported PowerVR fingerprint verified.\n{layout.PackageName}\n" +
                $"{layout.LibraryCount} required ARMv7 libraries and {layout.AssetCount} APK assets found.");
        }
        catch (Exception exception)
        {
            SetInputState(_apkValidationLabel, "Not accepted", Warning, "!");
            MessageBox.Show(this, exception.Message, "APK not accepted", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        UpdateReadyState();
    }

    private async Task SelectMainObbAsync(string path)
    {
        string fullPath = Path.GetFullPath(path);
        _validatedMainObbPath = null;
        _obbBox.Text = fullPath;
        SetInputState(_mainObbValidationLabel, "Checking…", Info, "…");
        UpdateReadyState();

        try
        {
            ObbLayout layout = await Task.Run(() => DataBuilderService.InspectMainObb(fullPath));
            if (!PathsMatch(fullPath, _obbBox.Text))
            {
                return;
            }
            _validatedMainObbPath = fullPath;
            SetInputState(
                _mainObbValidationLabel,
                $"PowerVR verified  •  {FormatBytes(layout.TotalBytes)}",
                Primary,
                "✓");

            if (PathsMatch(fullPath, _patchObbBox.Text))
            {
                _validatedPatchObbPath = null;
                SetInputState(_patchObbValidationLabel, "Choose a different file", Warning, "!");
            }

            if (layout.PatchPath is not null && !PathsMatch(layout.PatchPath, _patchObbBox.Text))
            {
                await SelectPatchObbAsync(layout.PatchPath, detectedAutomatically: true);
            }
            else if (layout.PatchPath is null && string.IsNullOrWhiteSpace(_patchObbBox.Text))
            {
                SetInputState(_patchObbValidationLabel, "Not found beside main OBB", Warning, "!");
            }
        }
        catch (Exception exception)
        {
            SetInputState(_mainObbValidationLabel, "Not accepted", Warning, "!");
            MessageBox.Show(this, exception.Message, "Main OBB not accepted", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        UpdateBaseNote();
        UpdateReadyState();
    }

    private async Task SelectPatchObbAsync(string path, bool detectedAutomatically = false)
    {
        string fullPath = Path.GetFullPath(path);
        _validatedPatchObbPath = null;
        _patchObbBox.Text = fullPath;
        SetInputState(_patchObbValidationLabel, "Checking…", Info, "…");
        UpdateReadyState();

        try
        {
            if (PathsMatch(fullPath, _obbBox.Text))
            {
                throw new InvalidDataException("Main OBB and patch OBB must be two different files.");
            }
            ObbLayout layout = await Task.Run(() => DataBuilderService.InspectPatchObb(fullPath));
            if (!PathsMatch(fullPath, _patchObbBox.Text))
            {
                return;
            }
            _validatedPatchObbPath = fullPath;
            string source = detectedAutomatically ? "Auto-detected + verified" : "PowerVR verified";
            SetInputState(
                _patchObbValidationLabel,
                $"{source}  •  {FormatBytes(layout.TotalBytes)}",
                Primary,
                "✓");
        }
        catch (Exception exception)
        {
            SetInputState(_patchObbValidationLabel, "Not accepted", Warning, "!");
            if (!detectedAutomatically)
            {
                MessageBox.Show(this, exception.Message, "Patch OBB not accepted", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }
        UpdateBaseNote();
        UpdateReadyState();
    }

    private async Task BrowseChapterFolderAsync()
    {
        using FolderBrowserDialog dialog = new()
        {
            Description = "Choose an episode folder or its com.telltalegames.minecraft100/files/Net folder",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = false
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            await AddChapterSourceAsync(dialog.SelectedPath);
        }
    }

    private async Task BrowseChapterZipAsync()
    {
        using OpenFileDialog dialog = new()
        {
            Title = "Choose one or more chapter ZIPs",
            Filter = "Chapter ZIP (*.zip)|*.zip",
            DefaultExt = "zip",
            AddExtension = false,
            CheckFileExists = true,
            RestoreDirectory = true,
            Multiselect = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            foreach (string path in dialog.FileNames)
            {
                await AddChapterSourceAsync(path);
            }
        }
    }

    private void BrowseOutput()
    {
        using FolderBrowserDialog dialog = new()
        {
            Description = "Choose where the ready mcsm folder should be created",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            string selected = dialog.SelectedPath.TrimEnd(Path.DirectorySeparatorChar);
            _outputBox.Text = Path.GetFileName(selected).Equals("mcsm", StringComparison.OrdinalIgnoreCase)
                ? selected
                : Path.Combine(selected, "mcsm");
        }
    }

    private async Task AddChapterSourceAsync(string path)
    {
        if (_chapterList.Items.Cast<ChapterSource>()
            .Any(source => source.Path.Equals(Path.GetFullPath(path), StringComparison.OrdinalIgnoreCase)))
        {
            SetStatus("That chapter source is already in the list.", Warning);
            return;
        }

        try
        {
            SetStatus("Scanning chapter files…", Info);
            ChapterSource source = await Task.Run(() => ChapterScanner.Inspect(path));
            _chapterList.Items.Add(source);
            SetStatus($"Added {source.DisplayName}", Primary);
            UpdateOptionalSummary();
            UpdateReadyState();
        }
        catch (Exception exception)
        {
            SetStatus("That chapter source could not be used.", Warning);
            MessageBox.Show(this, exception.Message, "Chapter not added", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void OpenExtrasDialog()
    {
        using ExtrasDialog dialog = new(
            _buttonFixBundle,
            _buttonFixError,
            _selectedButtonFixPath,
            _dataAddons);
        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return;
        }

        _selectedButtonFixPath = dialog.ButtonFixPath;
        _dataAddons.Clear();
        _dataAddons.AddRange(dialog.DataAddons);
        UpdateOptionalSummary();
        UpdateBaseNote();
    }

    private void UpdateOptionalSummary()
    {
        string episodes = _chapterList.Items.Count == 0
            ? "Episode 1 only"
            : $"{_chapterList.Items.Count} episode source(s)";
        string buttonFix = _selectedButtonFixPath is not null
            ? "custom button fix"
            : _buttonFixBundle is not null
                ? $"button fix built in ({_buttonFixBundle.FileCount})"
                : "button fix not supplied";
        string mods = _dataAddons.Count == 0
            ? "no mods"
            : $"{_dataAddons.Count} mod/data add-on(s) — UNTESTED";
        _extrasSummaryLabel.Text = $"{episodes}  ·  {buttonFix}  ·  {mods}";
        _extrasSummaryLabel.ForeColor = _dataAddons.Count > 0 ? Warning : TextSoft;
    }

    private async Task BuildDataAsync()
    {
        if (_buildCancellation is not null)
        {
            return;
        }
        if (!InputsLookReady())
        {
            UpdateReadyState();
            return;
        }

        string output = _outputBox.Text.Trim();
        if (_dataAddons.Count > 0)
        {
            DialogResult modAnswer = MessageBox.Show(
                this,
                "Data add-on / mod installation is experimental and has not been tested on Vita. " +
                "Selected add-ons are applied last and may replace files in the prepared data folder.\n\nContinue?",
                "Experimental mod installation",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning,
                MessageBoxDefaultButton.Button2);
            if (modAnswer != DialogResult.Yes)
            {
                return;
            }
        }

        if (Directory.Exists(output) && Directory.EnumerateFileSystemEntries(output).Any())
        {
            DialogResult answer = MessageBox.Show(
                this,
                "A data folder already exists there. It will be preserved as a timestamped backup before the new folder is installed.\n\nContinue?",
                "Existing mcsm folder",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Question,
                MessageBoxDefaultButton.Button2);
            if (answer != DialogResult.Yes)
            {
                return;
            }
        }

        try
        {
            _lastResult = null;
            _openButton.Enabled = false;
            _buildCancellation = new CancellationTokenSource();
            SetBusy(true);

            var chapters = _chapterList.Items.Cast<ChapterSource>().ToList();
            var profile = (Choice?)_profileBox.SelectedItem ?? throw new InvalidDataException("Choose a profile.");
            var language = (Choice?)_languageBox.SelectedItem ?? throw new InvalidDataException("Choose a language.");
            var request = new BuildRequest(
                _apkBox.Text.Trim(),
                _obbBox.Text.Trim(),
                _patchObbBox.Text.Trim(),
                output,
                chapters,
                profile.Value,
                language.Value,
                _selectedButtonFixPath,
                _dataAddons.ToList());

            var progress = new Progress<BuildProgress>(update =>
            {
                _progress.Value = update.Percent;
                _statusLabel.Text = update.Status;
                _statusLabel.ForeColor = Info;
                _progressDetail.Text = $"{FormatBytes(update.BytesCopied)} of {FormatBytes(update.TotalBytes)}";
            });

            _lastResult = await _builder.BuildAsync(request, progress, _buildCancellation.Token);
            _progress.Value = 100;
            SetStatus("Ready — copy the whole mcsm folder to ux0:data\\", Primary);
            _progressDetail.Text =
                $"Episodes {string.Join(", ", _lastResult.IncludedEpisodes)} · " +
                $"controller fix {_lastResult.ButtonFixFileCount} · " +
                $"choice stats {(_lastResult.ChoiceDataIncluded ? "ready" : "not included")} · " +
                $"mods {_lastResult.DataAddonFileCount} files · {FormatBytes(_lastResult.TotalBytes)}";
            _openButton.Enabled = true;

            string backup = _lastResult.BackupDirectory is null
                ? string.Empty
                : $"\n\nYour previous folder was preserved at:\n{_lastResult.BackupDirectory}";
            MessageBox.Show(
                this,
                $"Your Vita data folder is ready.\n\nCopy this folder:\n{_lastResult.OutputDirectory}\n\nTo:\nux0:data\\{backup}",
                "Build complete",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
        }
        catch (OperationCanceledException)
        {
            _progress.Value = 0;
            _progressDetail.Text = string.Empty;
            SetStatus("Build cancelled. No finished folder was replaced.", Warning);
        }
        catch (Exception exception)
        {
            _progress.Value = 0;
            _progressDetail.Text = string.Empty;
            SetStatus("Build stopped — nothing was installed on the Vita.", Warning);
            MessageBox.Show(this, exception.Message, "Could not build data folder", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            _buildCancellation?.Dispose();
            _buildCancellation = null;
            SetBusy(false);
        }
    }

    private void SetBusy(bool busy)
    {
        foreach (Control input in _inputs)
        {
            input.Enabled = !busy;
        }
        SetBuildButtonState(!busy && InputsLookReady(), busy);
        _cancelButton.Enabled = busy;
        _cancelButton.Visible = busy;
        _openButton.Enabled = !busy && _lastResult is not null;
        _openButton.Visible = !busy && _lastResult is not null;
        UseWaitCursor = false;
    }

    private void UpdateReadyState()
    {
        if (_buildCancellation is not null)
        {
            return;
        }
        bool ready = InputsLookReady();
        SetBuildButtonState(ready, busy: false);
        if (ready)
        {
            SetStatus("Exact PowerVR base set verified. Ready to build.", Primary);
        }
        else if (!PathsMatch(_validatedApkPath, _apkBox.Text))
        {
            SetStatus("Start with the required 32-bit PowerVR .apk.", TextSoft);
        }
        else if (!PathsMatch(_validatedMainObbPath, _obbBox.Text))
        {
            SetStatus("PowerVR APK verified — now add the main .obb.", TextSoft);
        }
        else if (!PathsMatch(_validatedPatchObbPath, _patchObbBox.Text))
        {
            SetStatus("Main data ready — add the separate patch .obb.", TextSoft);
        }
        else
        {
            SetStatus("Choose where the finished mcsm folder should go.", TextSoft);
        }
    }

    private bool InputsLookReady() =>
        PathsMatch(_validatedApkPath, _apkBox.Text)
        && PathsMatch(_validatedMainObbPath, _obbBox.Text)
        && PathsMatch(_validatedPatchObbPath, _patchObbBox.Text)
        && !string.IsNullOrWhiteSpace(_outputBox.Text);

    private void SetBuildButtonState(bool ready, bool busy)
    {
        _buildButton.Enabled = true;
        _buildButton.Visible = !busy;
        _buildButton.TabStop = ready;
        _buildButton.Text = busy
            ? "BUILDING…"
            : ready
                ? "BUILD DATA FOLDER"
                : "ADD REQUIRED FILES";
        _buildButton.BackColor = ready ? Primary : Color.FromArgb(30, 41, 59);
        _buildButton.ForeColor = ready ? Color.FromArgb(5, 46, 22) : TextSoft;
        _buildButton.Cursor = ready ? Cursors.Hand : Cursors.Default;
        _buildButton.AccessibleDescription = ready
            ? "Build the validated Vita data folder"
            : "Add the required APK, main OBB, and patch OBB first";
    }

    private void UpdateBaseNote()
    {
        string? activeFixError = _selectedButtonFixPath is null ? _buttonFixError : null;
        bool hasButtonFix = _selectedButtonFixPath is not null || _buttonFixBundle is not null;
        bool hasChoiceData = _choiceDataBundle is not null;
        string controllerText = _selectedButtonFixPath is not null
            ? "custom controller prompts"
            : hasButtonFix
                ? "controller prompts"
                : "no controller fix";
        string choiceText = hasChoiceData ? "offline choice stats" : "no offline choice stats";
        string obbText = PathsMatch(_validatedPatchObbPath, _patchObbBox.Text)
            ? "Both OBBs ready"
            : "Main + patch OBB required";
        _baseNoteLabel.Text = $"{obbText}  ·  Built in: {controllerText} + {choiceText}";
        string? supportError = activeFixError ?? _choiceDataError;
        _baseNoteLabel.ForeColor = supportError is not null
            ? Warning
            : hasButtonFix && hasChoiceData && PathsMatch(_validatedPatchObbPath, _patchObbBox.Text)
                ? Primary
                : TextSoft;
        _toolTip.SetToolTip(
            _baseNoteLabel,
            supportError ?? "Controller prompts and offline crowd-choice data are installed automatically.");
    }

    private static void SetInputState(Label label, string text, Color color, string icon)
    {
        label.Text = $"{icon}  {text}";
        label.ForeColor = color;
    }

    private static bool PathsMatch(string? expected, string? actual)
    {
        if (string.IsNullOrWhiteSpace(expected) || string.IsNullOrWhiteSpace(actual))
        {
            return false;
        }
        try
        {
            return Path.GetFullPath(expected).Equals(Path.GetFullPath(actual.Trim()), StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private void SetStatus(string text, Color color)
    {
        _statusLabel.Text = text;
        _statusLabel.ForeColor = color;
    }

    private void OpenLastResult()
    {
        if (_lastResult is null || !Directory.Exists(_lastResult.OutputDirectory))
        {
            return;
        }
        Process.Start(new ProcessStartInfo
        {
            FileName = _lastResult.OutputDirectory,
            UseShellExecute = true
        });
    }

    private void OnDragEnter(object? sender, DragEventArgs e)
    {
        if (e.Data?.GetData(DataFormats.FileDrop) is string[] paths
            && paths.Any(IsSupportedDrop))
        {
            e.Effect = DragDropEffects.Copy;
        }
        else
        {
            e.Effect = DragDropEffects.None;
        }
    }

    private async void OnDragDrop(object? sender, DragEventArgs e)
    {
        if (e.Data?.GetData(DataFormats.FileDrop) is not string[] paths)
        {
            return;
        }

        int ignored = 0;
        foreach (string path in paths.OrderBy(path =>
                     Path.GetFileName(path).StartsWith("patch.", StringComparison.OrdinalIgnoreCase) ? 1 : 0))
        {
            string extension = Path.GetExtension(path);
            if (File.Exists(path) && extension.Equals(".apk", StringComparison.OrdinalIgnoreCase))
            {
                await SelectApkAsync(path);
            }
            else if (File.Exists(path) && extension.Equals(".obb", StringComparison.OrdinalIgnoreCase))
            {
                bool looksLikePatch = Path.GetFileName(path).StartsWith("patch.", StringComparison.OrdinalIgnoreCase);
                if (looksLikePatch || PathsMatch(_validatedMainObbPath, _obbBox.Text))
                {
                    await SelectPatchObbAsync(path);
                }
                else
                {
                    await SelectMainObbAsync(path);
                }
            }
            else if (Directory.Exists(path)
                     || (File.Exists(path) && extension.Equals(".zip", StringComparison.OrdinalIgnoreCase)))
            {
                await AddChapterSourceAsync(path);
            }
            else
            {
                ignored++;
            }
        }

        if (ignored > 0)
        {
            SetStatus(
                $"Ignored {ignored} unsupported file(s). Base game accepts only .apk and .obb.",
                Warning);
        }
    }

    private static bool IsSupportedDrop(string path)
    {
        if (Directory.Exists(path))
        {
            return true;
        }
        if (!File.Exists(path))
        {
            return false;
        }
        string extension = Path.GetExtension(path);
        return extension.Equals(".apk", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".obb", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".zip", StringComparison.OrdinalIgnoreCase);
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        if (_buildCancellation is null)
        {
            return;
        }

        DialogResult result = MessageBox.Show(
            this,
            "A build is still running. Cancel it and close?",
            "Build in progress",
            MessageBoxButtons.YesNo,
            MessageBoxIcon.Warning,
            MessageBoxDefaultButton.Button2);
        if (result != DialogResult.Yes)
        {
            e.Cancel = true;
            return;
        }
        _buildCancellation.Cancel();
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB", "TB"];
        double value = bytes;
        int unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return $"{value:0.#} {units[unit]}";
    }

    private sealed record Choice(string Label, string Value)
    {
        public override string ToString() => Label;
    }
}

internal sealed class SlimProgressBar : Control
{
    private int _value;

    public int Value
    {
        get => _value;
        set
        {
            _value = Math.Clamp(value, 0, 100);
            Invalidate();
        }
    }

    public SlimProgressBar()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        BackColor = Color.FromArgb(30, 41, 59);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using SolidBrush background = new(BackColor);
        using SolidBrush fill = new(Color.FromArgb(52, 211, 153));
        e.Graphics.FillRectangle(background, ClientRectangle);
        if (_value > 0)
        {
            int width = Math.Max(1, ClientSize.Width * _value / 100);
            e.Graphics.FillRectangle(fill, new Rectangle(0, 0, width, ClientSize.Height));
        }
    }
}
