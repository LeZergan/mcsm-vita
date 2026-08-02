using System.IO.Compression;

namespace McsmVitaDataBuilder;

public static class DataAddonScanner
{
    public static DataAddonSource Inspect(string path)
    {
        if (Directory.Exists(path))
        {
            string root = NormalizeFolderRoot(path);
            var files = Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
                .Select(file => new
                {
                    Relative = NormalizeRelativePath(Path.GetRelativePath(root, file)),
                    Size = new FileInfo(file).Length
                })
                .ToList();
            ValidateFiles(files.Select(file => file.Relative));
            return CreateSource(root, DataAddonSourceKind.Folder, files.Count, files.Sum(file => file.Size));
        }

        if (File.Exists(path) && Path.GetExtension(path).Equals(".zip", StringComparison.OrdinalIgnoreCase))
        {
            using ZipArchive zip = ZipFile.OpenRead(path);
            var files = zip.Entries
                .Where(entry => !string.IsNullOrEmpty(entry.Name))
                .Select(entry => new
                {
                    Relative = NormalizeArchivePath(entry.FullName),
                    Size = entry.Length
                })
                .ToList();
            ValidateFiles(files.Select(file => file.Relative));
            return CreateSource(Path.GetFullPath(path), DataAddonSourceKind.ZipArchive, files.Count, files.Sum(file => file.Size));
        }

        throw new InvalidDataException("Choose a mod/data folder or a .zip archive.");
    }

    public static string NormalizeFolderRoot(string path)
    {
        string root = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar);
        if (Path.GetFileName(root).Equals("mcsm", StringComparison.OrdinalIgnoreCase))
        {
            return root;
        }

        string child = Path.Combine(root, "mcsm");
        return Directory.Exists(child) ? child : root;
    }

    public static string NormalizeArchivePath(string archivePath)
    {
        string relative = archivePath.Replace('\\', '/').TrimStart('/');
        string lower = relative.ToLowerInvariant();

        foreach (string prefix in new[] { "ux0/data/mcsm/", "data/mcsm/", "mcsm/" })
        {
            if (lower.StartsWith(prefix, StringComparison.Ordinal))
            {
                relative = relative[prefix.Length..];
                return NormalizeRelativePath(relative);
            }
        }

        int nestedMcsm = lower.IndexOf("/mcsm/", StringComparison.Ordinal);
        if (nestedMcsm >= 0)
        {
            relative = relative[(nestedMcsm + "/mcsm/".Length)..];
        }
        return NormalizeRelativePath(relative);
    }

    public static string NormalizeRelativePath(string relativePath)
    {
        string normalized = relativePath.Replace('\\', '/').TrimStart('/');
        if (string.IsNullOrWhiteSpace(normalized)
            || normalized.Contains(':')
            || normalized.Split('/', StringSplitOptions.RemoveEmptyEntries).Any(part => part is "." or ".."))
        {
            throw new InvalidDataException($"Unsafe or empty path in data add-on: {relativePath}");
        }
        return normalized.Replace('/', Path.DirectorySeparatorChar);
    }

    public static void EnsureNotCritical(string relativePath)
    {
        string normalized = NormalizeRelativePath(relativePath);
        if (normalized.Contains(Path.DirectorySeparatorChar))
        {
            return;
        }

        string name = Path.GetFileName(normalized);
        if (DataBuilderService.RequiredLibraries.Contains(name, StringComparer.OrdinalIgnoreCase)
            || name.Equals(DataBuilderService.MainObbName, StringComparison.OrdinalIgnoreCase)
            || name.Equals(DataBuilderService.PatchObbName, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"The data add-on tries to replace protected runtime file '{name}'. " +
                "Core libraries and OBBs cannot be installed as mods.");
        }
    }

    private static void ValidateFiles(IEnumerable<string> paths)
    {
        string[] files = paths.ToArray();
        if (files.Length == 0)
        {
            throw new InvalidDataException("The selected mod/data add-on contains no files.");
        }

        foreach (string file in files)
        {
            EnsureNotCritical(file);
        }

        string? duplicate = files
            .GroupBy(file => file, StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault(group => group.Count() > 1)
            ?.Key;
        if (duplicate is not null)
        {
            throw new InvalidDataException($"The data add-on contains the same destination twice: {duplicate}");
        }
    }

    private static DataAddonSource CreateSource(string path, DataAddonSourceKind kind, int count, long bytes)
    {
        if (count == 0)
        {
            throw new InvalidDataException("The selected mod/data add-on contains no files.");
        }
        return new DataAddonSource(path, kind, count, bytes);
    }
}
