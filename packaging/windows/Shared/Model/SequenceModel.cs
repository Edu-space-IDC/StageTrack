using System.Text.Json;
using System.Text.Json.Serialization;

namespace RpSeq.Core.Model;

public static class SeqFormat
{
    public const string Extension = ".rpseq";
    public const string Format = "rpseq";
    public const int Version = 1;

    public static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        PropertyNameCaseInsensitive = true,
    };
}

/// <summary>Secuencia = conjunto de canciones (una por pestaña/proyecto de REAPER).</summary>
public sealed class SequenceDoc
{
    public string Format { get; set; } = SeqFormat.Format;
    public int Version { get; set; } = SeqFormat.Version;
    public string Name { get; set; } = "";
    public string Created { get; set; } = DateTime.UtcNow.ToString("o");
    public List<SongModel> Songs { get; set; } = new();
}

public sealed class SongModel
{
    public string Name { get; set; } = "";
    /// <summary>Ruta relativa dentro del paquete (cover/01.png) o null si no hay carátula.</summary>
    public string? Cover { get; set; }
    public string? SourceProject { get; set; }
    public double Tempo { get; set; } = 120;
    public int TimeSigNum { get; set; } = 4;
    public int TimeSigDen { get; set; } = 4;
    public int SampleRate { get; set; } = 44100;
    public double Duration { get; set; }
    public double MasterVolume { get; set; } = 1;
    public double MasterPan { get; set; }
    public List<TrackModel> Tracks { get; set; } = new();
    public List<RegionModel> Regions { get; set; } = new();
    public List<MarkerModel> Markers { get; set; } = new();
}

public sealed class TrackModel
{
    public int Index { get; set; }              // 1-based, como en REAPER
    public string? Guid { get; set; }
    public string Name { get; set; } = "";
    public string? Color { get; set; }          // "#rrggbb"
    public double Volume { get; set; } = 1;     // lineal
    public double Pan { get; set; }             // -1..1
    public bool Mute { get; set; }
    public bool Solo { get; set; }
    public bool IsFolder { get; set; }
    public int FolderDepth { get; set; }        // I_FOLDERDEPTH de REAPER
    public int ParentIndex { get; set; }        // 0 = sin carpeta
    public bool MainSend { get; set; } = true;  // salida al master/padre
    public List<HwOutModel> HwOuts { get; set; } = new();
    public List<SendModel> Sends { get; set; } = new();   // recepciones (AUXRECV)
    public List<ItemModel> Items { get; set; } = new();
}

public sealed class HwOutModel
{
    public int Dest { get; set; }
    public int Mode { get; set; }
    public double Volume { get; set; } = 1;
    public double Pan { get; set; }
    public bool Mute { get; set; }
    public int SrcChan { get; set; }
    public string? Raw { get; set; }
}

public sealed class SendModel
{
    public int SourceIndex { get; set; }        // 0-based (pista origen)
    public int Mode { get; set; }
    public double Volume { get; set; } = 1;
    public double Pan { get; set; }
    public bool Mute { get; set; }
    public int SrcChan { get; set; }
    public int DstChan { get; set; }
}

public sealed class ItemModel
{
    public string Name { get; set; } = "";
    public double Position { get; set; }
    public double Length { get; set; }
    public double Offset { get; set; }          // inicio dentro del audio (s)
    public double PlayRate { get; set; } = 1;
    public double Volume { get; set; } = 1;
    public double FadeIn { get; set; }
    public double FadeOut { get; set; }
    public bool Mute { get; set; }
    /// <summary>Ruta relativa dentro del paquete (audio/...). Null si falta el archivo.</summary>
    public string? File { get; set; }
    public string? OriginalFile { get; set; }
    public bool Missing { get; set; }

    /// <summary>Ruta absoluta encontrada al leer el .rpp (no se guarda en el JSON).</summary>
    [JsonIgnore] public string? SourcePath { get; set; }
}

public sealed class RegionModel
{
    public string Name { get; set; } = "";
    public double Start { get; set; }
    public double End { get; set; }
    public string? Color { get; set; }
}

public sealed class MarkerModel
{
    public string Name { get; set; } = "";
    public double Position { get; set; }
    public string? Color { get; set; }
}
