using System.IO.Compression;
using System.Text.RegularExpressions;

namespace McsmVitaDataBuilder;

public static partial class ChapterScanner
{
    public static ChapterSource Inspect(string path)
    {
        if (Directory.Exists(path))
        {
            var files = Directory.EnumerateFiles(path, "*", SearchOption.AllDirectories)
                .Where(IsChapterFile)
                .Select(file => (Name: Path.GetFileName(file), Size: new FileInfo(file).Length))
                .ToList();
            return CreateSource(path, ChapterSourceKind.Folder, files);
        }

        if (File.Exists(path) && Path.GetExtension(path).Equals(".zip", StringComparison.OrdinalIgnoreCase))
        {
            using ZipArchive zip = ZipFile.OpenRead(path);
            var files = zip.Entries
                .Where(entry => !string.IsNullOrEmpty(entry.Name) && IsChapterFile(entry.Name))
                .Select(entry => (Name: entry.Name, Size: entry.Length))
                .ToList();
            return CreateSource(path, ChapterSourceKind.ZipArchive, files);
        }

        throw new InvalidDataException("Choose a chapter folder or a .zip containing chapter files.");
    }

    public static bool IsChapterFile(string path)
    {
        string extension = Path.GetExtension(path);
        return extension.Equals(".ttarch2", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".lua", StringComparison.OrdinalIgnoreCase);
    }

    public static IReadOnlyList<int> DetectEpisodes(IEnumerable<string> fileNames)
    {
        var episodes = new SortedSet<int>();
        foreach (string fileName in fileNames)
        {
            Match match = EpisodeMarkerRegex().Match(Path.GetFileName(fileName));
            if (match.Success
                && int.TryParse(match.Groups[1].Value, out int episode)
                && episode is >= 2 and <= 8)
            {
                episodes.Add(episode);
            }
        }
        return episodes.ToList();
    }

    private static ChapterSource CreateSource(
        string path,
        ChapterSourceKind kind,
        IReadOnlyCollection<(string Name, long Size)> files)
    {
        if (files.Count == 0)
        {
            throw new InvalidDataException("No .ttarch2 or descriptor .lua files were found in that source.");
        }

        IReadOnlyList<int> episodes = DetectEpisodes(files.Select(file => file.Name));
        if (episodes.Count == 0)
        {
            throw new InvalidDataException(
                "Chapter files were found, but the required Minecraft10N_data.ttarch2 episode marker is missing. " +
                "Choose the episode's complete Android files/Net folder or a ZIP made from it.");
        }

        return new ChapterSource(
            Path.GetFullPath(path),
            kind,
            episodes,
            files.Count,
            files.Sum(file => file.Size));
    }

    [GeneratedRegex(@"^MCSM_android_Minecraft10([2-8])_data\.ttarch2$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex EpisodeMarkerRegex();
}
