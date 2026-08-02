namespace McsmVitaDataBuilder;

public sealed class ExtrasDialog : Form
{
    private static readonly Color Page = Color.FromArgb(10, 15, 28);
    private static readonly Color Card = Color.FromArgb(22, 31, 50);
    private static readonly Color Field = Color.FromArgb(11, 18, 32);
    private static readonly Color Border = Color.FromArgb(50, 64, 86);
    private static readonly Color Primary = Color.FromArgb(52, 211, 153);
    private static readonly Color Warning = Color.FromArgb(251, 191, 36);
    private static readonly Color TextMain = Color.FromArgb(241, 245, 249);
    private static readonly Color TextSoft = Color.FromArgb(148, 163, 184);

    private readonly ButtonFixBundle? _bundledFix;
    private readonly string? _bundledFixError;
    private readonly Label _fixStatus = new();
    private readonly ListBox _addonList = new();
    private readonly List<DataAddonSource> _addons;
    private string? _buttonFixPath;

    public string? ButtonFixPath => _buttonFixPath;
    public IReadOnlyList<DataAddonSource> DataAddons => _addons.ToList();

    public ExtrasDialog(
        ButtonFixBundle? bundledFix,
        string? bundledFixError,
        string? selectedButtonFixPath,
        IEnumerable<DataAddonSource> dataAddons)
    {
        _bundledFix = bundledFix;
        _bundledFixError = bundledFixError;
        _buttonFixPath = selectedButtonFixPath;
        _addons = dataAddons.ToList();

        Text = "Fixes and experimental mods";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ClientSize = new Size(760, 610);
        BackColor = Page;
        ForeColor = TextMain;
        Font = new Font("Segoe UI", 9.5f);

        BuildUi();
        RefreshFixStatus();
        RefreshAddonList();
    }

    private void BuildUi()
    {
        Label title = new()
        {
            AutoSize = true,
            Text = "Fixes & data add-ons",
            Font = new Font("Segoe UI Semibold", 22f),
            ForeColor = TextMain,
            Location = new Point(28, 20)
        };
        Label subtitle = new()
        {
            AutoSize = true,
            Text = "Optional tools stay out of the normal APK + OBB setup flow.",
            ForeColor = TextSoft,
            Location = new Point(31, 62)
        };

        Panel fixCard = CreateCard(new Rectangle(28, 94, 704, 142));
        Label fixHeading = CreateHeading("Controller button fix", 20, 16);
        Label fixDescription = CreateSoftLabel(
            "The built-in pack is automatic. You can supply a different complete fix folder or ZIP.",
            20,
            46);
        _fixStatus.AutoSize = false;
        _fixStatus.Location = new Point(20, 73);
        _fixStatus.Size = new Size(650, 22);
        _fixStatus.Font = new Font("Segoe UI Semibold", 9f);

        Button chooseFixFolder = CreateButton("Choose folder", 20, 102, 118);
        Button chooseFixZip = CreateButton("Choose ZIP", 146, 102, 104);
        Button useBuiltIn = CreateGhostButton("Reset to built-in", 260, 102, 142);
        chooseFixFolder.Click += (_, _) => ChooseFixFolder();
        chooseFixZip.Click += (_, _) => ChooseFixZip();
        useBuiltIn.Click += (_, _) =>
        {
            _buttonFixPath = null;
            RefreshFixStatus();
        };
        fixCard.Controls.AddRange([
            fixHeading, fixDescription, _fixStatus,
            chooseFixFolder, chooseFixZip, useBuiltIn
        ]);

        Panel modCard = CreateCard(new Rectangle(28, 248, 704, 276));
        Label modHeading = CreateHeading("Data add-ons / mods", 20, 16);
        Label warning = new()
        {
            AutoSize = false,
            Text = "EXPERIMENTAL — UNTESTED ON VITA. Add-ons are merged last and may replace assets or settings. " +
                   "Core native libraries and OBBs are protected.",
            ForeColor = Warning,
            Font = new Font("Segoe UI Semibold", 9f),
            Location = new Point(20, 47),
            Size = new Size(655, 42)
        };

        _addonList.Location = new Point(20, 94);
        _addonList.Size = new Size(520, 150);
        _addonList.BackColor = Field;
        _addonList.ForeColor = TextMain;
        _addonList.BorderStyle = BorderStyle.FixedSingle;
        _addonList.IntegralHeight = false;

        Button addFolder = CreateButton("Add folder", 552, 94, 126);
        Button addZip = CreateButton("Add ZIP", 552, 132, 126);
        Button remove = CreateGhostButton("Remove", 552, 170, 126);
        addFolder.Click += async (_, _) => await AddFolderAsync();
        addZip.Click += async (_, _) => await AddZipAsync();
        remove.Click += (_, _) =>
        {
            if (_addonList.SelectedItem is DataAddonSource selected)
            {
                _addons.Remove(selected);
                RefreshAddonList();
            }
        };
        modCard.Controls.AddRange([modHeading, warning, _addonList, addFolder, addZip, remove]);

        Label footer = new()
        {
            AutoSize = true,
            Text = "Folders named mcsm and ZIP paths containing mcsm/ are detected automatically.",
            ForeColor = TextSoft,
            Location = new Point(31, 540)
        };
        Button cancel = CreateGhostButton("Cancel", 508, 565, 96);
        Button done = CreatePrimaryButton("SAVE OPTIONS", 612, 561, 120);
        cancel.DialogResult = DialogResult.Cancel;
        done.DialogResult = DialogResult.OK;
        CancelButton = cancel;
        AcceptButton = done;

        Controls.AddRange([title, subtitle, fixCard, modCard, footer, cancel, done]);
    }

    private void ChooseFixFolder()
    {
        using FolderBrowserDialog dialog = new()
        {
            Description = "Choose the complete controller button-fix folder",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = false
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            SetButtonFix(dialog.SelectedPath);
        }
    }

    private void ChooseFixZip()
    {
        using OpenFileDialog dialog = new()
        {
            Title = "Choose the controller button-fix ZIP",
            Filter = "ZIP archive (*.zip)|*.zip|All files (*.*)|*.*",
            CheckFileExists = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            SetButtonFix(dialog.FileName);
        }
    }

    private void SetButtonFix(string path)
    {
        try
        {
            ButtonFixBundle fix = DataBuilderService.InspectButtonFixSource(path);
            _buttonFixPath = path;
            _fixStatus.Text = $"Custom fix ready · {fix.FileCount} assets · {FormatBytes(fix.TotalBytes)}";
            _fixStatus.ForeColor = Primary;
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "Button fix not accepted", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private async Task AddFolderAsync()
    {
        using FolderBrowserDialog dialog = new()
        {
            Description = "Choose a mod folder, mcsm folder, or folder containing mcsm",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = false
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            await AddAddonAsync(dialog.SelectedPath);
        }
    }

    private async Task AddZipAsync()
    {
        using OpenFileDialog dialog = new()
        {
            Title = "Choose one or more mod/data ZIPs",
            Filter = "ZIP archive (*.zip)|*.zip|All files (*.*)|*.*",
            CheckFileExists = true,
            Multiselect = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            foreach (string path in dialog.FileNames)
            {
                await AddAddonAsync(path);
            }
        }
    }

    private async Task AddAddonAsync(string path)
    {
        if (_addons.Any(addon => addon.Path.Equals(Path.GetFullPath(path), StringComparison.OrdinalIgnoreCase)))
        {
            MessageBox.Show(this, "That data add-on is already listed.", "Already added", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        try
        {
            UseWaitCursor = true;
            DataAddonSource addon = await Task.Run(() => DataAddonScanner.Inspect(path));
            if (_addons.Any(existing => existing.Path.Equals(addon.Path, StringComparison.OrdinalIgnoreCase)))
            {
                MessageBox.Show(this, "That data add-on is already listed.", "Already added", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            _addons.Add(addon);
            RefreshAddonList();
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "Data add-on not accepted", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        finally
        {
            UseWaitCursor = false;
        }
    }

    private void RefreshFixStatus()
    {
        if (_buttonFixPath is not null)
        {
            try
            {
                ButtonFixBundle selected = DataBuilderService.InspectButtonFixSource(_buttonFixPath);
                _fixStatus.Text = $"Custom fix ready · {selected.FileCount} assets · {FormatBytes(selected.TotalBytes)}";
                _fixStatus.ForeColor = Primary;
                return;
            }
            catch (Exception exception)
            {
                _fixStatus.Text = $"Custom fix error: {exception.Message}";
                _fixStatus.ForeColor = Warning;
                return;
            }
        }

        if (_bundledFixError is not null)
        {
            _fixStatus.Text = $"Built-in fix error: {_bundledFixError}";
            _fixStatus.ForeColor = Warning;
        }
        else if (_bundledFix is not null)
        {
            _fixStatus.Text = $"Built into this EXE · {_bundledFix.FileCount} assets · {FormatBytes(_bundledFix.TotalBytes)}";
            _fixStatus.ForeColor = Primary;
        }
        else
        {
            _fixStatus.Text = "No fix bundled. Choose your local button-fix folder or ZIP.";
            _fixStatus.ForeColor = TextSoft;
        }
    }

    private void RefreshAddonList()
    {
        _addonList.BeginUpdate();
        _addonList.Items.Clear();
        foreach (DataAddonSource addon in _addons)
        {
            _addonList.Items.Add(addon);
        }
        _addonList.EndUpdate();
    }

    private static Panel CreateCard(Rectangle bounds)
    {
        Panel panel = new()
        {
            Bounds = bounds,
            BackColor = Card
        };
        panel.Paint += (_, e) =>
        {
            using Pen pen = new(Border, 1);
            e.Graphics.DrawRectangle(pen, 0, 0, panel.ClientSize.Width - 1, panel.ClientSize.Height - 1);
        };
        return panel;
    }

    private static Label CreateHeading(string text, int x, int y) => new()
    {
        AutoSize = true,
        Text = text,
        Font = new Font("Segoe UI Semibold", 13f),
        ForeColor = TextMain,
        Location = new Point(x, y)
    };

    private static Label CreateSoftLabel(string text, int x, int y) => new()
    {
        AutoSize = true,
        Text = text,
        ForeColor = TextSoft,
        Location = new Point(x, y)
    };

    private static Button CreateButton(string text, int x, int y, int width)
    {
        Button button = new()
        {
            Text = text,
            Location = new Point(x, y),
            Size = new Size(width, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.FromArgb(30, 41, 59),
            ForeColor = TextMain,
            Cursor = Cursors.Hand
        };
        button.FlatAppearance.BorderColor = Border;
        return button;
    }

    private static Button CreateGhostButton(string text, int x, int y, int width)
    {
        Button button = CreateButton(text, x, y, width);
        button.FlatAppearance.BorderSize = 0;
        button.BackColor = Page;
        button.ForeColor = TextSoft;
        return button;
    }

    private static Button CreatePrimaryButton(string text, int x, int y, int width)
    {
        Button button = CreateButton(text, x, y, width);
        button.FlatAppearance.BorderSize = 0;
        button.BackColor = Primary;
        button.ForeColor = Color.FromArgb(5, 46, 22);
        button.Font = new Font("Segoe UI Semibold", 9f);
        return button;
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB"];
        double value = bytes;
        int unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return $"{value:0.#} {units[unit]}";
    }
}
