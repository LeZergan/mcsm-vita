using System.IO.Compression;
using System.Text;
using McsmVitaDataBuilder;

if (args.Length == 2 && args[0].Equals("--render", StringComparison.OrdinalIgnoreCase))
{
    RenderPreview(args[1]);
    Console.WriteLine($"Rendered UI preview: {args[1]}");
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

    ChapterSource source2 = ChapterScanner.Inspect(Path.Combine(inputs, "episode2"));
    ChapterSource source3 = ChapterScanner.Inspect(episode3Zip);
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
        "ru");

    DataBuilderService builder = new();
    BuildResult first = await builder.BuildAsync(request);
    Assert(first.IncludedEpisodes.SequenceEqual([1, 2, 3]), "Included episode list is wrong.");
    Assert(File.Exists(Path.Combine(output, DataBuilderService.MainObbName)), "Canonical main OBB is missing.");
    Assert(File.Exists(Path.Combine(output, DataBuilderService.PatchObbName)), "Auto-detected patch OBB is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "feedInfo.dat")), "APK asset is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "nested", "config.bin")), "Nested APK asset is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft102_data.ttarch2")), "Episode 2 marker is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft103_data.ttarch2")), "Episode 3 marker is missing.");
    Assert(!File.Exists(Path.Combine(output, "assets", "do-not-copy.txt")), "Unrelated chapter file was copied.");
    Assert(Directory.Exists(Path.Combine(output, "Temp")), "Temp runtime folder is missing.");
    Assert(Directory.Exists(Path.Combine(output, "User")), "User runtime folder is missing.");

    string graphics = await File.ReadAllTextAsync(Path.Combine(output, "settings", "graphics.txt"));
    string game = await File.ReadAllTextAsync(Path.Combine(output, "settings", "game.txt"));
    Assert(graphics.Contains("profile = balanced"), "Selected graphics profile was not written.");
    Assert(graphics.Contains("advanced_fps_cap         = 30        # 60 | 30 | 20 | 15"), "Advanced 60 FPS choice is missing.");
    Assert(!graphics.Contains("toon", StringComparison.OrdinalIgnoreCase), "Removed wording returned.");
    Assert(game.Contains("language = ru"), "Selected language was not written.");
    Assert(game.Contains("chapters = auto"), "Episode auto-detection is not enabled.");

    BuildResult second = await builder.BuildAsync(request);
    Assert(second.BackupDirectory is not null && Directory.Exists(second.BackupDirectory), "Existing output was not preserved as a backup.");
    Assert(File.Exists(Path.Combine(output, "DATA_FOLDER_READY.txt")), "Final ready marker is missing.");

    Console.WriteLine("PASS: APK extraction, OBB naming, patch detection, folder/ZIP chapters, settings, validation, and backup replacement.");
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

static void RenderPreview(string outputPath)
{
    Exception? failure = null;
    Thread thread = new(() =>
    {
        try
        {
            Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
            Application.EnableVisualStyles();
            using MainForm form = new();
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
