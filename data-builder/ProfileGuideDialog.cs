namespace McsmVitaDataBuilder;

public sealed class ProfileGuideDialog : Form
{
    private static readonly Color Page = Color.FromArgb(10, 15, 28);
    private static readonly Color Card = Color.FromArgb(22, 31, 50);
    private static readonly Color Border = Color.FromArgb(50, 64, 86);
    private static readonly Color Primary = Color.FromArgb(52, 211, 153);
    private static readonly Color TextMain = Color.FromArgb(241, 245, 249);
    private static readonly Color TextSoft = Color.FromArgb(148, 163, 184);
    private static readonly Color Info = Color.FromArgb(96, 165, 250);

    private static readonly ProfileRow[] Profiles =
    [
        new(
            "BALANCED",
            "RECOMMENDED",
            "720×408  ·  30 FPS cap  ·  SGX 541  ·  full detail  ·  5000 distance  ·  outlines on",
            "The best all-round starting point: clear text, broad scenery and normal animation."),
        new(
            "PERFORMANCE",
            "FASTEST",
            "640×362  ·  30 FPS cap  ·  SGX 540  ·  600 detail  ·  2500 distance  ·  outlines off",
            "Cuts image and distant-world work first, while keeping faces and animation correct."),
        new(
            "QUALITY",
            "SHARPEST",
            "800×452  ·  30 FPS cap  ·  SGX 541  ·  full detail  ·  4000 distance  ·  outlines on",
            "The sharpest preset. Demanding scenes can run slower than Balanced."),
        new(
            "BATTERY",
            "LOWER POWER",
            "576×326  ·  30 FPS cap  ·  SGX 540  ·  700 detail  ·  3000 distance  ·  adaptive CPU",
            "Uses fewer pixels and an adaptive clock to reduce power use."),
        new(
            "CUSTOM",
            "YOUR SETTINGS",
            "Easy choices or exact resolution, 60/30/20/15 cap, PowerVR GPU, effects, detail and clock",
            "A cap is a target, not a guarantee: crowded character scenes can still fall near 20 FPS.")
    ];

    public ProfileGuideDialog()
    {
        Text = "Graphics profiles";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ShowInTaskbar = false;
        ClientSize = new Size(820, 532);
        BackColor = Page;
        ForeColor = TextMain;
        Font = new Font("Segoe UI", 9f);
        HandleCreated += (_, _) => WindowStyling.ApplyDarkTitleBar(this);

        Label title = new()
        {
            AutoSize = true,
            Text = "Choose the feel you want",
            Font = new Font("Segoe UI Semibold", 19f),
            ForeColor = TextMain,
            Location = new Point(26, 20)
        };
        Label subtitle = new()
        {
            AutoSize = true,
            Text = "Balanced is the safe starting point. Every preset stays editable later in settings\\graphics.txt.",
            Font = new Font("Segoe UI", 9.2f),
            ForeColor = TextSoft,
            Location = new Point(28, 57)
        };
        Controls.AddRange([title, subtitle]);

        int y = 88;
        foreach (ProfileRow profile in Profiles)
        {
            Controls.Add(CreateProfileRow(profile, y));
            y += 76;
        }

        Label footnote = new()
        {
            AutoSize = true,
            Text = "FPS values are caps. Heavy scenes may run below them; this does not mean the builder chose the wrong profile.",
            ForeColor = Info,
            Font = new Font("Segoe UI Semibold", 8.5f),
            Location = new Point(28, 474)
        };
        Button close = new()
        {
            Text = "CLOSE",
            DialogResult = DialogResult.OK,
            Location = new Point(684, 493),
            Size = new Size(108, 31),
            BackColor = Primary,
            ForeColor = Color.FromArgb(5, 46, 22),
            FlatStyle = FlatStyle.Flat,
            Font = new Font("Segoe UI Semibold", 9f),
            Cursor = Cursors.Hand
        };
        close.FlatAppearance.BorderSize = 0;
        AcceptButton = close;
        CancelButton = close;
        Controls.AddRange([footnote, close]);
    }

    private static Panel CreateProfileRow(ProfileRow profile, int y)
    {
        Panel panel = new()
        {
            Location = new Point(26, y),
            Size = new Size(766, 67),
            BackColor = Card
        };
        panel.Paint += (_, e) =>
        {
            using Pen pen = new(Border);
            e.Graphics.DrawRectangle(pen, 0, 0, panel.Width - 1, panel.Height - 1);
        };

        bool recommended = profile.Badge == "RECOMMENDED";
        Label name = new()
        {
            AutoSize = false,
            Text = profile.Name,
            Font = new Font("Segoe UI Semibold", 10f),
            ForeColor = recommended ? Primary : TextMain,
            Location = new Point(16, 12),
            Size = new Size(136, 20)
        };
        Label badge = new()
        {
            AutoSize = false,
            Text = profile.Badge,
            TextAlign = ContentAlignment.MiddleCenter,
            Font = new Font("Segoe UI Semibold", 7f),
            ForeColor = recommended ? Color.FromArgb(5, 46, 22) : TextSoft,
            BackColor = recommended ? Primary : Color.FromArgb(30, 41, 59),
            Location = new Point(16, 37),
            Size = new Size(100, 19)
        };
        Label settings = new()
        {
            AutoSize = false,
            Text = profile.Settings,
            Font = new Font("Segoe UI Semibold", 8.5f),
            ForeColor = TextMain,
            Location = new Point(160, 10),
            Size = new Size(586, 20)
        };
        Label meaning = new()
        {
            AutoSize = false,
            Text = profile.Meaning,
            Font = new Font("Segoe UI", 8.4f),
            ForeColor = TextSoft,
            Location = new Point(160, 35),
            Size = new Size(586, 23)
        };
        panel.Controls.AddRange([name, badge, settings, meaning]);
        return panel;
    }

    private sealed record ProfileRow(string Name, string Badge, string Settings, string Meaning);
}
