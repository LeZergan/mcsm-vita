using System.IO.Compression;
using System.Text;
using McsmVitaDataBuilder;

if (args.Length == 2 && args[0].Equals("--render", StringComparison.OrdinalIgnoreCase))
{
    RenderForm(args[1], () => new MainForm());
    Console.WriteLine($"Rendered UI preview: {args[1]}");
    return 0;
}

if (args.Length == 2 && args[0].Equals("--render-extras", StringComparison.OrdinalIgnoreCase))
{
    RenderForm(
        args[1],
        () => new ExtrasDialog(DataBuilderService.InspectBundledButtonFix(), null, null, []));
    Console.WriteLine($"Rendered extras UI preview: {args[1]}");
    return 0;
}

string root = Path.Combine(Path.GetTempPath(), $"mcsm-data-builder-smoke-{Guid.NewGuid():N}");
try
{
    Directory.CreateDirectory(root);
    string inputs = Path.Combine(root, "inputs");
    Directory.CreateDirectory(inputs);

    string apkPath = Path.Combine(inputs, "minecraft.apk");
    using (ZipArchive apk = ZipFile.Open(apkPath, ZipArchiveMode.Create))
    {
        foreach (string library in DataBuilderService.RequiredLibraries)
        {
            WriteEntry(apk, $"lib/armeabi-v7a/{library}", $"synthetic-{library}");
        }
        WriteEntry(apk, "assets/feedInfo.dat", "synthetic-feed");
        WriteEntry(apk, "assets/nested/config.bin", "synthetic-config");
        WriteEntry(apk, "lib/x86/libmain.so", "wrong-abi-copy");
    }

    string mainObb = Path.Combine(inputs, "main.99999.com.telltalegames.minecraft100.obb");
    await File.WriteAllBytesAsync(mainObb, new byte[2 * 1024 * 1024]);
    string patchObb = Path.Combine(inputs, "patch.99999.com.telltalegames.minecraft100.obb");
    await File.WriteAllTextAsync(patchObb, "synthetic patch");

    string episode2 = Path.Combine(inputs, "episode2", "com.telltalegames.minecraft100", "files", "Net");
    Directory.CreateDirectory(episode2);
    await File.WriteAllTextAsync(
        Path.Combine(episode2, "MCSM_android_Minecraft102_data.ttarch2"),
        "episode 2 marker");
    await File.WriteAllTextAsync(
        Path.Combine(episode2, "_resdesc_50_Minecraft102_android-pvr.lua"),
        "episode 2 descriptor");
    await File.WriteAllTextAsync(
        Path.Combine(episode2, "MCSM_android_JesseMale105_dlog.ttarch2"),
        "cross-episode legacy filename must not change detection");
    await File.WriteAllTextAsync(Path.Combine(episode2, "do-not-copy.txt"), "ignored");

    string episode3Zip = Path.Combine(inputs, "episode3.zip");
    using (ZipArchive zip = ZipFile.Open(episode3Zip, ZipArchiveMode.Create))
    {
        WriteEntry(zip, "nested/Net/MCSM_android_Minecraft103_data.ttarch2", "episode 3 marker");
        WriteEntry(zip, "nested/Net/_resdesc_50_Minecraft103_android-pvr.lua", "episode 3 descriptor");
        WriteEntry(zip, "nested/Net/readme.txt", "ignored");
    }

    string buttonFix = Path.Combine(inputs, "button-fix");
    Directory.CreateDirectory(buttonFix);
    foreach (string mesh in new[]
             {
                 "ui_action_promptFacebuttonDown.d3dmesh",
                 "ui_action_promptFacebuttonLeft.d3dmesh",
                 "ui_action_promptFacebuttonRight.d3dmesh",
                 "ui_action_promptFacebuttonUp.d3dmesh"
             })
    {
        await File.WriteAllTextAsync(Path.Combine(buttonFix, mesh), $"synthetic {mesh}");
    }

    string modFolder = Path.Combine(inputs, "folder-mod", "mcsm");
    Directory.CreateDirectory(Path.Combine(modFolder, "assets"));
    Directory.CreateDirectory(Path.Combine(modFolder, "settings"));
    await File.WriteAllTextAsync(Path.Combine(modFolder, "assets", "folder-mod.asset"), "folder mod");
    await File.WriteAllTextAsync(Path.Combine(modFolder, "settings", "folder-mod.txt"), "enabled");

    string modZip = Path.Combine(inputs, "zip-mod.zip");
    using (ZipArchive zip = ZipFile.Open(modZip, ZipArchiveMode.Create))
    {
        WriteEntry(zip, "package/mcsm/assets/feedInfo.dat", "modded feed");
        WriteEntry(zip, "package/mcsm/User/zip-mod.cfg", "enabled");
    }

    string unsafeMod = Path.Combine(inputs, "unsafe-mod");
    Directory.CreateDirectory(unsafeMod);
    await File.WriteAllTextAsync(Path.Combine(unsafeMod, "libmain.so"), "must be blocked");
    bool protectedFileRejected = false;
    try
    {
        _ = DataAddonScanner.Inspect(unsafeMod);
    }
    catch (InvalidDataException)
    {
        protectedFileRejected = true;
    }
    Assert(protectedFileRejected, "A data add-on was allowed to replace a protected native library.");

    ChapterSource source2 = ChapterScanner.Inspect(Path.Combine(inputs, "episode2"));
    ChapterSource source3 = ChapterScanner.Inspect(episode3Zip);
    DataAddonSource addonFolder = DataAddonScanner.Inspect(Path.Combine(inputs, "folder-mod"));
    DataAddonSource addonZip = DataAddonScanner.Inspect(modZip);
    Assert(source2.Episodes.SequenceEqual([2]), "Episode 2 folder detection failed.");
    Assert(source3.Episodes.SequenceEqual([3]), "Episode 3 ZIP detection failed.");
    Assert(
        ChapterScanner.DetectEpisodes([
            "MCSM_android_JesseMale105_dlog.ttarch2",
            "MCSM_android_Minecraft107_data.ttarch2"
        ]).SequenceEqual([7]),
        "A cross-episode legacy filename confused marker-based detection.");

    string output = Path.Combine(root, "result", "mcsm");
    var request = new BuildRequest(
        apkPath,
        mainObb,
        output,
        [source2, source3],
        "balanced",
        "ru",
        buttonFix,
        [addonFolder, addonZip]);

    DataBuilderService builder = new();
    BuildResult first = await builder.BuildAsync(request);
    Assert(first.IncludedEpisodes.SequenceEqual([1, 2, 3]), "Included episode list is wrong.");
    Assert(first.ButtonFixFileCount == 4, "User-supplied controller fix was not installed.");
    Assert(first.DataAddonFileCount == 4, "Experimental data add-on file count is wrong.");
    Assert(first.DataAddonOverwriteCount == 1, "Data add-on replacement count is wrong.");
    Assert(File.Exists(Path.Combine(output, DataBuilderService.MainObbName)), "Canonical main OBB is missing.");
    Assert(File.Exists(Path.Combine(output, DataBuilderService.PatchObbName)), "Auto-detected patch OBB is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "feedInfo.dat")), "APK asset is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "nested", "config.bin")), "Nested APK asset is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft102_data.ttarch2")), "Episode 2 marker is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft103_data.ttarch2")), "Episode 3 marker is missing.");
    Assert(!File.Exists(Path.Combine(output, "assets", "do-not-copy.txt")), "Unrelated chapter file was copied.");
    Assert(File.Exists(Path.Combine(output, "assets", "ui_action_promptFacebuttonDown.d3dmesh")), "Controller fix mesh is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "folder-mod.asset")), "Folder data add-on is missing.");
    Assert(File.Exists(Path.Combine(output, "User", "zip-mod.cfg")), "ZIP data add-on is missing.");
    Assert(await File.ReadAllTextAsync(Path.Combine(output, "assets", "feedInfo.dat")) == "modded feed", "Data add-on was not applied last.");
    Assert(Directory.Exists(Path.Combine(output, "Temp")), "Temp runtime folder is missing.");
    Assert(Directory.Exists(Path.Combine(output, "User")), "User runtime folder is missing.");

    string graphics = await File.ReadAllTextAsync(Path.Combine(output, "settings", "graphics.txt"));
    string game = await File.ReadAllTextAsync(Path.Combine(output, "settings", "game.txt"));
    Assert(graphics.Contains("profile = balanced"), "Selected graphics profile was not written.");
    Assert(graphics.Contains("advanced_fps_cap         = 30        # 60 | 30 | 20 | 15"), "Advanced 60 FPS choice is missing.");
    Assert(!graphics.Contains("toon", StringComparison.OrdinalIgnoreCase), "Removed wording returned.");
    Assert(game.Contains("language = ru"), "Selected language was not written.");
    Assert(game.Contains("chapters = auto"), "Episode auto-detection is not enabled.");

    ButtonFixBundle? embeddedFix = DataBuilderService.InspectBundledButtonFix();
    BuildRequest embeddedRequest = request with
    {
        ButtonFixPath = null,
        DataAddons = []
    };
    BuildResult second = await builder.BuildAsync(embeddedRequest);
    Assert(second.BackupDirectory is not null && Directory.Exists(second.BackupDirectory), "Existing output was not preserved as a backup.");
    Assert(second.ButtonFixFileCount == (embeddedFix?.FileCount ?? 0), "Built-in controller fix count is wrong.");
    if (embeddedFix is not null)
    {
        Assert(File.Exists(Path.Combine(output, "assets", embeddedFix.Assets[0].Name)), "Built-in controller fix was not extracted.");
    }
    Assert(File.Exists(Path.Combine(output, "DATA_FOLDER_READY.txt")), "Final ready marker is missing.");

    Console.WriteLine("PASS: APK/OBB extraction, chapters, supplied/built-in controller fixes, experimental folder/ZIP data add-ons, settings, validation, and backups.");
    return 0;
}
catch (Exception exception)
{
    Console.Error.WriteLine(exception);
    return 1;
}
finally
{
    if (Directory.Exists(root))
    {
        Directory.Delete(root, recursive: true);
    }
}

static void WriteEntry(ZipArchive archive, string path, string content)
{
    ZipArchiveEntry entry = archive.CreateEntry(path, CompressionLevel.NoCompression);
    using StreamWriter writer = new(entry.Open(), new UTF8Encoding(false));
    writer.Write(content);
}

static void Assert(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

static void RenderForm(string outputPath, Func<Form> formFactory)
{
    Exception? failure = null;
    Thread thread = new(() =>
    {
        try
        {
            Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
            Application.EnableVisualStyles();
            using Form form = formFactory();
            form.Show();
            Application.DoEvents();
            form.Refresh();
            using Bitmap bitmap = new(form.Width, form.Height);
            form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputPath))!);
            bitmap.Save(outputPath, System.Drawing.Imaging.ImageFormat.Png);
            form.Close();
        }
        catch (Exception exception)
        {
            failure = exception;
        }
    });
    thread.SetApartmentState(ApartmentState.STA);
    thread.Start();
    thread.Join();
    if (failure is not null)
    {
        throw failure;
    }
}
