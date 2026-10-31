using System.IO.Compression;
using System.Text.Json;
using RpSeq.Core.Model;
using RpSeq.Core.Rpp;

namespace RpSeq.Core.Package;

public sealed class LoadedSequence
{
    public SequenceDoc Doc { get; init; } = new();
    /// <summary>Carpeta base: las rutas File/Cover del documento son relativas a ella.</summary>
    public string BaseDir { get; init; } = "";

    public string? AudioPath(ItemModel item)
    {
        if (!string.IsNullOrEmpty(item.File))
        {
            var p = Path.GetFullPath(Path.Combine(BaseDir, item.File.Replace('/', Path.DirectorySeparatorChar)));
            return File.Exists(p) ? p : null;
        }
        return item.SourcePath != null && File.Exists(item.SourcePath) ? item.SourcePath : null;
    }

    public string? CoverPath(SongModel song)
    {
        if (string.IsNullOrEmpty(song.Cover)) return null;
        var p = Path.GetFullPath(Path.Combine(BaseDir, song.Cover.Replace('/', Path.DirectorySeparatorChar)));
        return File.Exists(p) ? p : null;
    }
}

/// <summary>Abre .zip (paquete exportado), .rpseq o .rpp y devuelve el modelo listo para reproducir.</summary>
public static class SequenceImporter
{
    public static LoadedSequence Load(string path, string extractRoot)
    {
        var ext = Path.GetExtension(path).ToLowerInvariant();
        if (ext == ".rpp")
        {
            var doc = new SequenceDoc { Name = Path.GetFileNameWithoutExtension(path) };
            doc.Songs.Add(RppReader.ReadSong(path));
            return new LoadedSequence { Doc = doc, BaseDir = Path.GetDirectoryName(Path.GetFullPath(path)) ?? "" };
        }
        if (ext == ".zip")
        {
            var dir = Path.Combine(extractRoot, SequenceExporter.SafeName(Path.GetFileNameWithoutExtension(path)) + "_" + Guid.NewGuid().ToString("N")[..6]);
            Directory.CreateDirectory(dir);
            SafeExtract(path, dir);
            var seq = Directory.EnumerateFiles(dir, "*" + SeqFormat.Extension, SearchOption.TopDirectoryOnly).FirstOrDefault()
                      ?? throw new InvalidDataException("El zip no contiene un archivo " + SeqFormat.Extension);
            return Load(seq, extractRoot);
        }
        if (ext == SeqFormat.Extension)
        {
            var doc = JsonSerializer.Deserialize<SequenceDoc>(File.ReadAllText(path), SeqFormat.Json)
                      ?? throw new InvalidDataException("Archivo de secuencia vacío.");
            if (!string.Equals(doc.Format, SeqFormat.Format, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("No es una secuencia válida.");
            if (doc.Version > SeqFormat.Version)
                throw new InvalidDataException("La secuencia es de una versión más nueva del programa.");
            return new LoadedSequence { Doc = doc, BaseDir = Path.GetDirectoryName(Path.GetFullPath(path)) ?? "" };
        }
        throw new NotSupportedException("Formato no soportado: " + ext);
    }

    /// <summary>Extrae el zip evitando rutas que salgan de la carpeta destino (zip-slip).</summary>
    private static void SafeExtract(string zipPath, string dest)
    {
        var root = Path.GetFullPath(dest).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        using var zip = ZipFile.OpenRead(zipPath);
        foreach (var e in zip.Entries)
        {
            if (string.IsNullOrEmpty(e.Name)) continue; // carpeta
            var target = Path.GetFullPath(Path.Combine(dest, e.FullName));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            e.ExtractToFile(target, true);
        }
    }
}
