using System.Globalization;
using System.Text;

namespace RpSeq.Core.Rpp;

/// <summary>Entrada ordenada de un bloque RPP: una línea (tokens) o un bloque hijo.</summary>
public sealed class RppEntry
{
    public string[]? Tokens { get; init; }
    public RppNode? Node { get; init; }
}

/// <summary>Bloque de un archivo .RPP de REAPER (&lt;NOMBRE args ... &gt;).</summary>
public sealed class RppNode
{
    public string Name { get; set; } = "";
    public List<string> Args { get; } = new();
    public List<RppEntry> Entries { get; } = new();

    public IEnumerable<string[]> Lines => Entries.Where(e => e.Tokens != null).Select(e => e.Tokens!);
    public IEnumerable<RppNode> Children => Entries.Where(e => e.Node != null).Select(e => e.Node!);

    public string[]? Line(string key) =>
        Lines.FirstOrDefault(t => t.Length > 0 && t[0].Equals(key, StringComparison.Ordinal));

    public IEnumerable<string[]> LinesNamed(string key) =>
        Lines.Where(t => t.Length > 0 && t[0].Equals(key, StringComparison.Ordinal));

    public RppNode? Child(string name) =>
        Children.FirstOrDefault(c => c.Name.Equals(name, StringComparison.Ordinal));

    public IEnumerable<RppNode> ChildrenNamed(string name) =>
        Children.Where(c => c.Name.Equals(name, StringComparison.Ordinal));

    // ---- lectura de valores
    public static double Num(string[]? t, int i, double def = 0)
    {
        if (t == null || i >= t.Length) return def;
        return double.TryParse(t[i], NumberStyles.Float, CultureInfo.InvariantCulture, out var v) && !double.IsNaN(v) && !double.IsInfinity(v) ? v : def;
    }

    public static int Int(string[]? t, int i, int def = 0) => (int)Math.Round(Num(t, i, def));

    public static string Str(string[]? t, int i, string def = "") => t != null && i < t.Length ? t[i] : def;
}

/// <summary>Lector de texto RPP: tokeniza líneas (comillas " ' `) y arma el árbol de bloques.</summary>
public static class RppParser
{
    public static RppNode ParseFile(string path) => Parse(File.ReadLines(path, Encoding.UTF8));

    public static RppNode Parse(IEnumerable<string> lines)
    {
        var root = new RppNode { Name = "" };
        var stack = new Stack<RppNode>();
        stack.Push(root);
        foreach (var raw in lines)
        {
            var line = raw.Trim();
            if (line.Length == 0) continue;
            if (line[0] == '<')
            {
                var toks = Tokenize(line.Substring(1));
                var node = new RppNode { Name = toks.Count > 0 ? toks[0] : "" };
                for (var i = 1; i < toks.Count; i++) node.Args.Add(toks[i]);
                stack.Peek().Entries.Add(new RppEntry { Node = node });
                stack.Push(node);
            }
            else if (line == ">")
            {
                if (stack.Count > 1) stack.Pop();
            }
            else
            {
                stack.Peek().Entries.Add(new RppEntry { Tokens = Tokenize(line).ToArray() });
            }
        }
        // el nodo raíz contiene <REAPER_PROJECT ...>
        return root.Child("REAPER_PROJECT") ?? root;
    }

    public static List<string> Tokenize(string s)
    {
        var res = new List<string>();
        var sb = new StringBuilder();
        var i = 0;
        while (i < s.Length)
        {
            var c = s[i];
            if (char.IsWhiteSpace(c)) { i++; continue; }
            sb.Clear();
            if (c == '"' || c == '\'' || c == '`')
            {
                var q = c; i++;
                while (i < s.Length && s[i] != q) { sb.Append(s[i]); i++; }
                i++; // cierra comilla
            }
            else
            {
                while (i < s.Length && !char.IsWhiteSpace(s[i])) { sb.Append(s[i]); i++; }
            }
            res.Add(sb.ToString());
        }
        return res;
    }
}
