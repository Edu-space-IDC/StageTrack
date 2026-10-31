using RpSeq.Core.Model;

namespace RpSeq.Core.Rpp;

/// <summary>Convierte un archivo .RPP de REAPER en un SongModel (pistas, ítems, regiones, routing).</summary>
public static class RppReader
{
    public static SongModel ReadSong(string rppPath, string? displayName = null)
    {
        var root = RppParser.ParseFile(rppPath);
        var projectDir = Path.GetDirectoryName(Path.GetFullPath(rppPath)) ?? "";
        var song = new SongModel
        {
            Name = string.IsNullOrWhiteSpace(displayName) ? Path.GetFileNameWithoutExtension(rppPath) : displayName!,
            SourceProject = rppPath,
        };

        var tempo = root.Line("TEMPO");
        if (tempo != null)
        {
            song.Tempo = RppNode.Num(tempo, 1, 120);
            song.TimeSigNum = Math.Max(1, RppNode.Int(tempo, 2, 4));
            song.TimeSigDen = Math.Max(1, RppNode.Int(tempo, 3, 4));
        }
        var sr = root.Line("SAMPLERATE");
        if (sr != null && RppNode.Int(sr, 1) > 0) song.SampleRate = RppNode.Int(sr, 1);
        var mv = root.Line("MASTER_VOLUME");
        if (mv != null) { song.MasterVolume = RppNode.Num(mv, 1, 1); song.MasterPan = RppNode.Num(mv, 2, 0); }
        var recordPath = RppNode.Str(root.Line("RECORD_PATH"), 1, "");

        ReadMarkers(root, song);
        ReadTracks(root, song, projectDir, recordPath);

        var end = song.Regions.Count > 0 ? song.Regions.Max(r => r.End) : 0;
        foreach (var t in song.Tracks)
            foreach (var it in t.Items)
                end = Math.Max(end, it.Position + it.Length);
        song.Duration = end;
        return song;
    }

    // ------------------------------------------------------------ marcadores / regiones
    private static void ReadMarkers(RppNode root, SongModel song)
    {
        var regionParts = new Dictionary<int, List<(double pos, string name, string? color)>>();
        foreach (var m in root.LinesNamed("MARKER"))
        {
            var idx = RppNode.Int(m, 1);
            var pos = RppNode.Num(m, 2);
            var name = RppNode.Str(m, 3);
            // REAPER puede guardar el indicador como 1 o como 9; el bit bajo
            // marca la entrada como región y los demás bits son opciones.
            var isRegion = (RppNode.Int(m, 4) & 1) != 0;
            var color = ColorToHex(RppNode.Int(m, 5));
            if (isRegion)
            {
                if (!regionParts.TryGetValue(idx, out var list)) regionParts[idx] = list = new();
                list.Add((pos, name, color));
            }
            else song.Markers.Add(new MarkerModel { Name = name, Position = pos, Color = color });
        }
        foreach (var kv in regionParts.OrderBy(k => k.Key))
        {
            var parts = kv.Value;
            if (parts.Count < 2) continue;
            var start = parts.OrderBy(p => p.pos).First();
            var end = parts.OrderBy(p => p.pos).Last();
            var name = !string.IsNullOrEmpty(start.name) ? start.name : end.name;
            song.Regions.Add(new RegionModel { Name = name, Start = start.pos, End = end.pos, Color = start.color ?? end.color });
        }
        song.Regions.Sort((a, b) => a.Start.CompareTo(b.Start));
        song.Markers.Sort((a, b) => a.Position.CompareTo(b.Position));
    }

    // ------------------------------------------------------------ pistas
    private static void ReadTracks(RppNode root, SongModel song, string projectDir, string recordPath)
    {
        var stack = new List<int>(); // pila de carpetas (índices 1-based)
        var index = 0;
        foreach (var tn in root.ChildrenNamed("TRACK"))
        {
            index++;
            var vp = tn.Line("VOLPAN");
            var ms = tn.Line("MUTESOLO");
            var bus = tn.Line("ISBUS");
            var depth = RppNode.Int(bus, 2, 0);
            var track = new TrackModel
            {
                Index = index,
                Guid = tn.Args.Count > 0 ? tn.Args[0] : "",
                Name = RppNode.Str(tn.Line("NAME"), 1, ""),
                Color = ColorToHex(RppNode.Int(tn.Line("PEAKCOL"), 1)),
                Volume = RppNode.Num(vp, 1, 1),
                Pan = RppNode.Num(vp, 2, 0),
                Mute = RppNode.Int(ms, 1) != 0,
                Solo = RppNode.Int(ms, 2) != 0,
                IsFolder = RppNode.Int(bus, 1) == 1,
                FolderDepth = depth,
                ParentIndex = stack.Count > 0 ? stack[^1] : 0,
                MainSend = tn.Line("MAINSEND") is { } msn ? RppNode.Int(msn, 1, 1) != 0 : true,
            };
            if (string.IsNullOrEmpty(track.Guid)) track.Guid = null;

            foreach (var h in tn.LinesNamed("HWOUT"))
                track.HwOuts.Add(new HwOutModel
                {
                    Dest = RppNode.Int(h, 1), Mode = RppNode.Int(h, 2), Volume = RppNode.Num(h, 3, 1),
                    Pan = RppNode.Num(h, 4), Mute = RppNode.Int(h, 5) != 0, SrcChan = RppNode.Int(h, 7),
                    Raw = string.Join(" ", h.Skip(1)),
                });
            foreach (var a in tn.LinesNamed("AUXRECV"))
                track.Sends.Add(new SendModel
                {
                    SourceIndex = RppNode.Int(a, 1), Mode = RppNode.Int(a, 2), Volume = RppNode.Num(a, 3, 1),
                    Pan = RppNode.Num(a, 4), Mute = RppNode.Int(a, 5) != 0,
                    SrcChan = RppNode.Int(a, 8), DstChan = RppNode.Int(a, 9),
                });

            foreach (var item in tn.ChildrenNamed("ITEM"))
            {
                var im = ReadItem(item, projectDir, recordPath);
                if (im != null) track.Items.Add(im);
            }
            song.Tracks.Add(track);

            if (depth > 0) for (var i = 0; i < depth; i++) stack.Add(index);
            else if (depth < 0) for (var i = 0; i < -depth && stack.Count > 0; i++) stack.RemoveAt(stack.Count - 1);
        }
    }

    // ------------------------------------------------------------ ítems / takes
    private sealed class Take
    {
        public bool Selected;
        public double Offset;
        public double Rate = 1;
        public string Name = "";
        public RppNode? Source;
    }

    private static ItemModel? ReadItem(RppNode item, string projectDir, string recordPath)
    {
        var takes = new List<Take> { new() };
        foreach (var e in item.Entries)
        {
            var cur = takes[^1];
            if (e.Node != null)
            {
                if (e.Node.Name == "SOURCE") cur.Source = e.Node;
                continue;
            }
            var t = e.Tokens!;
            if (t.Length == 0) continue;
            switch (t[0])
            {
                case "TAKE":
                    takes.Add(new Take { Selected = t.Skip(1).Any(x => x == "SEL") });
                    break;
                case "SOFFS": cur.Offset = RppNode.Num(t, 1); break;
                case "PLAYRATE": cur.Rate = Math.Max(0.01, RppNode.Num(t, 1, 1)); break;
                case "NAME": cur.Name = RppNode.Str(t, 1); break;
            }
        }
        var active = takes.Skip(1).FirstOrDefault(x => x.Selected) ?? takes[0];
        if (active.Source == null) return null;

        var (file, extraOffset) = FindFile(active.Source);
        if (string.IsNullOrEmpty(file)) return null; // MIDI / vacío: no se reproduce

        var fi = item.Line("FADEIN");
        var fo = item.Line("FADEOUT");
        var resolved = ResolveAudio(file!, projectDir, recordPath);
        return new ItemModel
        {
            Name = !string.IsNullOrEmpty(active.Name) ? active.Name : Path.GetFileName(file!),
            Position = RppNode.Num(item.Line("POSITION"), 1),
            Length = RppNode.Num(item.Line("LENGTH"), 1),
            Offset = active.Offset + extraOffset,
            PlayRate = active.Rate,
            Volume = Math.Abs(RppNode.Num(item.Line("VOLPAN"), 1, 1)),
            FadeIn = Math.Max(0, RppNode.Num(fi, 2)),
            FadeOut = Math.Max(0, RppNode.Num(fo, 2)),
            Mute = RppNode.Int(item.Line("MUTE"), 1) != 0,
            OriginalFile = file,
            SourcePath = resolved,
            Missing = resolved == null,
        };
    }

    /// <summary>Busca FILE dentro de SOURCE (incluye SECTION anidado con STARTPOS).</summary>
    private static (string? file, double extraOffset) FindFile(RppNode source)
    {
        var f = source.Line("FILE");
        if (f != null && f.Length > 1) return (f[1], 0);
        foreach (var child in source.ChildrenNamed("SOURCE"))
        {
            var (file, off) = FindFile(child);
            if (!string.IsNullOrEmpty(file)) return (file, off + RppNode.Num(source.Line("STARTPOS"), 1));
        }
        return (null, 0);
    }

    // ------------------------------------------------------------ rutas de audio
    public static string? ResolveAudio(string file, string projectDir, string recordPath)
    {
        try
        {
            var norm = file.Replace('/', Path.DirectorySeparatorChar).Replace('\\', Path.DirectorySeparatorChar);
            if (Path.IsPathRooted(norm) && File.Exists(norm)) return norm;
            var name = Path.GetFileName(norm);
            var rec = (recordPath ?? "").Replace('/', Path.DirectorySeparatorChar).Replace('\\', Path.DirectorySeparatorChar);
            var candidates = new List<string>();
            if (!Path.IsPathRooted(norm)) candidates.Add(Path.Combine(projectDir, norm));
            if (!string.IsNullOrEmpty(rec))
                candidates.Add(Path.IsPathRooted(rec) ? Path.Combine(rec, name) : Path.Combine(projectDir, rec, name));
            candidates.Add(Path.Combine(projectDir, name));
            candidates.Add(Path.Combine(projectDir, "Audio", name));
            candidates.Add(Path.Combine(projectDir, "Media", name));
            foreach (var c in candidates) if (File.Exists(c)) return Path.GetFullPath(c);

            if (Directory.Exists(projectDir) && projectDir.Length > 3)
            {
                var found = Directory.EnumerateFiles(projectDir, name, SearchOption.AllDirectories).FirstOrDefault();
                if (found != null) return found;
            }
        }
        catch { /* rutas inválidas o sin permiso */ }
        return null;
    }

    // ------------------------------------------------------------ colores
    /// <summary>Color nativo de REAPER (COLORREF + flag 0x1000000) → "#rrggbb"; null si no hay color.</summary>
    public static string? ColorToHex(int native)
    {
        if ((native & 0x1000000) == 0) return null;
        var r = native & 0xFF; var g = (native >> 8) & 0xFF; var b = (native >> 16) & 0xFF;
        return $"#{r:x2}{g:x2}{b:x2}";
    }
}
