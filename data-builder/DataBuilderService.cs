using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace McsmVitaDataBuilder;

public sealed class DataBuilderService
{
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

    private static readonly string[] RequiredButtonMeshes =
    [
        "ui_action_promptFacebuttonDown.d3dmesh",
        "ui_action_promptFacebuttonLeft.d3dmesh",
        "ui_action_promptFacebuttonRight.d3dmesh",
        "ui_action_promptFacebuttonUp.d3dmesh"
    ];

    public async Task<BuildResult> BuildAsync(
        BuildRequest request,
        IProgress<BuildProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequest(request);
        ApkLayout apkLayout = InspectApk(request.ApkPath);
        ObbLayout mainObb = InspectMainObb(request.MainObbPath);
        ObbLayout patchObb = InspectPatchObb(request.PatchObbPath);
        ButtonFixBundle? buttonFix = ResolveButtonFix(request.ButtonFixPath);

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

        long totalBytes = apkLayout.TotalBytes
            + mainObb.TotalBytes
            + patchObb.TotalBytes
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
                dataAddonFileCount,
                dataAddonOverwriteCount,
                cancellationToken);
            VerifyOutput(stagingDirectory, includedEpisodes, buttonFix);

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

    public static ApkLayout InspectApk(string apkPath)
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
            return new ApkLayout(RequiredLibraries.Length, assets.Count, libraryBytes + assets.Sum(entry => entry.Length));
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

    public static ObbLayout InspectMainObb(string mainObbPath)
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

        return new ObbLayout(bytes, FindPatchObb(mainObbPath));
    }

    public static ObbLayout InspectPatchObb(string patchObbPath)
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

        return new ObbLayout(bytes, null);
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
        if (!new[] { "performance", "balanced", "quality", "battery" }.Contains(request.GraphicsProfile))
        {
            throw new InvalidDataException("Choose a valid graphics profile.");
        }
        if (!new[] { "en", "fr", "de", "es", "pt", "ru", "zh" }.Contains(request.LanguageCode))
        {
            throw new InvalidDataException("Choose a valid language.");
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
        foreach (string file in Directory.EnumerateFiles(source.Path, "*", SearchOption.AllDirectories)
                     .Where(ChapterScanner.IsChapterFile))
        {
            cancellationToken.ThrowIfCancellationRequested();
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

    private static async Task<int> CopyChapterZipAsync(
        ChapterSource source,
        string assetsDirectory,
        IDictionary<string, string> copiedNames,
        Action<int> addBytes,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        int copied = 0;
        using ZipArchive zip = ZipFile.OpenRead(source.Path);
        foreach (ZipArchiveEntry entry in zip.Entries
                     .Where(entry => !string.IsNullOrEmpty(entry.Name) && ChapterScanner.IsChapterFile(entry.Name)))
        {
            cancellationToken.ThrowIfCancellationRequested();
            using Stream input = entry.Open();
            bool written = await CopyChapterStreamAsync(
                input,
                entry.Length,
                entry.Name,
                $"{source.Path}:{entry.FullName}",
                assetsDirectory,
                copiedNames,
                addBytes,
                cancellationToken);
            if (written)
            {
                copied++;
                report($"Added chapter file {entry.Name}");
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
        graphics = Regex.Replace(
            graphics,
            @"(?m)^profile\s*=\s*\S+",
            $"profile = {request.GraphicsProfile}",
            RegexOptions.CultureInvariant);

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
            $"Base OBBs: main + patch included\r\n" +
            $"Graphics profile: {request.GraphicsProfile}\r\n" +
            $"Language: {request.LanguageCode}\r\n" +
            $"Controller button fix: {(buttonFix is null ? "not supplied" : $"included ({buttonFix.FileCount} assets)")}\r\n" +
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

    private static void VerifyOutput(
        string output,
        IEnumerable<int> episodes,
        ButtonFixBundle? buttonFix)
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

public sealed record ApkLayout(int LibraryCount, int AssetCount, long TotalBytes);

public sealed record ObbLayout(long TotalBytes, string? PatchPath);
