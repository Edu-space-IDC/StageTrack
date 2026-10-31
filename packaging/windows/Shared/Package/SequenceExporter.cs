using System.IO.Compression;
using System.Text;
using System.Text.Json;
using RpSeq.Core.Model;
using RpSeq.Core.Rpp;

namespace RpSeq.Core.Package;

public sealed class ExportReport
{
    public int Songs { get; set; }
    public int AudioFiles { get; set; }
    public List<string> Missing { get; } = new();
    public List<string> Skipped { get; } = new();
}

/// <summary>Genera NombreSecuencia.zip con NombreSecuencia.rpseq + Audio/ + Covers/.</summary>
public static class SequenceExporter
{
    public sealed record SongInput(string Name, string ProjectPath, string? Cover);

    private static readonly HashSet<string> Stored = new(StringComparer.OrdinalIgnoreCase) { ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".opus" };

    public static string SafeName(string name)
    {
        var bad = Path.GetInvalidFileNameChars();
        var sb = new StringBuilder();
        foreach (var c in name.Trim()) sb.Append(Array.IndexOf(bad, c) >= 0 ? '_' : c);
        var r = sb.ToString().Trim().TrimEnd('.');
        return r.Length == 0 ? "Secuencia" : r;
    }

    public static ExportReport Export(string sequenceName, IReadOnlyList<SongInput> songs, string zipPath, Action<string>? progress = null)
    {
        var report = new ExportReport();
        var seqName = SafeName(sequenceName);
        var doc = new SequenceDoc { Name = seqName };
        var tmp = zipPath + ".tmp";
        if (File.Exists(tmp)) File.Delete(tmp);

        var written = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase); // origen → ruta en zip
        var usedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        using (var fs = new FileStream(tmp, FileMode.Create, FileAccess.ReadWrite))
        using (var zip = new ZipArchive(fs, ZipArchiveMode.Create))
        {
            var n = 0;
            foreach (var input in songs)
            {
                n++;
                if (string.IsNullOrWhiteSpace(input.ProjectPath) || !File.Exists(input.ProjectPath))
                {
                    report.Skipped.Add(string.IsNullOrWhiteSpace(input.Name) ? $"Canción {n}" : input.Name);
                    continue;
                }
                progress?.Invoke($"Leyendo {input.Name}…");
                SongModel song;
                try { song = RppReader.ReadSong(input.ProjectPath, input.Name); }
                catch (Exception ex) { report.Skipped.Add($"{input.Name}: {ex.Message}"); continue; }

                song.SourceProject = null; // no exponemos rutas del equipo de origen
                song.Cover = WriteCover(zip, input.Cover, n);

                var folder = $"Audio/{n:00}-{SafeName(song.Name)}";
                foreach (var track in song.Tracks)
                {
                    foreach (var item in track.Items)
                    {
                        if (item.SourcePath == null)
                        {
                            report.Missing.Add($"{song.Name} · {item.OriginalFile}");
                            continue;
                        }
                        if (!written.TryGetValue(item.SourcePath, out var entryPath))
                        {
                            var fileName = SafeName(Path.GetFileName(item.SourcePath));
                            entryPath = $"{folder}/{fileName}";
                            var k = 2;
                            while (!usedNames.Add(entryPath))
                                entryPath = $"{folder}/{Path.GetFileNameWithoutExtension(fileName)}_{k++}{Path.GetExtension(fileName)}";
                            progress?.Invoke($"Copiando {fileName}…");
                            var level = Stored.Contains(Path.GetExtension(fileName)) ? CompressionLevel.NoCompression : CompressionLevel.Fastest;
                            zip.CreateEntryFromFile(item.SourcePath, entryPath, level);
                            written[item.SourcePath] = entryPath;
                            report.AudioFiles++;
                        }
                        item.File = entryPath;
                    }
                }
                doc.Songs.Add(song);
                report.Songs++;
            }

            var json = JsonSerializer.Serialize(doc, SeqFormat.Json);
            var entry = zip.CreateEntry(seqName + SeqFormat.Extension, CompressionLevel.Optimal);
            using (var w = new StreamWriter(entry.Open(), new UTF8Encoding(false))) w.Write(json);
        }

        if (report.Songs == 0) { File.Delete(tmp); throw new InvalidOperationException("Ninguna canción se pudo exportar (guarda los proyectos en disco primero)."); }
        if (File.Exists(zipPath)) File.Delete(zipPath);
        File.Move(tmp, zipPath);
        return report;
    }

    /// <summary>Guarda la carátula (data URI o URL http/https) en Covers/NN.ext y devuelve la ruta relativa.</summary>
    private static string? WriteCover(ZipArchive zip, string? cover, int n)
    {
        if (string.IsNullOrWhiteSpace(cover) || cover == "-") return null;
        try
        {
            byte[]? bytes = null; string ext = ".png";
            if (cover.StartsWith("data:", StringComparison.OrdinalIgnoreCase))
            {
                var comma = cover.IndexOf(',');
                if (comma < 0) return null;
                var head = cover.Substring(5, comma - 5).ToLowerInvariant();
                ext = head.Contains("jpeg") || head.Contains("jpg") ? ".jpg" : head.Contains("webp") ? ".webp" : head.Contains("gif") ? ".gif" : ".png";
                bytes = head.Contains("base64") ? Convert.FromBase64String(cover.Substring(comma + 1))
                                                : Encoding.UTF8.GetBytes(Uri.UnescapeDataString(cover.Substring(comma + 1)));
            }
            else if (Uri.TryCreate(cover, UriKind.Absolute, out var uri) && (uri.Scheme == "http" || uri.Scheme == "https"))
            {
                using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
                using var resp = http.GetAsync(uri).GetAwaiter().GetResult();
                if (!resp.IsSuccessStatusCode) return null;
                bytes = resp.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult();
                var mt = resp.Content.Headers.ContentType?.MediaType ?? "";
                ext = mt.Contains("jpeg") ? ".jpg" : mt.Contains("webp") ? ".webp" : mt.Contains("gif") ? ".gif" : ".png";
            }
            if (bytes == null || bytes.Length == 0) return null;
            var path = $"Covers/{n:00}{ext}";
            var e = zip.CreateEntry(path, CompressionLevel.NoCompression);
            using var s = e.Open();
            s.Write(bytes, 0, bytes.Length);
            return path;
        }
        catch { return null; }
    }
}
