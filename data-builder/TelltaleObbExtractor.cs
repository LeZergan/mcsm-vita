using System.ComponentModel;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace McsmVitaDataBuilder;

internal sealed record TelltaleExtractionResult(int FileCount, long DeclaredBytes);

internal static class TelltaleObbExtractor
{
    private const int MinecraftStoryModeGameNumber = 58;
    private const string ExtractorResourceName = "McsmVitaDataBuilder.ThirdParty.ttarchext.exe";

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLinkW(
        string newFileName,
        string existingFileName,
        IntPtr securityAttributes);

    public static async Task<TelltaleExtractionResult> ExtractAsync(
        string obbPath,
        string assetsDirectory,
        string label,
        Action<string> report,
        CancellationToken cancellationToken)
    {
        ValidateNcttHeader(obbPath, label);
        Directory.CreateDirectory(assetsDirectory);

        string toolDirectory = Path.Combine(
            Path.GetTempPath(),
            $"mcsm-ttarchext-{Guid.NewGuid():N}");
        string toolPath = Path.Combine(toolDirectory, "ttarchext.exe");
        string aliasPath = Path.Combine(
            Path.GetDirectoryName(Path.GetFullPath(obbPath))!,
            $".mcsm-{label}-{Guid.NewGuid():N}.ttarch2");

        Directory.CreateDirectory(toolDirectory);
        try
        {
            await ExtractToolAsync(toolPath, cancellationToken);

            // ttarchext identifies a TTARCH2 container from the filename extension.
            // The copied OBB already lives on the output drive, so a hard link gives
            // it the required extension without duplicating another ~814 MB file.
            if (!CreateHardLinkW(aliasPath, Path.GetFullPath(obbPath), IntPtr.Zero))
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    $"Could not prepare the {label} OBB for extraction.");
            }

            report($"Extracting {label} OBB assets — this can take a few minutes…");
            ProcessStartInfo startInfo = new()
            {
                FileName = toolPath,
                WorkingDirectory = toolDirectory,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            startInfo.ArgumentList.Add("-o");
            startInfo.ArgumentList.Add(MinecraftStoryModeGameNumber.ToString());
            startInfo.ArgumentList.Add(aliasPath);
            startInfo.ArgumentList.Add(Path.GetFullPath(assetsDirectory));

            using Process process = new() { StartInfo = startInfo };
            if (!process.Start())
            {
                throw new IOException("The bundled Telltale OBB extractor could not be started.");
            }

            Task<string> stdoutTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
            Task<string> stderrTask = process.StandardError.ReadToEndAsync(cancellationToken);
            try
            {
                await process.WaitForExitAsync(cancellationToken);
            }
            catch
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }
                throw;
            }

            string stdout = await stdoutTask;
            string stderr = await stderrTask;
            if (process.ExitCode != 0)
            {
                string detail = string.IsNullOrWhiteSpace(stderr) ? stdout : stderr;
                throw new InvalidDataException(
                    $"The {label} OBB could not be extracted. " +
                    Tail(detail, 1200));
            }

            MatchCollection entries = Regex.Matches(
                stdout,
                @"(?m)^\s*[0-9a-fA-F]+\s+(\d+)\s+.+$");
            long declaredBytes = entries.Sum(match => long.Parse(match.Groups[1].Value));
            Match countMatch = Regex.Match(stdout, @"(?m)^-\s+(\d+)\s+files found\s*$");
            int fileCount = countMatch.Success
                ? int.Parse(countMatch.Groups[1].Value)
                : entries.Count;

            if (fileCount <= 0 || declaredBytes <= 0)
            {
                throw new InvalidDataException(
                    $"The {label} OBB extractor completed without producing any game assets.");
            }

            return new TelltaleExtractionResult(fileCount, declaredBytes);
        }
        finally
        {
            TryDeleteFile(aliasPath);
            TryDeleteDirectory(toolDirectory);
        }
    }

    private static void ValidateNcttHeader(string path, string label)
    {
        Span<byte> header = stackalloc byte[4];
        using FileStream stream = File.OpenRead(path);
        if (stream.Read(header) != header.Length || !header.SequenceEqual("NCTT"u8))
        {
            throw new InvalidDataException(
                $"The {label} OBB is not the expected Telltale NCTT container.");
        }
    }

    private static async Task ExtractToolAsync(string destination, CancellationToken cancellationToken)
    {
        Assembly assembly = typeof(TelltaleObbExtractor).Assembly;
        await using Stream source = assembly.GetManifestResourceStream(ExtractorResourceName)
            ?? throw new InvalidOperationException("The bundled Telltale OBB extractor is missing.");
        await using FileStream output = new(
            destination,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            128 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        await source.CopyToAsync(output, cancellationToken);
        await output.FlushAsync(cancellationToken);
    }

    private static string Tail(string value, int limit)
    {
        string trimmed = value.Trim();
        return trimmed.Length <= limit ? trimmed : trimmed[^limit..];
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
