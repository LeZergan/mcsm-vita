namespace McsmVitaDataBuilder;

public sealed class CustomProfileDialog : Form
{
    private static readonly Color Page = Color.FromArgb(10, 15, 28);
    private static readonly Color Card = Color.FromArgb(22, 31, 50);
    private static readonly Color Field = Color.FromArgb(11, 18, 32);
    private static readonly Color Border = Color.FromArgb(50, 64, 86);
    private static readonly Color Primary = Color.FromArgb(52, 211, 153);
    private static readonly Color PrimaryDark = Color.FromArgb(6, 95, 70);
    private static readonly Color Info = Color.FromArgb(96, 165, 250);
    private static readonly Color Warning = Color.FromArgb(251, 191, 36);
    private static readonly Color TextMain = Color.FromArgb(241, 245, 249);
    private static readonly Color TextSoft = Color.FromArgb(148, 163, 184);

    private readonly Panel _easyPanel = new();
    private readonly Panel _advancedPanel = new();
    private readonly Button _easyButton = new();
    private readonly Button _advancedButton = new();
    private readonly Label _summary = new();
    private readonly ToolTip _toolTip = new();

    private readonly ComboBox _picture = new();
    private readonly ComboBox _motion = new();
    private readonly ComboBox _gpu = new();
    private readonly ComboBox _effects = new();
    private readonly ComboBox _world = new();
    private readonly ComboBox _power = new();

    private readonly ComboBox _resolution = new();
    private readonly ComboBox _fps = new();
    private readonly ComboBox _advancedGpu = new();
    private readonly ComboBox _outlines = new();
    private readonly ComboBox _shadows = new();
    private readonly NumericUpDown _detail = new();
    private readonly ComboBox _distance = new();
    private readonly ComboBox _clock = new();
    private readonly ComboBox _upscale = new();
    private readonly ComboBox _vsync = new();
    private readonly ComboBox _nearest = new();
    private readonly ComboBox _fbfetch = new();

    private string _mode = "easy";
    private bool _loadingSettings;

    public CustomProfileSettings Settings { get; private set; }

    public CustomProfileDialog(CustomProfileSettings settings)
    {
        Settings = settings;
        Text = "Custom graphics profile";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ClientSize = new Size(800, 670);
        BackColor = Page;
        ForeColor = TextMain;
        Font = new Font("Segoe UI", 9.5f);
        DoubleBuffered = true;
        HandleCreated += (_, _) => WindowStyling.ApplyDarkTitleBar(this);
        Shown += (_, _) => WindowStyling.ApplyDarkTitleBar(this);

        BuildUi();
        LoadSettings(settings);
    }

    private void BuildUi()
    {
        Label title = new()
        {
            AutoSize = true,
            Text = "Make your custom profile",
            Font = new Font("Segoe UI Semibold", 22f),
            ForeColor = TextMain,
            Location = new Point(28, 20)
        };
        Label subtitle = new()
        {
            AutoSize = true,
            Text = "Easy mode uses clear choices. Advanced mode exposes every supported exact value.",
            ForeColor = TextSoft,
            Location = new Point(31, 62)
        };

        Panel modeCard = CreateCard(new Rectangle(28, 92, 744, 58));
        Label modeLabel = new()
        {
            AutoSize = true,
            Text = "SETUP MODE",
            Font = new Font("Segoe UI Semibold", 8.5f),
            ForeColor = TextSoft,
            Location = new Point(18, 20)
        };
        ConfigureModeButton(_easyButton, "EASY — RECOMMENDED", 136);
        ConfigureModeButton(_advancedButton, "ADVANCED", 346);
        _easyButton.Click += (_, _) => SetMode("easy");
        _advancedButton.Click += (_, _) => SetMode("advanced");
        modeCard.Controls.AddRange([modeLabel, _easyButton, _advancedButton]);

        ConfigureContentPanel(_easyPanel);
        ConfigureContentPanel(_advancedPanel);
        BuildEasyPanel();
        BuildAdvancedPanel();

        _summary.AutoSize = false;
        _summary.Location = new Point(30, 574);
        _summary.Size = new Size(740, 26);
        _summary.Font = new Font("Segoe UI Semibold", 9.5f);
        _summary.ForeColor = Primary;

        Label warning = new()
        {
            AutoSize = false,
            Text = "60 FPS is only a cap. Busy scenes with many characters can still fall to around 20 FPS.",
            Location = new Point(30, 600),
            Size = new Size(740, 24),
            ForeColor = Warning,
            Font = new Font("Segoe UI", 8.8f)
        };

        Button reset = CreateGhostButton("Reset", 30, 630, 90);
        Button cancel = CreateGhostButton("Cancel", 508, 630, 96);
        Button save = CreatePrimaryButton("USE CUSTOM PROFILE", 612, 626, 160);
        save.Size = new Size(160, 34);
        save.Font = new Font("Segoe UI Semibold", 8.2f);
        reset.Click += (_, _) => LoadSettings(new CustomProfileSettings());
        cancel.DialogResult = DialogResult.Cancel;
        save.Click += (_, _) =>
        {
            Settings = ReadSettings();
            DialogResult = DialogResult.OK;
            Close();
        };
        CancelButton = cancel;
        AcceptButton = save;

        Controls.AddRange([
            title, subtitle, modeCard, _easyPanel, _advancedPanel,
            _summary, warning, reset, cancel, save
        ]);
    }

    private void BuildEasyPanel()
    {
        AddEasyField(
            _easyPanel,
            _picture,
            "Picture sharpness",
            "Controls internal resolution.",
            20,
            18,
            [
                new("Sharp — recommended (720×408)", "sharp"),
                new("Fast (640×362)", "fast"),
                new("Quality (800×452)", "quality"),
                new("Native Vita (960×544)", "native"),
                new("Battery (576×326)", "battery"),
                new("Low (480×272)", "low")
            ]);
        AddEasyField(
            _easyPanel,
            _motion,
            "Motion target",
            "Maximum frame-rate target.",
            382,
            18,
            [
                new("Smooth — 30 FPS", "smooth"),
                new("Steady — 20 FPS", "steady"),
                new("Low — 15 FPS", "low")
            ]);
        AddEasyField(
            _easyPanel,
            _gpu,
            "PowerVR GPU name",
            "SGX540 is the fastest compatibility identity.",
            20,
            138,
            [
                new("PowerVR SGX 540 — fastest", "fastest"),
                new("PowerVR SGX 541 — fast", "fast"),
                new("PowerVR SGX 542 — medium", "medium"),
                new("PowerVR SGX 543 — quality", "quality"),
                new("PowerVR SGX 543MP — original", "original")
            ]);
        AddEasyField(
            _easyPanel,
            _effects,
            "Effects",
            "Minimal is fastest; Full also enables shadows.",
            382,
            138,
            [
                new("Outlines only — recommended", "outlines"),
                new("Minimal — fastest", "minimal"),
                new("Full — demanding", "full")
            ]);
        AddEasyField(
            _easyPanel,
            _world,
            "World detail",
            "Controls geometry detail and view distance.",
            20,
            258,
            [
                new("Balanced — recommended", "balanced"),
                new("Fast", "fast"),
                new("Detailed", "detailed"),
                new("Unlimited — demanding", "unlimited")
            ]);
        AddEasyField(
            _easyPanel,
            _power,
            "CPU power",
            "Performance uses 444 MHz; Battery adapts speed.",
            382,
            258,
            [
                new("Performance — 444 MHz", "performance"),
                new("Battery — adaptive", "battery")
            ]);
    }

    private void BuildAdvancedPanel()
    {
        AddAdvancedCombo(_resolution, "Resolution", 20, 18, [
            new("720×408 — recommended", "720x408"), new("960×544 — native", "960x544"),
            new("800×452", "800x452"), new("640×362", "640x362"),
            new("576×326", "576x326"), new("480×272", "480x272")
        ]);
        AddAdvancedCombo(_fps, "FPS cap", 382, 18, [
            new("30 FPS — recommended", "30"), new("60 FPS — maximum target", "60"),
            new("20 FPS", "20"), new("15 FPS", "15")
        ]);
        AddAdvancedCombo(_advancedGpu, "PowerVR GPU name", 20, 80, [
            new("PowerVR SGX 540 — fastest", "sgx540"), new("PowerVR SGX 541", "sgx541"),
            new("PowerVR SGX 542", "sgx542"), new("PowerVR SGX 543", "sgx543"),
            new("PowerVR SGX 543MP — original", "sgx543mp")
        ]);
        AddAdvancedCombo(_clock, "CPU clock", 382, 80, [
            new("444 MHz — performance", "444"), new("Adaptive — battery", "adaptive")
        ]);
        AddAdvancedCombo(_outlines, "Outlines", 20, 142, OnOff());
        AddAdvancedCombo(_shadows, "Shadows", 382, 142, OnOff(defaultOn: false));

        Label detailLabel = CreateFieldLabel("Detail (100–1000)", 20, 204);
        _detail.Location = new Point(20, 226);
        _detail.Size = new Size(326, 28);
        _detail.Minimum = 100;
        _detail.Maximum = 1000;
        _detail.Increment = 50;
        _detail.BackColor = Field;
        _detail.ForeColor = TextMain;
        _detail.BorderStyle = BorderStyle.FixedSingle;
        _detail.ValueChanged += (_, _) => RefreshSummary();
        _advancedPanel.Controls.AddRange([detailLabel, _detail]);

        AddAdvancedCombo(_distance, "Draw distance", 382, 204, [
            new("3500 — balanced", "3500"), new("2500 — fastest", "2500"),
            new("5000 — detailed", "5000"), new("6000 — far", "6000"),
            new("Unlimited — demanding", "0")
        ]);
        AddAdvancedCombo(_upscale, "Upscale filter", 20, 266, [
            new("Linear — smoother", "linear"), new("Nearest — sharper pixels", "nearest")
        ]);
        AddAdvancedCombo(_vsync, "VSync", 382, 266, OnOff());
        AddAdvancedCombo(_nearest, "Thin-seam fix", 20, 328, OnOff(defaultOn: false));
        AddAdvancedCombo(_fbfetch, "White glass/light fix", 382, 328, OnOff(defaultOn: false));

        _toolTip.SetToolTip(_advancedGpu, "Changes the GPU identity reported to the engine; it does not overclock the Vita GPU.");
        _toolTip.SetToolTip(_nearest, "Nearest texture filtering can remove thin white seams but changes image filtering.");
        _toolTip.SetToolTip(_fbfetch, "Use only if glass or light effects render as solid white.");
    }

    private void AddEasyField(
        Control parent,
        ComboBox combo,
        string title,
        string help,
        int x,
        int y,
        ProfileOption[] options)
    {
        Label label = CreateFieldLabel(title, x, y);
        ConfigureCombo(combo, x, y + 22, 326, options);
        Label description = new()
        {
            AutoSize = false,
            Text = help,
            ForeColor = TextSoft,
            Font = new Font("Segoe UI", 8.2f),
            Location = new Point(x, y + 55),
            Size = new Size(326, 38)
        };
        parent.Controls.AddRange([label, combo, description]);
    }

    private void AddAdvancedCombo(ComboBox combo, string title, int x, int y, ProfileOption[] options)
    {
        Label label = CreateFieldLabel(title, x, y);
        ConfigureCombo(combo, x, y + 22, 326, options);
        _advancedPanel.Controls.AddRange([label, combo]);
    }

    private static ProfileOption[] OnOff(bool defaultOn = true) => defaultOn
        ? [new("On", "on"), new("Off", "off")]
        : [new("Off", "off"), new("On", "on")];

    private void ConfigureCombo(ComboBox combo, int x, int y, int width, ProfileOption[] options)
    {
        combo.Location = new Point(x, y);
        combo.Size = new Size(width, 28);
        combo.DropDownStyle = ComboBoxStyle.DropDownList;
        combo.FlatStyle = FlatStyle.Flat;
        combo.BackColor = Field;
        combo.ForeColor = TextMain;
        combo.Font = new Font("Segoe UI", 9f);
        combo.Items.AddRange(options);
        combo.SelectedIndexChanged += (_, _) => RefreshSummary();
    }

    private void LoadSettings(CustomProfileSettings settings)
    {
        _loadingSettings = true;
        SelectValue(_picture, settings.Picture);
        SelectValue(_motion, settings.Motion);
        SelectValue(_gpu, settings.Gpu);
        SelectValue(_effects, settings.Effects);
        SelectValue(_world, settings.World);
        SelectValue(_power, settings.Power);

        SelectValue(_resolution, settings.Resolution);
        SelectValue(_fps, settings.FpsCap.ToString());
        SelectValue(_advancedGpu, settings.AdvancedGpu);
        SelectValue(_outlines, settings.Outlines);
        SelectValue(_shadows, settings.Shadows);
        _detail.Value = Math.Clamp(settings.Detail, (int)_detail.Minimum, (int)_detail.Maximum);
        SelectValue(_distance, settings.DrawDistance.ToString());
        SelectValue(_clock, settings.Clock);
        SelectValue(_upscale, settings.Upscale);
        SelectValue(_vsync, settings.Vsync);
        SelectValue(_nearest, settings.NearestFilter);
        SelectValue(_fbfetch, settings.FbfetchZero);
        _loadingSettings = false;
        SetMode(settings.Mode == "advanced" ? "advanced" : "easy");
    }

    private CustomProfileSettings ReadSettings() => new()
    {
        Mode = _mode,
        Picture = ValueOf(_picture),
        Motion = ValueOf(_motion),
        Gpu = ValueOf(_gpu),
        Effects = ValueOf(_effects),
        World = ValueOf(_world),
        Power = ValueOf(_power),
        Resolution = ValueOf(_resolution),
        FpsCap = int.Parse(ValueOf(_fps)),
        AdvancedGpu = ValueOf(_advancedGpu),
        Outlines = ValueOf(_outlines),
        Shadows = ValueOf(_shadows),
        Detail = (int)_detail.Value,
        DrawDistance = int.Parse(ValueOf(_distance)),
        Clock = ValueOf(_clock),
        Upscale = ValueOf(_upscale),
        Vsync = ValueOf(_vsync),
        NearestFilter = ValueOf(_nearest),
        FbfetchZero = ValueOf(_fbfetch)
    };

    private void SetMode(string mode)
    {
        _mode = mode;
        _easyPanel.Visible = mode == "easy";
        _advancedPanel.Visible = mode == "advanced";
        StyleModeButton(_easyButton, mode == "easy");
        StyleModeButton(_advancedButton, mode == "advanced");
        RefreshSummary();
    }

    private void RefreshSummary()
    {
        if (_loadingSettings || _picture.SelectedItem is null || _resolution.SelectedItem is null)
        {
            return;
        }
        CustomProfileSettings current = ReadSettings();
        _summary.Text = $"Will build: Custom · {current.Summary}";
    }

    private static void SelectValue(ComboBox combo, string value)
    {
        for (int index = 0; index < combo.Items.Count; index++)
        {
            if (combo.Items[index] is ProfileOption option
                && option.Value.Equals(value, StringComparison.Ordinal))
            {
                combo.SelectedIndex = index;
                return;
            }
        }
        combo.SelectedIndex = 0;
    }

    private static string ValueOf(ComboBox combo) =>
        (combo.SelectedItem as ProfileOption)?.Value
        ?? throw new InvalidDataException("Choose every custom graphics option.");

    private static void ConfigureContentPanel(Panel panel)
    {
        panel.Bounds = new Rectangle(28, 160, 744, 402);
        panel.BackColor = Card;
        panel.Paint += (_, e) =>
        {
            using Pen pen = new(Border, 1);
            e.Graphics.DrawRectangle(pen, 0, 0, panel.ClientSize.Width - 1, panel.ClientSize.Height - 1);
        };
    }

    private static Panel CreateCard(Rectangle bounds)
    {
        Panel panel = new() { Bounds = bounds, BackColor = Card };
        panel.Paint += (_, e) =>
        {
            using Pen pen = new(Border, 1);
            e.Graphics.DrawRectangle(pen, 0, 0, panel.ClientSize.Width - 1, panel.ClientSize.Height - 1);
        };
        return panel;
    }

    private static Label CreateFieldLabel(string text, int x, int y) => new()
    {
        AutoSize = true,
        Text = text.ToUpperInvariant(),
        Font = new Font("Segoe UI Semibold", 8.3f),
        ForeColor = TextSoft,
        Location = new Point(x, y)
    };

    private static void ConfigureModeButton(Button button, string text, int x)
    {
        button.Text = text;
        button.Location = new Point(x, 12);
        button.Size = new Size(198, 34);
        button.FlatStyle = FlatStyle.Flat;
        button.Cursor = Cursors.Hand;
        button.Font = new Font("Segoe UI Semibold", 8.8f);
    }

    private static void StyleModeButton(Button button, bool selected)
    {
        button.FlatAppearance.BorderColor = selected ? Primary : Border;
        button.BackColor = selected ? PrimaryDark : Field;
        button.ForeColor = selected ? Primary : TextSoft;
    }

    private static Button CreateGhostButton(string text, int x, int y, int width)
    {
        Button button = new()
        {
            Text = text,
            Location = new Point(x, y),
            Size = new Size(width, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Page,
            ForeColor = TextSoft,
            Cursor = Cursors.Hand
        };
        button.FlatAppearance.BorderSize = 0;
        return button;
    }

    private static Button CreatePrimaryButton(string text, int x, int y, int width)
    {
        Button button = new()
        {
            Text = text,
            Location = new Point(x, y),
            Size = new Size(width, 34),
            FlatStyle = FlatStyle.Flat,
            BackColor = Primary,
            ForeColor = Color.FromArgb(5, 46, 22),
            Cursor = Cursors.Hand
        };
        button.FlatAppearance.BorderSize = 0;
        return button;
    }

    private sealed record ProfileOption(string Label, string Value)
    {
        public override string ToString() => Label;
    }
}
