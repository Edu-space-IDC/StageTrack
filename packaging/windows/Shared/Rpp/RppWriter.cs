using System.Globalization;
using System.Text;
using RpSeq.Core.Model;
using RpSeq.Core.Package;

namespace RpSeq.Core.Rpp;

/// <summary>Genera un proyecto .rpp de REAPER a partir de una canción de una secuencia.</summary>
public static class RppWriter
{
    private static string N(double v) => v.ToString("0.##########", CultureInfo.InvariantCulture);
    private static string Q(string? s) => "\"" + (s ?? "").Replace('"', '\'').Replace("\r", " ").Replace("\n", " ") + "\"";
    private static string G() => Guid.NewGuid().ToString("B").ToUpperInvariant();

    private static int NativeColor(string? hex)
    {
        if (string.IsNullOrEmpty(hex)) return 0;
        var h = hex.TrimStart('#');
        if (h.Length != 6 || !int.TryParse(h, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var rgb)) return 0;
        var r = (rgb >> 16) & 255; var g = (rgb >> 8) & 255; var b = rgb & 255;
        return 0x1000000 | r | (g << 8) | (b << 16);
    }

    private static string SourceType(string path) => Path.GetExtension(path).ToLowerInvariant() switch
    {
        ".mp3" => "MP3",
        ".flac" => "FLAC",
        ".ogg" or ".oga" => "VORBIS",
        _ => "WAVE",
    };

    public static string Write(SongModel song, LoadedSequence seq)
    {
        var sb = new StringBuilder();
        sb.AppendLine("<REAPER_PROJECT 0.1 \"7.0/win64\" " + DateTimeOffset.UtcNow.ToUnixTimeSeconds());
        sb.AppendLine("  RIPPLE 0");
        sb.AppendLine("  SAMPLERATE " + Math.Max(8000, song.SampleRate) + " 0 0");
        sb.AppendLine($"  TEMPO {N(song.Tempo)} {song.TimeSigNum} {song.TimeSigDen}");
        sb.AppendLine($"  MASTER_VOLUME {N(song.MasterVolume)} {N(song.MasterPan)} -1 -1 1");

        // marcadores y regiones
        var id = 1;
        foreach (var m in song.Markers)
            sb.AppendLine($"  MARKER {id++} {N(m.Position)} {Q(m.Name)} 0 {NativeColor(m.Color)} 1 R {G()} 0");
        foreach (var r in song.Regions)
        {
            var rid = id++;
            sb.AppendLine($"  MARKER {rid} {N(r.Start)} {Q(r.Name)} 1 {NativeColor(r.Color)} 1 R {G()} 0");
            sb.AppendLine($"  MARKER {rid} {N(r.End)} \"\" 1 {NativeColor(r.Color)} 1 R {G()} 0");
        }

        foreach (var t in song.Tracks)
        {
            sb.AppendLine("  <TRACK " + (string.IsNullOrEmpty(t.Guid) ? G() : t.Guid));
            sb.AppendLine("    NAME " + Q(t.Name));
            var col = NativeColor(t.Color);
            if (col != 0) sb.AppendLine("    PEAKCOL " + col);
            sb.AppendLine($"    VOLPAN {N(t.Volume)} {N(t.Pan)} -1 -1 1");
            sb.AppendLine($"    MUTESOLO {(t.Mute ? 1 : 0)} {(t.Solo ? 1 : 0)} 0");
            var d = t.FolderDepth;
            sb.AppendLine($"    ISBUS {(d > 0 ? 1 : d < 0 ? 2 : 0)} {d}");
            sb.AppendLine("    SHOWINMIX 1 0.6667 0.5 1 0.5 0 -1 0");
            sb.AppendLine($"    MAINSEND {(t.MainSend ? 1 : 0)} 0");
            foreach (var s in t.Sends)
                sb.AppendLine($"    AUXRECV {s.SourceIndex} {s.Mode} {N(s.Volume)} {N(s.Pan)} {(s.Mute ? 1 : 0)} 0 0 {s.SrcChan} {s.DstChan} -1 0 -1 \"\"");
            foreach (var h in t.HwOuts)
                sb.AppendLine("    HWOUT " + (!string.IsNullOrWhiteSpace(h.Raw)
                    ? h.Raw
                    : $"{h.Dest} {h.Mode} {N(h.Volume)} {N(h.Pan)} {(h.Mute ? 1 : 0)} 0 {h.SrcChan} -1:U"));

            foreach (var it in t.Items)
            {
                var path = seq.AudioPath(it);
                if (path == null) continue; // sin audio no hay ítem que crear
                sb.AppendLine("    <ITEM");
                sb.AppendLine("      POSITION " + N(it.Position));
                sb.AppendLine("      SNAPOFFS 0");
                sb.AppendLine("      LENGTH " + N(it.Length));
                sb.AppendLine("      LOOP 0");
                sb.AppendLine($"      FADEIN 1 {N(it.FadeIn)} 0 1 0 0 0");
                sb.AppendLine($"      FADEOUT 1 {N(it.FadeOut)} 0 1 0 0 0");
                sb.AppendLine($"      MUTE {(it.Mute ? 1 : 0)} 0");
                sb.AppendLine("      IGUID " + G());
                sb.AppendLine("      NAME " + Q(it.Name));
                sb.AppendLine($"      VOLPAN {N(it.Volume)} 0 1 -1");
                sb.AppendLine("      SOFFS " + N(it.Offset));
                sb.AppendLine($"      PLAYRATE {N(it.PlayRate <= 0 ? 1 : it.PlayRate)} 1 0 -1 0 0.0025");
                sb.AppendLine("      CHANMODE 0");
                sb.AppendLine("      <SOURCE " + SourceType(path));
                sb.AppendLine("        FILE " + Q(path));
                sb.AppendLine("      >");
                sb.AppendLine("    >");
            }
            sb.AppendLine("  >");
        }
        sb.AppendLine(">");
        return sb.ToString();
    }
}
