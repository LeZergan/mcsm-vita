using System.IO.Compression;
using System.Text;
using McsmVitaDataBuilder;

if (args.Length == 2 && args[0].Equals("--inspect-apk", StringComparison.OrdinalIgnoreCase))
{
    ApkLayout apk = DataBuilderService.InspectApk(args[1]);
    Console.WriteLine(
        $"APK accepted: {apk.PackageName} v{apk.VersionName} " +
        $"(versionCode {apk.VersionCode}), {apk.LibraryCount} ARMv7 libraries, {apk.AssetCount} assets.");
    return 0;
}

if (args.Length == 4 && args[0].Equals("--inspect-base", StringComparison.OrdinalIgnoreCase))
{
    ApkLayout apk = DataBuilderService.InspectApk(args[1]);
    ObbLayout main = DataBuilderService.InspectMainObb(args[2]);
    ObbLayout patch = DataBuilderService.InspectPatchObb(args[3]);
    Console.WriteLine(
        $"PowerVR base set accepted: APK v{apk.VersionName} ({apk.VersionCode}), " +
        $"main {main.TotalBytes} bytes, patch {patch.TotalBytes} bytes.");
    return 0;
}

if (args.Length == 2 && args[0].Equals("--scan-folder", StringComparison.OrdinalIgnoreCase))
{
    SetupFolderScanResult found = SetupFolderScanner.Scan(args[1]);
    Console.WriteLine(
        $"Folder scan: APK={(found.ApkPath is null ? "missing" : Path.GetFileName(found.ApkPath))}; " +
        $"main={(found.MainObbPath is null ? "missing" : Path.GetFileName(found.MainObbPath))}; " +
        $"patch={(found.PatchObbPath is null ? "missing" : Path.GetFileName(found.PatchObbPath))}; " +
        $"episodes={string.Join(",", found.Chapters.SelectMany(source => source.Episodes).Distinct().Order())}.");
    return found.BaseSetComplete ? 0 : 2;
}

if (args.Length == 5 && args[0].Equals("--build-real", StringComparison.OrdinalIgnoreCase))
{
    var request = new BuildRequest(
        args[1],
        args[2],
        args[3],
        args[4],
        [],
        "balanced",
        new CustomProfileSettings(),
        "en",
        null,
        []);
    var progress = new Progress<BuildProgress>(value =>
        Console.WriteLine($"{value.Percent,3}%  {value.Status}"));
    BuildResult result = await new DataBuilderService().BuildAsync(request, progress);
    int activeBaseFiles = Directory.EnumerateFiles(
            Path.Combine(result.OutputDirectory, "assets"),
            "*",
            SearchOption.TopDirectoryOnly)
        .Count(path =>
            Path.GetExtension(path).Equals(".ttarch2", StringComparison.OrdinalIgnoreCase)
            || Path.GetExtension(path).Equals(".lua", StringComparison.OrdinalIgnoreCase));
    Console.WriteLine(
        $"Real data build ready: {result.OutputDirectory}; " +
        $"{activeBaseFiles} active base archives/descriptors.");
    return 0;
}

if (args.Length == 2 && args[0].Equals("--render", StringComparison.OrdinalIgnoreCase))
{
    RenderForm(args[1], () => new MainForm());
    Console.WriteLine($"Rendered UI preview: {args[1]}");
    return 0;
}

if (args.Length == 2 && args[0].Equals("--render-ready", StringComparison.OrdinalIgnoreCase))
{
    RenderForm(
        args[1],
        () =>
        {
            MainForm form = new();
            form.ApplyReadyPreview();
            return form;
        });
    Console.WriteLine($"Rendered ready-state UI preview: {args[1]}");
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

if (args.Length == 2 && args[0].Equals("--render-custom", StringComparison.OrdinalIgnoreCase))
{
    RenderForm(args[1], () => new CustomProfileDialog(new CustomProfileSettings()));
    Console.WriteLine($"Rendered easy custom-profile preview: {args[1]}");
    return 0;
}

if (args.Length == 2 && args[0].Equals("--render-custom-advanced", StringComparison.OrdinalIgnoreCase))
{
    RenderForm(
        args[1],
        () => new CustomProfileDialog(new CustomProfileSettings { Mode = "advanced" }));
    Console.WriteLine($"Rendered advanced custom-profile preview: {args[1]}");
    return 0;
}

if (args.Length == 2 && args[0].Equals("--render-profiles", StringComparison.OrdinalIgnoreCase))
{
    RenderForm(args[1], () => new ProfileGuideDialog());
    Console.WriteLine($"Rendered graphics-profile guide preview: {args[1]}");
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
        WriteEntry(
            apk,
            "AndroidManifest.xml",
            "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" " +
            "package=\"com.telltalegames.minecraft100\" " +
            "android:versionCode=\"40137\" android:versionName=\"1.37\" />");
        foreach (string library in DataBuilderService.RequiredLibraries)
        {
            WriteEntry(apk, $"lib/armeabi-v7a/{library}", $"synthetic-{library}");
        }
        WriteEntry(apk, "assets/feedInfo.dat", "synthetic-feed");
        WriteEntry(apk, "assets/nested/config.bin", "synthetic-config");
        WriteEntry(apk, "lib/x86/libmain.so", "wrong-abi-copy");
    }

    ApkLayout inspectedApk = DataBuilderService.InspectSyntheticApk(apkPath);
    Assert(inspectedApk.VersionCode == 40137, "Supported APK version detection failed.");
    Assert(inspectedApk.VersionName == "1.37", "Supported APK version name detection failed.");
    bool unknownRendererRejected = false;
    try
    {
        _ = DataBuilderService.InspectApk(apkPath);
    }
    catch (InvalidDataException)
    {
        unknownRendererRejected = true;
    }
    Assert(unknownRendererRejected, "An unknown v1.37 renderer fingerprint was accepted.");

    string oldApk = Path.Combine(inputs, "minecraft-old.apk");
    using (ZipArchive apk = ZipFile.Open(oldApk, ZipArchiveMode.Create))
    {
        WriteEntry(
            apk,
            "AndroidManifest.xml",
            "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" " +
            "package=\"com.telltalegames.minecraft100\" " +
            "android:versionCode=\"40136\" android:versionName=\"1.36\" />");
        foreach (string library in DataBuilderService.RequiredLibraries)
        {
            WriteEntry(apk, $"lib/armeabi-v7a/{library}", $"synthetic-{library}");
        }
        WriteEntry(apk, "assets/feedInfo.dat", "synthetic-feed");
    }
    bool oldApkRejected = false;
    try
    {
        _ = DataBuilderService.InspectSyntheticApk(oldApk);
    }
    catch (InvalidDataException)
    {
        oldApkRejected = true;
    }
    Assert(oldApkRejected, "An older APK version was accepted.");

    string mainObb = Path.Combine(inputs, "main.99999.com.telltalegames.minecraft100.obb");
    await File.WriteAllBytesAsync(mainObb, new byte[2 * 1024 * 1024]);
    string patchObb = Path.Combine(inputs, "patch.99999.com.telltalegames.minecraft100.obb");
    await File.WriteAllTextAsync(patchObb, "synthetic patch");

    ObbLayout inspectedMainObb = DataBuilderService.InspectSyntheticMainObb(mainObb);
    Assert(
        inspectedMainObb.PatchPath?.Equals(patchObb, StringComparison.OrdinalIgnoreCase) == true,
        "The neighboring required patch OBB was not detected.");
    _ = DataBuilderService.InspectSyntheticPatchObb(patchObb);

    string disguisedApk = Path.Combine(inputs, "minecraft.zip");
    File.Copy(apkPath, disguisedApk);
    bool wrongApkExtensionRejected = false;
    try
    {
        _ = DataBuilderService.InspectSyntheticApk(disguisedApk);
    }
    catch (InvalidDataException)
    {
        wrongApkExtensionRejected = true;
    }
    Assert(wrongApkExtensionRejected, "A non-.apk base package was accepted.");

    string disguisedObb = Path.Combine(inputs, "main-data.bin");
    File.Copy(mainObb, disguisedObb);
    bool wrongObbExtensionRejected = false;
    try
    {
        _ = DataBuilderService.InspectMainObb(disguisedObb);
    }
    catch (InvalidDataException)
    {
        wrongObbExtensionRejected = true;
    }
    Assert(wrongObbExtensionRejected, "A non-.obb main expansion was accepted.");

    bool swappedObbsRejected = false;
    try
    {
        _ = DataBuilderService.InspectMainObb(patchObb);
    }
    catch (InvalidDataException)
    {
        swappedObbsRejected = true;
    }
    Assert(swappedObbsRejected, "A patch OBB was accepted in the main OBB slot.");

    bool mainInPatchSlotRejected = false;
    try
    {
        _ = DataBuilderService.InspectPatchObb(mainObb);
    }
    catch (InvalidDataException)
    {
        mainInPatchSlotRejected = true;
    }
    Assert(mainInPatchSlotRejected, "A main OBB was accepted in the patch OBB slot.");

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

    string episode4Obb = Path.Combine(inputs, "chapter-episode4-powervr.obb");
    using (ZipArchive obb = ZipFile.Open(episode4Obb, ZipArchiveMode.Create))
    {
        WriteEntry(obb, "files/Net/MCSM_android_Minecraft104_data.ttarch2", "episode 4 marker");
        WriteEntry(obb, "files/Net/_resdesc_50_Minecraft104_android-pvr.lua", "episode 4 descriptor");
        WriteEntry(obb, "files/Net/MCSM_android_Minecraft104_mali.ttarch2", "wrong renderer ignored");
    }

    string nestedEpisode5 = Path.Combine(root, "nested-episode5.obb");
    using (ZipArchive obb = ZipFile.Open(nestedEpisode5, ZipArchiveMode.Create))
    {
        WriteEntry(obb, "deep/Net/MCSM_android_Minecraft105_data.ttarch2", "episode 5 marker");
        WriteEntry(obb, "deep/Net/_resdesc_50_Minecraft105_android-pvr.lua", "episode 5 descriptor");
    }
    string fullChapterZip = Path.Combine(inputs, "full-chapters.zip");
    using (ZipArchive zip = ZipFile.Open(fullChapterZip, ZipArchiveMode.Create))
    {
        zip.CreateEntryFromFile(nestedEpisode5, "owned chapters/episode5.obb", CompressionLevel.NoCompression);
        WriteEntry(zip, "notes/readme.txt", "ignored");
    }

    string maliOnlyObb = Path.Combine(inputs, "chapter-episode6-mali.obb");
    using (ZipArchive obb = ZipFile.Open(maliOnlyObb, ZipArchiveMode.Create))
    {
        WriteEntry(obb, "files/Net/MCSM_android_Minecraft106_data.ttarch2", "episode 6 marker");
        WriteEntry(obb, "files/Net/_resdesc_50_Minecraft106_android-mali.lua", "wrong renderer");
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
    ChapterSource source4 = ChapterScanner.Inspect(episode4Obb);
    ChapterSource source5 = ChapterScanner.Inspect(fullChapterZip);
    DataAddonSource addonFolder = DataAddonScanner.Inspect(Path.Combine(inputs, "folder-mod"));
    DataAddonSource addonZip = DataAddonScanner.Inspect(modZip);
    Assert(source2.Episodes.SequenceEqual([2]), "Episode 2 folder detection failed.");
    Assert(source3.Episodes.SequenceEqual([3]), "Episode 3 ZIP detection failed.");
    Assert(source4.Kind == ChapterSourceKind.ObbArchive && source4.Episodes.SequenceEqual([4]), "Episode 4 OBB detection failed.");
    Assert(source5.Episodes.SequenceEqual([5]), "Nested full chapter ZIP detection failed.");
    bool maliChapterRejected = false;
    try
    {
        _ = ChapterScanner.Inspect(maliOnlyObb);
    }
    catch (InvalidDataException)
    {
        maliChapterRejected = true;
    }
    Assert(maliChapterRejected, "A Mali-only chapter OBB was accepted as PowerVR data.");
    SetupFolderScanResult scannedFolder = SetupFolderScanner.Scan(inputs);
    Assert(scannedFolder.ApkPath is null, "Folder scan accepted an unverified synthetic APK.");
    Assert(scannedFolder.MainObbPath is null, "Folder scan accepted an unverified synthetic main OBB.");
    Assert(scannedFolder.PatchObbPath is null, "Folder scan accepted an unverified synthetic patch OBB.");
    Assert(
        scannedFolder.Chapters.SelectMany(source => source.Episodes).Distinct().Order().SequenceEqual([2, 3, 4, 5]),
        "Folder scan did not discover folder, ZIP, OBB, and nested chapter inputs.");
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
        patchObb,
        output,
        [source2, source3, source4, source5],
        "custom",
        new CustomProfileSettings
        {
            Mode = "advanced",
            Resolution = "640x362",
            FpsCap = 60,
            AdvancedGpu = "sgx541",
            Outlines = "off",
            Shadows = "off",
            Detail = 650,
            DrawDistance = 3000,
            Clock = "444",
            Upscale = "nearest",
            Vsync = "off",
            NearestFilter = "on",
            FbfetchZero = "on"
        },
        "ru",
        buttonFix,
        [addonFolder, addonZip]);

    DataBuilderService builder = new(allowSyntheticInputs: true);
    bool missingPatchRejected = false;
    try
    {
        _ = await builder.BuildAsync(request with { PatchObbPath = string.Empty });
    }
    catch (FileNotFoundException)
    {
        missingPatchRejected = true;
    }
    Assert(missingPatchRejected, "A build without the required patch OBB was accepted.");

    bool duplicateObbsRejected = false;
    try
    {
        _ = await builder.BuildAsync(request with { PatchObbPath = mainObb });
    }
    catch (InvalidDataException)
    {
        duplicateObbsRejected = true;
    }
    Assert(duplicateObbsRejected, "The same OBB was accepted for both required OBB slots.");

    BuildResult first = await builder.BuildAsync(request);
    BundledAsset? embeddedChoiceData = DataBuilderService.InspectBundledChoiceData();
    Assert(first.IncludedEpisodes.SequenceEqual([1, 2, 3, 4, 5]), "Included episode list is wrong.");
    Assert(first.ButtonFixFileCount == 4, "User-supplied controller fix was not installed.");
    Assert(first.ChoiceDataIncluded == (embeddedChoiceData is not null), "Offline choice-data status is wrong.");
    Assert(first.DataAddonFileCount == 4, "Experimental data add-on file count is wrong.");
    Assert(first.DataAddonOverwriteCount == 1, "Data add-on replacement count is wrong.");
    Assert(File.Exists(Path.Combine(output, DataBuilderService.MainObbName)), "Canonical main OBB is missing.");
    Assert(File.Exists(Path.Combine(output, DataBuilderService.PatchObbName)), "Auto-detected patch OBB is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "feedInfo.dat")), "APK asset is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "nested", "config.bin")), "Nested APK asset is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft102_data.ttarch2")), "Episode 2 marker is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft103_data.ttarch2")), "Episode 3 marker is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft104_data.ttarch2")), "Episode 4 OBB marker is missing.");
    Assert(File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft105_data.ttarch2")), "Nested Episode 5 marker is missing.");
    Assert(!File.Exists(Path.Combine(output, "assets", "MCSM_android_Minecraft104_mali.ttarch2")), "A Mali chapter asset was copied.");
    Assert(!File.Exists(Path.Combine(output, "assets", "do-not-copy.txt")), "Unrelated chapter file was copied.");
    Assert(File.Exists(Path.Combine(output, "assets", "ui_action_promptFacebuttonDown.d3dmesh")), "Controller fix mesh is missing.");
    if (embeddedChoiceData is not null)
    {
        string rootChoice = Path.Combine(output, "choice.prop");
        string tempChoice = Path.Combine(output, "Temp", "choice.prop");
        Assert(File.Exists(rootChoice), "Root offline choice dataset is missing.");
        Assert(File.Exists(tempChoice), "Temp offline choice dataset is missing.");
        Assert(
            File.ReadAllBytes(rootChoice).SequenceEqual(File.ReadAllBytes(tempChoice)),
            "Offline choice dataset copies do not match.");
    }
    Assert(File.Exists(Path.Combine(output, "assets", "folder-mod.asset")), "Folder data add-on is missing.");
    Assert(File.Exists(Path.Combine(output, "User", "zip-mod.cfg")), "ZIP data add-on is missing.");
    Assert(await File.ReadAllTextAsync(Path.Combine(output, "assets", "feedInfo.dat")) == "modded feed", "Data add-on was not applied last.");
    Assert(Directory.Exists(Path.Combine(output, "Temp")), "Temp runtime folder is missing.");
    Assert(Directory.Exists(Path.Combine(output, "User")), "User runtime folder is missing.");

    string graphics = await File.ReadAllTextAsync(Path.Combine(output, "settings", "graphics.txt"));
    string game = await File.ReadAllTextAsync(Path.Combine(output, "settings", "game.txt"));
    Assert(graphics.Contains("profile = custom"), "Selected custom graphics profile was not written.");
    Assert(graphics.Contains("custom_mode = advanced"), "Custom profile mode was not written.");
    Assert(graphics.Contains("advanced_resolution      = 640x362"), "Custom resolution was not written.");
    Assert(graphics.Contains("advanced_fps_cap         = 60"), "Custom 60 FPS cap was not written.");
    Assert(graphics.Contains("advanced_gpu             = sgx541"), "Custom PowerVR GPU name was not written.");
    Assert(graphics.Contains("advanced_detail          = 650"), "Custom detail was not written.");
    Assert(graphics.Contains("advanced_draw_distance   = 3000"), "Custom draw distance was not written.");
    Assert(graphics.Contains("advanced_nearest_filter  = on"), "Custom seam fix was not written.");
    Assert(graphics.Contains("advanced_fbfetch_zero    = on"), "Custom glass/light fix was not written.");
    Assert(!graphics.Contains("toon", StringComparison.OrdinalIgnoreCase), "Removed wording returned.");
    Assert(game.Contains("language = ru"), "Selected language was not written.");
    Assert(game.Contains("chapters = auto"), "Episode auto-detection is not enabled.");

    ButtonFixBundle? embeddedFix = DataBuilderService.InspectBundledButtonFix();
    BuildRequest embeddedRequest = request with
    {
        GraphicsProfile = "balanced",
        ButtonFixPath = null,
        DataAddons = []
    };
    BuildResult second = await builder.BuildAsync(embeddedRequest);
    Assert(second.BackupDirectory is not null && Directory.Exists(second.BackupDirectory), "Existing output was not preserved as a backup.");
    Assert(second.ButtonFixFileCount == (embeddedFix?.FileCount ?? 0), "Built-in controller fix count is wrong.");
    string balancedGraphics = await File.ReadAllTextAsync(Path.Combine(output, "settings", "graphics.txt"));
    Assert(balancedGraphics.Contains("profile = balanced"), "Recommended Balanced profile was not written.");
    if (embeddedFix is not null)
    {
        Assert(File.Exists(Path.Combine(output, "assets", embeddedFix.Assets[0].Name)), "Built-in controller fix was not extracted.");
    }
    Assert(File.Exists(Path.Combine(output, "DATA_FOLDER_READY.txt")), "Final ready marker is missing.");

    Console.WriteLine("PASS: folder discovery, PowerVR/version checks, profile defaults, chapter folders/OBBs/nested ZIPs, renderer filtering, controller fixes, choice data, add-ons, settings, and backups.");
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
