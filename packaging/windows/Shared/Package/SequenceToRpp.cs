using System.Text;
using RpSeq.Core.Model;
using RpSeq.Core.Rpp;

namespace RpSeq.Core.Package;

/// <summary>Convierte una secuencia (.zip o .rpseq) en proyectos .rpp listos para abrir en REAPER.</summary>
public static class SequenceToRpp
{
    /// <summary>Extrae el paquete junto al archivo y crea un .rpp por canción. Devuelve las rutas generadas.</summary>
    public static List<string> Convert(string path, out List<string> warnings)
    {
        warnings = new List<string>();
        var root = Path.GetDirectoryName(Path.GetFullPath(path)) ?? Path.GetTempPath();
        var seq = SequenceImporter.Load(path, root);
        var result = new List<string>();
        var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var song in seq.Doc.Songs)
        {
            var baseName = SequenceExporter.SafeName(string.IsNullOrWhiteSpace(song.Name) ? seq.Doc.Name : song.Name);
            var name = baseName; var n = 2;
            while (!used.Add(name) || File.Exists(Path.Combine(seq.BaseDir, name + ".rpp"))) name = baseName + " (" + n++ + ")";
            var target = Path.Combine(seq.BaseDir, name + ".rpp");

            var missing = song.Tracks.SelectMany(t => t.Items).Count(i => seq.AudioPath(i) == null);
            if (missing > 0) warnings.Add($"{song.Name}: {missing} audio(s) no se encontraron y se omitieron.");
            File.WriteAllText(target, RppWriter.Write(song, seq), new UTF8Encoding(false));
            result.Add(target);
        }
        return result;
    }
}
