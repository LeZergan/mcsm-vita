using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace McsmVitaDataBuilder;

public sealed class DataBuilderService
{
    public const string SupportedPackageName = "com.telltalegames.minecraft100";
    public const int SupportedApkVersionCode = 40137;
    public const string SupportedApkVersionName = "1.37";
    public const string SupportedApkSha256 = "3B8111421CC37E96FF6B222548BD476584AA32A1A68A0B2052BF36CA188EE41B";
    public const string SupportedMainObbSha256 = "C4EFFC61744BA051516C79E5B118E5A11E0CD1B1C37F4957594685A9DB0449FD";
    public const string SupportedPatchObbSha256 = "B041CEBF54D361839EC642D3FC5A80D1757C16185D3E8FBE26ED2D2701398AA5";
    public const long SupportedApkBytes = 19_835_654;
    public const long SupportedMainObbBytes = 813_669_332;
    public const long SupportedPatchObbBytes = 3_868_418;
    public const int SupportedMainAssetCount = 111;
    public const int SupportedPatchAssetCount = 34;
    public const long SupportedMainExtractedBytes = 813_600_664;
    public const long SupportedPatchExtractedBytes = 3_801_906;
    public const int SupportedActiveBaseAssetCount = 144;
    public const long SupportedActiveBaseAssetBytes = 817_402_440;
    public const string SupportedChoiceDataSha256 = "F5F0C7FF7467707C7224BF056C6F7111E8D27279AA0BEE3BA422886B7EBB2616";
    public const string MainObbName = "main.40129.com.telltalegames.minecraft100.obb";
    public const string PatchObbName = "patch.40135.com.telltalegames.minecraft100.obb";

    public static readonly string[] RequiredLibraries =
    [
        "libmain.so",
        "libGameEngine.so",
        "libSDL2.so",
        "libfmod.so",
        "libfmodstudio.so"
    ];

    private static readonly string[] RequiredBaseAssets =
    [
        "_rescdesc_50_version_101.lua",
        "MCSM_android-pvr_Project_all.ttarch2",
        "MCSM_android-pvr_Minecraft101_txmesh.ttarch2",
        "MCSM_android_Minecraft101_data.ttarch2",
        "MCSM_android_Minecraft101_ms.ttarch2",
        "MCSM_android_Minecraft101_voice.ttarch2"
    ];

    private static readonly string[] RequiredButtonMeshes =
    [
        "ui_action_promptFacebuttonDown.d3dmesh",
        "ui_action_promptFacebuttonLeft.d3dmesh",
        "ui_action_promptFacebuttonRight.d3dmesh",
        "ui_action_promptFacebuttonUp.d3dmesh"
    ];

    private readonly bool _allowSyntheticInputs;

    public DataBuilderService()
    {
    }

    internal DataBuilderService(bool allowSyntheticInputs)
    {
        _allowSyntheticInputs = allowSyntheticInputs;
    }

    public async Task<BuildResult> BuildAsync(
        BuildRequest request,
        IProgress<BuildProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequest(request);
        ApkLayout apkLayout = InspectApkCore(request.ApkPath, !_allowSyntheticInputs);
        ObbLayout mainObb = InspectMainObbCore(request.MainObbPath, !_allowSyntheticInputs, false);
        ObbLayout patchObb = InspectPatchObbCore(request.PatchObbPath, !_allowSyntheticInputs);
        ButtonFixBundle? buttonFix = ResolveButtonFix(request.ButtonFixPath);
        BundledAsset? choiceData = InspectBundledChoiceData();

        string outputDirectory = Path.GetFullPath(request.OutputDirectory.Trim());
        if (!Path.GetFileName(outputDirectory.TrimEnd(Path.DirectorySeparatorChar))
            .Equals("mcsm", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("The final output folder must be named 'mcsm'.");
        }

        string? parent = Directory.GetParent(outputDirectory)?.FullName;
        if (string.IsNullOrWhiteSpace(parent))
        {
            throw new InvalidDataException("Choose a normal folder, not the root of a drive.");
        }
        Directory.CreateDirectory(parent);

        long extractionBytes = _allowSyntheticInputs
            ? 0
            : SupportedMainExtractedBytes + SupportedPatchExtractedBytes;
        long totalBytes = apkLayout.TotalBytes
            + mainObb.TotalBytes
            + patchObb.TotalBytes
            + extractionBytes
            + (choiceData?.Size * 2 ?? 0)
            + (buttonFix?.TotalBytes ?? 0)
            + request.ChapterSources.Sum(source => source.TotalBytes)
            + request.DataAddons.Sum(source => source.TotalBytes);
        EnsureFreeSpace(outputDirectory, totalBytes);

        string stagingDirectory = Path.Combine(parent, $".mcsm-building-{Guid.NewGuid():N}");
        string? backupDirectory = null;
        var copiedNames = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        long copiedBytes = 0;
        int chapterFileCount = 0;
        var includedEpisodes = new SortedSet<int> { 1 };

        void Report(string status)
        {
            int percent = totalBytes <= 0
                ? 0
                : (int)Math.Clamp(copiedBytes * 100L / totalBytes, 0, 100);
            progress?.Report(new BuildProgress(percent, status, copiedBytes, totalBytes));
        }

        try
        {
            Directory.CreateDirectory(stagingDirectory);
            foreach (string folder in new[] { "assets", "Net", "Temp", "User", "settings" })
            {
                Directory.CreateDirectory(Path.Combine(stagingDirectory, folder));
            }

            Report("Checking the APK…");
            using (ZipArchive apk = ZipFile.OpenRead(request.ApkPath))
            {
                foreach (string library in RequiredLibraries)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    ZipArchiveEntry entry = apk.GetEntry($"lib/armeabi-v7a/{library}")
                        ?? throw new InvalidDataException($"The APK is missing lib/armeabi-v7a/{library}.");
                    string destination = Path.Combine(stagingDirectory, library);
                    using Stream source = entry.Open();
                    await CopyStreamAsync(source, destination, entry.Length, bytes => copiedBytes += bytes, cancellationToken);
                    Report($"Extracted {library}");
                }

                foreach (ZipArchiveEntry entry in apk.Entries.Where(IsApkAsset))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    string relative = entry.FullName.Replace('/', Path.DirectorySeparatorChar);
                    string destination = SafeDestination(stagingDirectory, relative);
                    using Stream source = entry.Open();
                    await CopyStreamAsync(source, destination, entry.Length, bytes => copiedBytes += bytes, cancellationToken);
                    Report($"Extracted APK asset {entry.Name}");
                }
            }

            Report("Copying the main game data…");
            await CopyFileAsync(
                request.MainObbPath,
                Path.Combine(stagingDirectory, MainObbName),
                bytes => copiedBytes += bytes,
                cancellationToken);
            Report("Main OBB ready");

            await CopyFileAsync(
                request.PatchObbPath,
                Path.Combine(stagingDirectory, PatchObbName),
                bytes => copiedBytes += bytes,
                cancellationToken);
            Report("Patch OBB ready");

            int baseArchiveEntryCount = 0;
            if (!_allowSyntheticInputs)
            {
                string assetsDirectory = Path.Combine(stagingDirectory, "assets");
                TelltaleExtractionResult mainExtraction = await TelltaleObbExtractor.ExtractAsync(
                    Path.Combine(stagingDirectory, MainObbName),
                    assetsDirectory,
                    "main",
                    Report,
                    cancellationToken);
                ValidateBaseExtraction(
                    "main",
                    mainExtraction,
                    SupportedMainAssetCount,
                    SupportedMainExtractedBytes);
                copiedBytes += mainExtraction.DeclaredBytes;
                baseArchiveEntryCount += mainExtraction.FileCount;
                Report($"Main OBB extracted — {mainExtraction.FileCount} assets");

                TelltaleExtractionResult patchExtraction = await TelltaleObbExtractor.ExtractAsync(
                    Path.Combine(stagingDirectory, PatchObbName),
                    assetsDirectory,
                    "patch",
                    Report,
                    cancellationToken);
                ValidateBaseExtraction(
                    "patch",
                    patchExtraction,
                    SupportedPatchAssetCount,
                    SupportedPatchExtractedBytes);
                copiedBytes += patchExtraction.DeclaredBytes;
                baseArchiveEntryCount += patchExtraction.FileCount;
                VerifyExtractedBaseAssets(assetsDirectory);
                Report($"Patch OBB extracted — {patchExtraction.FileCount} assets");
            }

            // User-supplied fixes intentionally come after the base archives so they
            // can replace the original controller prompts instead of being overwritten.
            int buttonFixFileCount = 0;
            if (buttonFix is not null)
            {
                Report("Installing the controller button fix…");
                buttonFixFileCount = await CopyButtonFixAsync(
                    buttonFix,
                    Path.Combine(stagingDirectory, "assets"),
                    bytes => copiedBytes += bytes,
                    status => Report(status),
                    cancellationToken);
            }

            if (choiceData is not null)
            {
                Report("Installing offline choice statistics…");
                await CopyBundledChoiceDataAsync(
                    stagingDirectory,
                    bytes => copiedBytes += bytes,
                    cancellationToken);
            }

            foreach (ChapterSource source in request.ChapterSources)
            {
                cancellationToken.ThrowIfCancellationRequested();
                foreach (int episode in source.Episodes)
                {
                    includedEpisodes.Add(episode);
                }

                int copied = source.Kind switch
                {
                    ChapterSourceKind.Folder => await CopyChapterFolderAsync(
                        source,
                        Path.Combine(stagingDirectory, "assets"),
                        copiedNames,
                        bytes => copiedBytes += bytes,
                        status => Report(status),
                        cancellationToken),
                    ChapterSourceKind.ZipArchive => await CopyChapterZipAsync(
                        source,
                        Path.Combine(stagingDirectory, "assets"),
                        copiedNames,
                        bytes => copiedBytes += bytes,
                        status => Report(status),
                        cancellationToken),
                    ChapterSourceKind.ObbArchive => await CopyChapterZipAsync(
                        source,
                        Path.Combine(stagingDirectory, "assets"),
                        copiedNames,
                        bytes => copiedBytes += bytes,
                        status => Report(status),
                        cancellationToken),
                    _ => throw new ArgumentOutOfRangeException()
                };
                chapterFileCount += copied;
            }

            await WriteSettingsAsync(stagingDirectory, request, cancellationToken);
            (int dataAddonFileCount, int dataAddonOverwriteCount) = await CopyDataAddonsAsync(
                request.DataAddons,
                stagingDirectory,
                bytes => copiedBytes += bytes,
                status => Report(status),
                cancellationToken);
            await WriteReadyMarkerAsync(
                stagingDirectory,
                request,
                includedEpisodes,
                buttonFix,
                choiceData,
                baseArchiveEntryCount,
                dataAddonFileCount,
                dataAddonOverwriteCount,
                cancellationToken);
            VerifyOutput(
                stagingDirectory,
                includedEpisodes,
                buttonFix,
                choiceData,
                requireBaseAssets: !_allowSyntheticInputs);

            Report("Finalizing the ready-to-copy folder…");
            if (Directory.Exists(outputDirectory))
            {
                backupDirectory = NextBackupPath(parent);
                Directory.Move(outputDirectory, backupDirectory);
            }

            try
            {
                Directory.Move(stagingDirectory, outputDirectory);
            }
            catch
            {
                if (backupDirectory is not null
                    && Directory.Exists(backupDirectory)
                    && !Directory.Exists(outputDirectory))
                {
                    Directory.Move(backupDirectory, outputDirectory);
                    backupDirectory = null;
                }
                throw;
            }

            copiedBytes = totalBytes;
            progress?.Report(new BuildProgress(100, "Data folder ready", copiedBytes, totalBytes));
            return new BuildResult(
                outputDirectory,
                backupDirectory,
                includedEpisodes.ToList(),
                chapterFileCount,
                buttonFixFileCount,
                choiceData is not null,
                dataAddonFileCount,
                dataAddonOverwriteCount,
                totalBytes);
        }
        catch
        {
            if (Directory.Exists(stagingDirectory))
            {
                Directory.Delete(stagingDirectory, recursive: true);
            }
            throw;
        }
    }

    public static ApkLayout InspectApk(string apkPath) => InspectApkCore(apkPath, true);

    internal static ApkLayout InspectSyntheticApk(string apkPath) => InspectApkCore(apkPath, false);

    private static ApkLayout InspectApkCore(string apkPath, bool enforceCompatibilityFingerprint)
    {
        if (!File.Exists(apkPath))
        {
            throw new FileNotFoundException("APK not found.", apkPath);
        }
        if (!Path.GetExtension(apkPath).Equals(".apk", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Only Android .apk files are accepted for the game package.");
        }

        try
        {
            using ZipArchive apk = ZipFile.OpenRead(apkPath);
            ZipArchiveEntry manifestEntry = apk.GetEntry("AndroidManifest.xml")
                ?? throw new InvalidDataException("The APK has no AndroidManifest.xml.");
            ApkManifestInfo manifest;
            using (Stream manifestStream = manifestEntry.Open())
            {
                manifest = ApkManifestReader.Read(manifestStream);
            }
            if (!manifest.PackageName.Equals(SupportedPackageName, StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    $"This APK belongs to '{manifest.PackageName}', not Minecraft: Story Mode ({SupportedPackageName}).");
            }
            if (manifest.VersionCode != SupportedApkVersionCode)
            {
                string detectedName = string.IsNullOrWhiteSpace(manifest.VersionName)
                    ? $"versionCode {manifest.VersionCode}"
                    : $"v{manifest.VersionName} (versionCode {manifest.VersionCode})";
                throw new InvalidDataException(
                    $"Unsupported APK version: {detectedName}. " +
                    $"This port requires the newest supported PowerVR APK: v{SupportedApkVersionName} " +
                    $"(versionCode {SupportedApkVersionCode}).");
            }

            if (enforceCompatibilityFingerprint)
            {
                VerifySupportedFingerprint(
                    apkPath,
                    SupportedApkSha256,
                    "This is v1.37, but it is not the supported PowerVR APK. " +
                    "Choose the PowerVR build used by this port; Mali and Adreno APKs are not accepted.");
            }

            var missing = RequiredLibraries
                .Where(library => apk.GetEntry($"lib/armeabi-v7a/{library}") is null)
                .ToList();
            if (missing.Count > 0)
            {
                throw new InvalidDataException(
                    "This is not the required 32-bit PowerVR APK. Missing: " + string.Join(", ", missing));
            }

            long libraryBytes = RequiredLibraries
                .Select(library => apk.GetEntry($"lib/armeabi-v7a/{library}")!.Length)
                .Sum();
            var assets = apk.Entries.Where(IsApkAsset).ToList();
            return new ApkLayout(
                manifest.PackageName,
                manifest.VersionCode,
                manifest.VersionName,
                RequiredLibraries.Length,
                assets.Count,
                libraryBytes + assets.Sum(entry => entry.Length));
        }
        catch (InvalidDataException)
        {
            throw;
        }
        catch (Exception exception)
        {
            throw new InvalidDataException("The selected APK could not be opened as an Android package.", exception);
        }
    }

    public static ObbLayout InspectMainObb(string mainObbPath) => InspectMainObbCore(mainObbPath, true, true);

    internal static ObbLayout InspectSupportedMainObbStandalone(string mainObbPath) =>
        InspectMainObbCore(mainObbPath, true, false);

    internal static ObbLayout InspectSyntheticMainObb(string mainObbPath) =>
        InspectMainObbCore(mainObbPath, false, true);

    private static ObbLayout InspectMainObbCore(
        string mainObbPath,
        bool enforceCompatibilityFingerprint,
        bool findNeighboringPatch)
    {
        if (!File.Exists(mainObbPath))
        {
            throw new FileNotFoundException("Main OBB not found.", mainObbPath);
        }
        if (!Path.GetExtension(mainObbPath).Equals(".obb", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Only Android .obb files are accepted for the main expansion data.");
        }

        string fileName = Path.GetFileName(mainObbPath);
        if (fileName.StartsWith("patch.", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "That is a patch OBB. Choose the larger main.*.obb file instead; a matching patch is found automatically.");
        }

        long bytes = new FileInfo(mainObbPath).Length;
        if (bytes < 1024 * 1024)
        {
            throw new InvalidDataException("The selected main OBB is unexpectedly small and cannot be used.");
        }

        if (enforceCompatibilityFingerprint)
        {
            VerifySupportedFingerprint(
                mainObbPath,
                SupportedMainObbSha256,
                "This is not the supported PowerVR main OBB. Choose the exact main expansion file used by this port.");
        }

        return new ObbLayout(bytes, findNeighboringPatch ? FindPatchObb(mainObbPath) : null);
    }

    public static ObbLayout InspectPatchObb(string patchObbPath) => InspectPatchObbCore(patchObbPath, true);

    internal static ObbLayout InspectSyntheticPatchObb(string patchObbPath) => InspectPatchObbCore(patchObbPath, false);

    private static ObbLayout InspectPatchObbCore(string patchObbPath, bool enforceCompatibilityFingerprint)
    {
        if (!File.Exists(patchObbPath))
        {
            throw new FileNotFoundException("Patch OBB not found.", patchObbPath);
        }
        if (!Path.GetExtension(patchObbPath).Equals(".obb", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Only Android .obb files are accepted for the patch expansion data.");
        }

        string fileName = Path.GetFileName(patchObbPath);
        if (fileName.StartsWith("main.", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("That looks like the main OBB. Choose the separate patch.*.obb file here.");
        }

        long bytes = new FileInfo(patchObbPath).Length;
        if (bytes <= 0)
        {
            throw new InvalidDataException("The selected patch OBB is empty and cannot be used.");
        }

        if (enforceCompatibilityFingerprint)
        {
            VerifySupportedFingerprint(
                patchObbPath,
                SupportedPatchObbSha256,
                "This is not the supported PowerVR patch OBB. Choose the exact patch expansion file used by this port.");
        }

        return new ObbLayout(bytes, null);
    }

    private static void VerifySupportedFingerprint(string path, string expectedSha256, string errorMessage)
    {
        using FileStream stream = new(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            1024 * 1024,
            FileOptions.SequentialScan);
        string hash = Convert.ToHexString(SHA256.HashData(stream));
        if (!hash.Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(errorMessage);
        }
    }

    public static ButtonFixBundle? InspectBundledButtonFix()
    {
        Assembly assembly = typeof(DataBuilderService).Assembly;
        string? resourceName = assembly.GetManifestResourceNames()
            .SingleOrDefault(name => name.EndsWith("LocalAssets.button-fix.zip", StringComparison.OrdinalIgnoreCase));
        if (resourceName is null)
        {
            return null;
        }

        using Stream stream = assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidDataException("The embedded controller button-fix package could not be opened.");
        return InspectButtonFixArchive(stream, ButtonFixSourceKind.Embedded, null);
    }

    public static BundledAsset? InspectBundledChoiceData()
    {
        Assembly assembly = typeof(DataBuilderService).Assembly;
        string? resourceName = assembly.GetManifestResourceNames()
            .SingleOrDefault(name => name.EndsWith("LocalAssets.choice.prop", StringComparison.OrdinalIgnoreCase));
        if (resourceName is null)
        {
            return null;
        }

        using Stream stream = assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidDataException("The embedded offline choice dataset could not be opened.");
        if (stream.Length <= 0)
        {
            throw new InvalidDataException("The embedded offline choice dataset is empty.");
        }
        string hash = Convert.ToHexString(SHA256.HashData(stream));
        if (!hash.Equals(SupportedChoiceDataSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The embedded choice.prop is not the supported offline crowd-choice dataset.");
        }
        return new BundledAsset("choice.prop", stream.Length);
    }

    public static ButtonFixBundle? ResolveButtonFix(string? selectedPath)
    {
        return string.IsNullOrWhiteSpace(selectedPath)
            ? InspectBundledButtonFix()
            : InspectButtonFixSource(selectedPath);
    }

    public static ButtonFixBundle InspectButtonFixSource(string path)
    {
        if (Directory.Exists(path))
        {
            var assets = Directory.EnumerateFiles(path, "*", SearchOption.AllDirectories)
                .Where(IsButtonAsset)
                .Select(file => new BundledAsset(Path.GetFileName(file), new FileInfo(file).Length))
                .ToList();
            return CreateButtonFixBundle(
                assets,
                ButtonFixSourceKind.Folder,
                Path.GetFullPath(path));
        }

        if (File.Exists(path) && Path.GetExtension(path).Equals(".zip", StringComparison.OrdinalIgnoreCase))
        {
            using FileStream stream = File.OpenRead(path);
            return InspectButtonFixArchive(
                stream,
                ButtonFixSourceKind.ZipArchive,
                Path.GetFullPath(path));
        }

        throw new InvalidDataException("Choose the controller button-fix folder or a .zip made from it.");
    }

    private static ButtonFixBundle InspectButtonFixArchive(
        Stream stream,
        ButtonFixSourceKind kind,
        string? path)
    {
        using ZipArchive zip = new(stream, ZipArchiveMode.Read, leaveOpen: true);
        var assets = zip.Entries
            .Where(entry => !string.IsNullOrEmpty(entry.Name) && IsButtonAsset(entry.Name))
            .Select(entry => new BundledAsset(entry.Name, entry.Length))
            .ToList();
        return CreateButtonFixBundle(assets, kind, path);
    }

    private static ButtonFixBundle CreateButtonFixBundle(
        IReadOnlyList<BundledAsset> assets,
        ButtonFixSourceKind kind,
        string? path)
    {
        if (assets.Count == 0)
        {
            throw new InvalidDataException("The controller button-fix source contains no .d3dtx or .d3dmesh assets.");
        }

        string? duplicate = assets
            .GroupBy(asset => asset.Name, StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault(group => group.Count() > 1)
            ?.Key;
        if (duplicate is not null)
        {
            throw new InvalidDataException($"The controller fix contains two files named '{duplicate}'.");
        }

        BundledAsset? empty = assets.FirstOrDefault(asset => asset.Size <= 0);
        if (empty is not null)
        {
            throw new InvalidDataException($"The controller fix contains an empty file: {empty.Name}");
        }

        string[] missingMeshes = RequiredButtonMeshes
            .Where(required => !assets.Any(asset => asset.Name.Equals(required, StringComparison.OrdinalIgnoreCase)))
            .ToArray();
        if (missingMeshes.Length > 0)
        {
            throw new InvalidDataException(
                "The selected folder is not the complete controller button fix. Missing: " +
                string.Join(", ", missingMeshes));
        }

        return new ButtonFixBundle(assets, assets.Sum(asset => asset.Size), kind, path);
    }

    public static string? FindPatchObb(string mainObbPath)
    {
        string? folder = Path.GetDirectoryName(Path.GetFullPath(mainObbPath));
        if (folder is null || !Directory.Exists(folder))
        {
            return null;
        }

        string canonical = Path.Combine(folder, PatchObbName);
        if (File.Exists(canonical))
        {
            return canonical;
        }

        string[] candidates = Directory.GetFiles(folder, "patch.*.obb", SearchOption.TopDirectoryOnly);
        return candidates.Length switch
        {
            0 => null,
            1 => candidates[0],
            _ => throw new InvalidDataException(
                "More than one patch OBB is beside the main OBB. Keep only the matching patch there, then try again.")
        };
    }

    private static void ValidateRequest(BuildRequest request)
    {
        if (!File.Exists(request.ApkPath))
        {
            throw new FileNotFoundException("Choose the game's APK first.", request.ApkPath);
        }
        if (!Path.GetExtension(request.ApkPath).Equals(".apk", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("The first file must be an .apk.");
        }
        if (!File.Exists(request.MainObbPath))
        {
            throw new FileNotFoundException("Choose the main OBB first.", request.MainObbPath);
        }
        if (!Path.GetExtension(request.MainObbPath).Equals(".obb", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("The second file must be a main .obb.");
        }
        if (!File.Exists(request.PatchObbPath))
        {
            throw new FileNotFoundException("Choose the patch OBB first.", request.PatchObbPath);
        }
        if (!Path.GetExtension(request.PatchObbPath).Equals(".obb", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("The third file must be a patch .obb.");
        }
        if (Path.GetFullPath(request.MainObbPath)
            .Equals(Path.GetFullPath(request.PatchObbPath), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Main OBB and patch OBB must be two different files.");
        }
        if (string.IsNullOrWhiteSpace(request.OutputDirectory))
        {
            throw new InvalidDataException("Choose where to create the mcsm folder.");
        }
        if (!new[] { "performance", "balanced", "quality", "battery", "custom" }
            .Contains(request.GraphicsProfile))
        {
            throw new InvalidDataException("Choose a valid graphics profile.");
        }
        ValidateCustomProfile(request.CustomProfile ?? new CustomProfileSettings());
        if (!new[] { "en", "fr", "de", "es", "pt", "ru", "zh" }.Contains(request.LanguageCode))
        {
            throw new InvalidDataException("Choose a valid language.");
        }
    }

    private static void ValidateCustomProfile(CustomProfileSettings custom)
    {
        RequireOption(custom.Mode, ["easy", "advanced"], "custom mode");
        RequireOption(custom.Picture, ["low", "battery", "fast", "sharp", "quality", "native"], "custom picture");
        RequireOption(custom.Motion, ["low", "steady", "smooth"], "custom motion");
        RequireOption(custom.Gpu, ["fastest", "fast", "medium", "quality", "original"], "custom GPU");
        RequireOption(custom.Effects, ["minimal", "outlines", "full"], "custom effects");
        RequireOption(custom.World, ["fast", "balanced", "detailed", "unlimited"], "custom world");
        RequireOption(custom.Power, ["battery", "performance"], "custom power");

        RequireOption(custom.Resolution, ["960x544", "800x452", "720x408", "640x362", "576x326", "480x272"], "advanced resolution");
        if (custom.FpsCap is not (60 or 30 or 20 or 15))
        {
            throw new InvalidDataException("Custom FPS cap must be 60, 30, 20, or 15.");
        }
        RequireOption(custom.AdvancedGpu, ["sgx540", "sgx541", "sgx542", "sgx543", "sgx543mp"], "advanced GPU");
        RequireOnOff(custom.Outlines, "outlines");
        RequireOnOff(custom.Shadows, "shadows");
        RequireOnOff(custom.Vsync, "VSync");
        RequireOnOff(custom.NearestFilter, "seam fix");
        RequireOnOff(custom.FbfetchZero, "glass/light fix");
        RequireOption(custom.Clock, ["444", "adaptive"], "clock");
        RequireOption(custom.Upscale, ["linear", "nearest"], "upscale filter");
        if (custom.Detail is < 100 or > 1000)
        {
            throw new InvalidDataException("Custom detail must be between 100 and 1000.");
        }
        if (custom.DrawDistance != 0 && custom.DrawDistance is < 2500 or > 6000)
        {
            throw new InvalidDataException("Custom draw distance must be 0 (unlimited) or between 2500 and 6000.");
        }
    }

    private static void RequireOnOff(string value, string label) =>
        RequireOption(value, ["on", "off"], label);

    private static void RequireOption(string value, IReadOnlyCollection<string> allowed, string label)
    {
        if (!allowed.Contains(value, StringComparer.Ordinal))
        {
            throw new InvalidDataException($"Choose a valid {label} value.");
        }
    }

    private static async Task<int> CopyChapterFolderAsync(
        ChapterSource source,
        string assetsDirectory,
        IDictionary<string, string> copiedNames,
        Action<int> addBytes,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        int copied = 0;
        foreach (string file in Directory.EnumerateFiles(source.Path, "*", SearchOption.AllDirectories))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (ChapterScanner.IsChapterPackage(file))
            {
                try
                {
                    copied += await CopyChapterArchiveFileAsync(
                        file,
                        assetsDirectory,
                        copiedNames,
                        addBytes,
                        report,
                        cancellationToken);
                }
                catch (InvalidDataException)
                {
                    // Full setup folders may also contain the raw base OBBs.
                }
                continue;
            }
            if (!ChapterScanner.IsChapterFile(file))
            {
                continue;
            }
            string fileName = Path.GetFileName(file);
            await using FileStream input = new(
                file, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            bool written = await CopyChapterStreamAsync(
                input,
                input.Length,
                fileName,
                file,
                assetsDirectory,
                copiedNames,
                addBytes,
                cancellationToken);
            if (written)
            {
                copied++;
                report($"Added chapter file {fileName}");
            }
        }
        return copied;
    }

    private static async Task<int> CopyButtonFixAsync(
        ButtonFixBundle bundle,
        string assetsDirectory,
        Action<int> addBytes,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        if (bundle.Kind == ButtonFixSourceKind.Folder)
        {
            int copiedFolderFiles = 0;
            foreach (string file in Directory.EnumerateFiles(bundle.Path!, "*", SearchOption.AllDirectories)
                         .Where(IsButtonAsset))
            {
                cancellationToken.ThrowIfCancellationRequested();
                string name = Path.GetFileName(file);
                await CopyFileAsync(
                    file,
                    Path.Combine(assetsDirectory, name),
                    addBytes,
                    cancellationToken);
                copiedFolderFiles++;
                report($"Installed controller asset {name}");
            }
            return copiedFolderFiles;
        }

        Stream stream;
        if (bundle.Kind == ButtonFixSourceKind.Embedded)
        {
            Assembly assembly = typeof(DataBuilderService).Assembly;
            string resourceName = assembly.GetManifestResourceNames()
                .Single(name => name.EndsWith("LocalAssets.button-fix.zip", StringComparison.OrdinalIgnoreCase));
            stream = assembly.GetManifestResourceStream(resourceName)
                ?? throw new InvalidDataException("The embedded controller button-fix package could not be opened.");
        }
        else
        {
            stream = File.OpenRead(bundle.Path!);
        }

        await using (stream)
        using (ZipArchive zip = new(stream, ZipArchiveMode.Read, leaveOpen: false))
        {
            int copied = 0;
            foreach (ZipArchiveEntry entry in zip.Entries
                         .Where(entry => !string.IsNullOrEmpty(entry.Name) && IsButtonAsset(entry.Name)))
            {
                cancellationToken.ThrowIfCancellationRequested();
                BundledAsset expected = bundle.Assets.Single(asset =>
                    asset.Name.Equals(entry.Name, StringComparison.OrdinalIgnoreCase));
                string destination = Path.Combine(assetsDirectory, expected.Name);
                using Stream input = entry.Open();
                await CopyStreamAsync(input, destination, expected.Size, addBytes, cancellationToken);
                copied++;
                report($"Installed controller asset {expected.Name}");
            }
            return copied;
        }
    }

    private static async Task CopyBundledChoiceDataAsync(
        string stagingDirectory,
        Action<int> addBytes,
        CancellationToken cancellationToken)
    {
        Assembly assembly = typeof(DataBuilderService).Assembly;
        string resourceName = assembly.GetManifestResourceNames()
            .Single(name => name.EndsWith("LocalAssets.choice.prop", StringComparison.OrdinalIgnoreCase));
        await using Stream stream = assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidDataException("The embedded offline choice dataset could not be opened.");
        using MemoryStream memory = new();
        await stream.CopyToAsync(memory, cancellationToken);
        byte[] data = memory.ToArray();

        foreach (string relative in new[] { "choice.prop", Path.Combine("Temp", "choice.prop") })
        {
            string destination = Path.Combine(stagingDirectory, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            await File.WriteAllBytesAsync(destination, data, cancellationToken);
            addBytes(data.Length);
        }
    }

    private static async Task<int> CopyChapterZipAsync(
        ChapterSource source,
        string assetsDirectory,
        IDictionary<string, string> copiedNames,
        Action<int> addBytes,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        return await CopyChapterArchiveFileAsync(
            source.Path,
            assetsDirectory,
            copiedNames,
            addBytes,
            report,
            cancellationToken);
    }

    private static async Task<int> CopyChapterArchiveFileAsync(
        string archivePath,
        string assetsDirectory,
        IDictionary<string, string> copiedNames,
        Action<int> addBytes,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        string temporaryDirectory = Path.Combine(
            Path.GetTempPath(),
            $"mcsm-chapter-build-{Guid.NewGuid():N}");
        Directory.CreateDirectory(temporaryDirectory);
        try
        {
            return await CopyChapterArchiveRecursiveAsync(
                archivePath,
                archivePath,
                temporaryDirectory,
                0,
                false,
                assetsDirectory,
                copiedNames,
                addBytes,
                report,
                cancellationToken);
        }
        finally
        {
            string fullTemporaryDirectory = Path.GetFullPath(temporaryDirectory);
            string tempRoot = Path.GetFullPath(Path.GetTempPath());
            if (Directory.Exists(fullTemporaryDirectory)
                && fullTemporaryDirectory.StartsWith(tempRoot, StringComparison.OrdinalIgnoreCase)
                && Path.GetFileName(fullTemporaryDirectory).StartsWith("mcsm-chapter-build-", StringComparison.Ordinal))
            {
                Directory.Delete(fullTemporaryDirectory, recursive: true);
            }
        }
    }

    private static async Task<int> CopyChapterArchiveRecursiveAsync(
        string archivePath,
        string sourceLabel,
        string temporaryDirectory,
        int depth,
        bool ignoreInvalidArchive,
        string assetsDirectory,
        IDictionary<string, string> copiedNames,
        Action<int> addBytes,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        if (depth > 4)
        {
            throw new InvalidDataException("Chapter packages may be nested no more than 4 levels deep.");
        }

        ZipArchive zip;
        try
        {
            zip = ZipFile.OpenRead(archivePath);
        }
        catch (InvalidDataException) when (ignoreInvalidArchive)
        {
            return 0;
        }

        int copied = 0;
        int nestedIndex = 0;
        using (zip)
        {
        foreach (ZipArchiveEntry entry in zip.Entries.Where(entry => !string.IsNullOrEmpty(entry.Name)))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (ChapterScanner.IsChapterFile(entry.FullName))
            {
                using Stream input = entry.Open();
                bool written = await CopyChapterStreamAsync(
                    input,
                    entry.Length,
                    entry.Name,
                    $"{sourceLabel}!/{entry.FullName}",
                    assetsDirectory,
                    copiedNames,
                    addBytes,
                    cancellationToken);
                if (written)
                {
                    copied++;
                    report($"Added chapter file {entry.Name}");
                }
                continue;
            }

            if (!ChapterScanner.IsChapterPackage(entry.Name))
            {
                continue;
            }
            if (depth >= 4)
            {
                throw new InvalidDataException("Chapter packages may be nested no more than 4 levels deep.");
            }

            string nestedPath = Path.Combine(
                temporaryDirectory,
                $"{depth:D2}-{nestedIndex++:D4}-{Guid.NewGuid():N}{Path.GetExtension(entry.Name).ToLowerInvariant()}");
            await using (FileStream output = new(
                             nestedPath,
                             FileMode.CreateNew,
                             FileAccess.Write,
                             FileShare.None,
                             1024 * 1024,
                             FileOptions.Asynchronous | FileOptions.SequentialScan))
            using (Stream input = entry.Open())
            {
                await input.CopyToAsync(output, 1024 * 1024, cancellationToken);
            }

            copied += await CopyChapterArchiveRecursiveAsync(
                nestedPath,
                $"{sourceLabel}!/{entry.FullName}",
                temporaryDirectory,
                depth + 1,
                true,
                assetsDirectory,
                copiedNames,
                addBytes,
                report,
                cancellationToken);
        }
        }
        return copied;
    }

    private static async Task<bool> CopyChapterStreamAsync(
        Stream input,
        long length,
        string fileName,
        string sourceLabel,
        string assetsDirectory,
        IDictionary<string, string> copiedNames,
        Action<int> addBytes,
        CancellationToken cancellationToken)
    {
        string destination = Path.Combine(assetsDirectory, Path.GetFileName(fileName));
        if (File.Exists(destination))
        {
            if (new FileInfo(destination).Length != length)
            {
                copiedNames.TryGetValue(fileName, out string? firstSource);
                throw ChapterConflict(fileName, firstSource, sourceLabel);
            }

            byte[] incomingHash = await SHA256.HashDataAsync(input, cancellationToken);
            await using FileStream existing = File.OpenRead(destination);
            byte[] existingHash = await SHA256.HashDataAsync(existing, cancellationToken);
            if (!incomingHash.AsSpan().SequenceEqual(existingHash))
            {
                copiedNames.TryGetValue(fileName, out string? firstSource);
                throw ChapterConflict(fileName, firstSource, sourceLabel);
            }
            return false;
        }

        await CopyStreamAsync(input, destination, length, addBytes, cancellationToken);
        copiedNames[fileName] = sourceLabel;
        return true;
    }

    private static InvalidDataException ChapterConflict(string fileName, string? first, string second) =>
        new(
            $"Two selected chapter sources contain different versions of '{fileName}'. " +
            $"Remove one source and build again.\n\nFirst: {first ?? "APK assets"}\nSecond: {second}");

    private static async Task<(int Files, int Overwrites)> CopyDataAddonsAsync(
        IReadOnlyList<DataAddonSource> addons,
        string stagingDirectory,
        Action<int> addBytes,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        int files = 0;
        int overwrites = 0;
        foreach (DataAddonSource addon in addons)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (addon.Kind == DataAddonSourceKind.Folder)
            {
                string root = DataAddonScanner.NormalizeFolderRoot(addon.Path);
                foreach (string source in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    string relative = DataAddonScanner.NormalizeRelativePath(Path.GetRelativePath(root, source));
                    DataAddonScanner.EnsureNotCritical(relative);
                    string destination = SafeDestination(stagingDirectory, relative);
                    if (File.Exists(destination)) overwrites++;
                    await CopyFileAsync(source, destination, addBytes, cancellationToken);
                    files++;
                    report($"Applied data add-on file {relative}");
                }
            }
            else
            {
                using ZipArchive zip = ZipFile.OpenRead(addon.Path);
                foreach (ZipArchiveEntry entry in zip.Entries.Where(entry => !string.IsNullOrEmpty(entry.Name)))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    string relative = DataAddonScanner.NormalizeArchivePath(entry.FullName);
                    DataAddonScanner.EnsureNotCritical(relative);
                    string destination = SafeDestination(stagingDirectory, relative);
                    if (File.Exists(destination)) overwrites++;
                    using Stream input = entry.Open();
                    await CopyStreamAsync(input, destination, entry.Length, addBytes, cancellationToken);
                    files++;
                    report($"Applied data add-on file {relative}");
                }
            }
        }
        return (files, overwrites);
    }

    private static async Task WriteSettingsAsync(
        string stagingDirectory,
        BuildRequest request,
        CancellationToken cancellationToken)
    {
        string graphics = await ReadEmbeddedTextAsync("graphics.txt", cancellationToken);
        graphics = ReplaceSettingValue(graphics, "profile", request.GraphicsProfile);

        CustomProfileSettings custom = request.CustomProfile ?? new CustomProfileSettings();
        var customValues = new Dictionary<string, string>
        {
            ["custom_mode"] = custom.Mode,
            ["custom_picture"] = custom.Picture,
            ["custom_motion"] = custom.Motion,
            ["custom_gpu"] = custom.Gpu,
            ["custom_effects"] = custom.Effects,
            ["custom_world"] = custom.World,
            ["custom_power"] = custom.Power,
            ["advanced_resolution"] = custom.Resolution,
            ["advanced_fps_cap"] = custom.FpsCap.ToString(),
            ["advanced_gpu"] = custom.AdvancedGpu,
            ["advanced_outlines"] = custom.Outlines,
            ["advanced_shadows"] = custom.Shadows,
            ["advanced_detail"] = custom.Detail.ToString(),
            ["advanced_draw_distance"] = custom.DrawDistance.ToString(),
            ["advanced_clock"] = custom.Clock,
            ["advanced_upscale"] = custom.Upscale,
            ["advanced_vsync"] = custom.Vsync,
            ["advanced_nearest_filter"] = custom.NearestFilter,
            ["advanced_fbfetch_zero"] = custom.FbfetchZero
        };
        foreach ((string key, string value) in customValues)
        {
            graphics = ReplaceSettingValue(graphics, key, value);
        }

        string game = await ReadEmbeddedTextAsync("game.txt", cancellationToken);
        game = Regex.Replace(
            game,
            @"(?m)^language\s*=\s*\S+",
            $"language = {request.LanguageCode}",
            RegexOptions.CultureInvariant);

        string settings = Path.Combine(stagingDirectory, "settings");
        await File.WriteAllTextAsync(Path.Combine(settings, "graphics.txt"), graphics, new UTF8Encoding(false), cancellationToken);
        await File.WriteAllTextAsync(Path.Combine(settings, "game.txt"), game, new UTF8Encoding(false), cancellationToken);
    }

    private static async Task WriteReadyMarkerAsync(
        string stagingDirectory,
        BuildRequest request,
        IEnumerable<int> includedEpisodes,
        ButtonFixBundle? buttonFix,
        BundledAsset? choiceData,
        int baseArchiveEntryCount,
        int dataAddonFileCount,
        int dataAddonOverwriteCount,
        CancellationToken cancellationToken)
    {
        string episodeText = string.Join(", ", includedEpisodes.Order());
        string readyText =
            "MCSM VITA DATA FOLDER — READY\r\n" +
            "================================\r\n\r\n" +
            "Copy this entire mcsm folder to ux0:data\\ on the PS Vita.\r\n" +
            "The final Vita path must be: ux0:data\\mcsm\\assets\r\n\r\n" +
            $"Detected episodes: {episodeText}\r\n" +
            $"Base OBBs: main + patch included and extracted ({baseArchiveEntryCount} archive entries)\r\n" +
            $"Graphics profile: {FormatGraphicsProfile(request)}\r\n" +
            $"Language: {request.LanguageCode}\r\n" +
            $"Controller button fix: {(buttonFix is null ? "not supplied" : $"included ({buttonFix.FileCount} assets)")}\r\n" +
            $"Offline choice statistics: {(choiceData is null ? "not supplied" : "included")}\r\n" +
            $"Experimental data add-ons: {dataAddonFileCount} files ({dataAddonOverwriteCount} replacements)\r\n\r\n" +
            "Advanced graphics and episode controls are in the settings folder.\r\n" +
            "Data add-on / mod installation is experimental and has not been tested on Vita.\r\n" +
            "This folder was built from files you selected; no game data is included with the tool.\r\n";
        await File.WriteAllTextAsync(
            Path.Combine(stagingDirectory, "DATA_FOLDER_READY.txt"),
            readyText,
            new UTF8Encoding(false),
            cancellationToken);
    }

    private static string ReplaceSettingValue(string text, string key, string value) =>
        Regex.Replace(
            text,
            $@"(?m)^({Regex.Escape(key)}\s*=\s*)\S+",
            match => match.Groups[1].Value + value,
            RegexOptions.CultureInvariant);

    private static string FormatGraphicsProfile(BuildRequest request)
    {
        if (!request.GraphicsProfile.Equals("custom", StringComparison.Ordinal))
        {
            return request.GraphicsProfile;
        }
        CustomProfileSettings custom = request.CustomProfile ?? new CustomProfileSettings();
        return $"custom ({custom.Summary})";
    }

    private static async Task<string> ReadEmbeddedTextAsync(string fileName, CancellationToken cancellationToken)
    {
        Assembly assembly = typeof(DataBuilderService).Assembly;
        string resourceName = assembly.GetManifestResourceNames()
            .Single(name => name.EndsWith($"Defaults.{fileName}", StringComparison.OrdinalIgnoreCase));
        await using Stream stream = assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidOperationException($"Embedded default '{fileName}' is missing.");
        using StreamReader reader = new(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        return await reader.ReadToEndAsync(cancellationToken);
    }

    private static void ValidateBaseExtraction(
        string label,
        TelltaleExtractionResult result,
        int expectedFiles,
        long expectedBytes)
    {
        if (result.FileCount != expectedFiles || result.DeclaredBytes != expectedBytes)
        {
            throw new InvalidDataException(
                $"The supported {label} OBB extracted an unexpected asset set " +
                $"({result.FileCount} files / {FormatBytes(result.DeclaredBytes)}; " +
                $"expected {expectedFiles} / {FormatBytes(expectedBytes)}). " +
                "The incomplete folder was not finalized.");
        }
    }

    private static void VerifyExtractedBaseAssets(string assetsDirectory)
    {
        FileInfo[] baseAssets = new DirectoryInfo(assetsDirectory)
            .EnumerateFiles("*", SearchOption.TopDirectoryOnly)
            .Where(file =>
                file.Extension.Equals(".ttarch2", StringComparison.OrdinalIgnoreCase)
                || file.Extension.Equals(".lua", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        long baseBytes = baseAssets.Sum(file => file.Length);

        if (baseAssets.Length != SupportedActiveBaseAssetCount
            || baseBytes != SupportedActiveBaseAssetBytes)
        {
            throw new InvalidDataException(
                "Base-game extraction is incomplete: " +
                $"found {baseAssets.Length} active archives/descriptors totaling {FormatBytes(baseBytes)}, " +
                $"expected {SupportedActiveBaseAssetCount} totaling {FormatBytes(SupportedActiveBaseAssetBytes)}. " +
                "The folder would black-screen on Vita, so it was not finalized.");
        }

        foreach (string assetName in RequiredBaseAssets)
        {
            string path = Path.Combine(assetsDirectory, assetName);
            if (!File.Exists(path) || new FileInfo(path).Length <= 0)
            {
                throw new InvalidDataException(
                    $"Base-game extraction is missing boot-critical asset '{assetName}'.");
            }
        }
    }

    private static void VerifyOutput(
        string output,
        IEnumerable<int> episodes,
        ButtonFixBundle? buttonFix,
        BundledAsset? choiceData,
        bool requireBaseAssets)
    {
        var required = RequiredLibraries
            .Select(library => Path.Combine(output, library))
            .Append(Path.Combine(output, MainObbName))
            .Append(Path.Combine(output, "settings", "graphics.txt"))
            .Append(Path.Combine(output, "settings", "game.txt"));

        string? missing = required.FirstOrDefault(path => !File.Exists(path) || new FileInfo(path).Length == 0);
        if (missing is not null)
        {
            throw new IOException($"Build verification failed: '{missing}' is missing or empty.");
        }

        if (requireBaseAssets)
        {
            foreach (string assetName in RequiredBaseAssets)
            {
                string assetPath = Path.Combine(output, "assets", assetName);
                if (!File.Exists(assetPath) || new FileInfo(assetPath).Length <= 0)
                {
                    throw new IOException(
                        $"Base-game extraction verification failed: '{assetName}' is missing. " +
                        "The folder would black-screen on Vita, so it was not finalized.");
                }
            }
        }

        foreach (int episode in episodes.Where(episode => episode >= 2))
        {
            string marker = Path.Combine(output, "assets", $"MCSM_android_Minecraft10{episode}_data.ttarch2");
            if (!File.Exists(marker))
            {
                throw new InvalidDataException(
                    $"Episode {episode} was detected, but its required marker archive is missing: {Path.GetFileName(marker)}");
            }
        }


        if (buttonFix is not null)
        {
            foreach (BundledAsset asset in buttonFix.Assets)
            {
                string path = Path.Combine(output, "assets", asset.Name);
                if (!File.Exists(path) || new FileInfo(path).Length <= 0)
                {
                    throw new IOException(
                        $"Controller button-fix verification failed: '{asset.Name}' is missing or incomplete.");
                }
            }
        }

        if (choiceData is not null)
        {
            string rootChoice = Path.Combine(output, "choice.prop");
            string tempChoice = Path.Combine(output, "Temp", "choice.prop");
            foreach (string path in new[] { rootChoice, tempChoice })
            {
                if (!File.Exists(path) || new FileInfo(path).Length <= 0)
                {
                    throw new IOException("Offline choice-statistics verification failed: choice.prop is missing.");
                }

                using FileStream stream = File.OpenRead(path);
                string hash = Convert.ToHexString(SHA256.HashData(stream));
                if (!hash.Equals(SupportedChoiceDataSha256, StringComparison.OrdinalIgnoreCase))
                {
                    throw new IOException("Offline choice-statistics verification failed: choice.prop is invalid.");
                }
            }
        }
    }

    private static string NextBackupPath(string parent)
    {
        string basePath = Path.Combine(parent, $"mcsm-backup-{DateTime.Now:yyyyMMdd-HHmmss}");
        string candidate = basePath;
        int suffix = 2;
        while (Directory.Exists(candidate))
        {
            candidate = $"{basePath}-{suffix++}";
        }
        return candidate;
    }

    private static void EnsureFreeSpace(string outputPath, long inputBytes)
    {
        string root = Path.GetPathRoot(Path.GetFullPath(outputPath))
            ?? throw new InvalidDataException("Could not determine the output drive.");
        long required = inputBytes + 256L * 1024 * 1024;
        long available = new DriveInfo(root).AvailableFreeSpace;
        if (available < required)
        {
            throw new IOException(
                $"Not enough free space. Need about {FormatBytes(required)}, but {FormatBytes(available)} is available.");
        }
    }

    private static bool IsApkAsset(ZipArchiveEntry entry) =>
        !string.IsNullOrEmpty(entry.Name)
        && entry.FullName.StartsWith("assets/", StringComparison.OrdinalIgnoreCase);

    private static bool IsButtonAsset(string path)
    {
        string extension = Path.GetExtension(path);
        return extension.Equals(".d3dtx", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".d3dmesh", StringComparison.OrdinalIgnoreCase);
    }

    private static string SafeDestination(string root, string relativePath)
    {
        string fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        string destination = Path.GetFullPath(Path.Combine(root, relativePath));
        if (!destination.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException($"Unsafe path inside APK: {relativePath}");
        }
        return destination;
    }

    private static async Task CopyFileAsync(
        string source,
        string destination,
        Action<int> addBytes,
        CancellationToken cancellationToken)
    {
        await using FileStream input = new(
            source, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        await CopyStreamAsync(input, destination, input.Length, addBytes, cancellationToken);
    }

    private static async Task CopyStreamAsync(
        Stream input,
        string destination,
        long expectedLength,
        Action<int> addBytes,
        CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        await using FileStream output = new(
            destination, FileMode.Create, FileAccess.Write, FileShare.None, 1024 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        byte[] buffer = new byte[1024 * 1024];
        long written = 0;
        while (true)
        {
            int read = await input.ReadAsync(buffer, cancellationToken);
            if (read == 0)
            {
                break;
            }
            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
            written += read;
            addBytes(read);
        }
        await output.FlushAsync(cancellationToken);
        if (written != expectedLength)
        {
            throw new IOException($"Copy verification failed for {Path.GetFileName(destination)}.");
        }
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
}

public sealed record ApkLayout(
    string PackageName,
    int VersionCode,
    string VersionName,
    int LibraryCount,
    int AssetCount,
    long TotalBytes);

public sealed record ObbLayout(long TotalBytes, string? PatchPath);
