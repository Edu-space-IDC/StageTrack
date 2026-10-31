using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Text.Json;
using System.Text;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;
using RpSeq.Core.Package;

namespace REAPERControlStudio;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new MainWindow());
    }
}

internal sealed class MainWindow : Form
{
    private readonly WebView2 browser = new() { Dock = DockStyle.Fill, Visible = false };
    private readonly Panel entry = new() { Dock = DockStyle.Fill, BackColor = Color.FromArgb(27, 28, 30) };
    private readonly TextBox urlBox = new() { Width = 592, Height = 38, Font = new Font("Segoe UI", 11), PlaceholderText = "http://127.0.0.1:8080/", BackColor = Color.FromArgb(21, 22, 23), ForeColor = Color.FromArgb(233, 234, 236), BorderStyle = BorderStyle.FixedSingle };
    private readonly Label status = new() { AutoSize = false, ForeColor = Color.FromArgb(133, 205, 163), Font = new Font("Segoe UI", 9), Size = new Size(592, 42) };
    private readonly CheckBox autoOpen = new() { AutoSize = true, Text = "Abrir URL automáticamente al iniciar", ForeColor = Color.FromArgb(205, 207, 211), BackColor = Color.FromArgb(33, 34, 36), Font = new Font("Segoe UI", 9) };
    private bool closingAfterSave;
    private bool closingCheckInProgress;
    private TaskCompletionSource<(bool Ok, string Message)>? exitSaveCompletion;
    private readonly string appDataDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "REAPER Control Studio");
    private readonly string settingsPath;
    private readonly string webViewDataDirectory;
    private CoreWebView2Environment? webViewEnvironment;
    private const string LauncherHost = "stage-track-control.local";
    private bool showingConnectionMenu;
    private bool launcherHostMapped;
    private string? pendingConnectionError;

    public MainWindow()
    {
        Text = "Reaper Control Studio";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(760, 520);
        Size = new Size(1200, 820);
        BackColor = Color.FromArgb(27, 28, 30);
        Icon = LoadAppIcon();
        settingsPath = Path.Combine(appDataDirectory, "settings.json");
        webViewDataDirectory = Path.Combine(appDataDirectory, "WebView2");
        BuildEntry();
        Controls.Add(browser);
        Controls.Add(entry);
        entry.Visible = false;
        Load += async (_, _) => await StartAsync();
        FormClosing += OnFormClosing;
    }

    private static Icon LoadAppIcon()
    {
        try
        {
            var assembly = typeof(MainWindow).Assembly;
            var resource = assembly.GetManifestResourceNames().FirstOrDefault(name => name.EndsWith(".reaper.ico", StringComparison.OrdinalIgnoreCase));
            if (resource is null) return SystemIcons.Application;
            using var stream = assembly.GetManifestResourceStream(resource);
            return stream is null ? SystemIcons.Application : new Icon(stream);
        }
        catch { return SystemIcons.Application; }
    }

    private string GetAppIconDataUri()
    {
        using var stream = typeof(MainWindow).Assembly.GetManifestResourceStream("REAPERControlStudio.app-icon.png");
        if (stream is null) throw new InvalidOperationException("No se encontro el icono PNG de la aplicacion.");
        using var buffer = new MemoryStream();
        stream.CopyTo(buffer);
        return "data:image/png;base64," + Convert.ToBase64String(buffer.ToArray());
    }

    private void BuildEntry()
    {
        var card = new Panel { Width = 680, Height = 460, BackColor = Color.FromArgb(33, 34, 36) };
        ApplyRoundedCorners(card, 18);
        card.Paint += (_, e) =>
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            using var path = RoundedPath(new Rectangle(0, 0, card.Width - 1, card.Height - 1), 18);
            using var pen = new Pen(Color.FromArgb(54, 56, 60));
            e.Graphics.DrawPath(pen, path);
        };
        var logo = new PictureBox { Image = Icon.ToBitmap(), SizeMode = PictureBoxSizeMode.Zoom, Size = new Size(48, 48), BackColor = card.BackColor, Location = new Point(44, 38) };
        var title = new Label { Text = "Reaper Control Studio", AutoSize = true, ForeColor = Color.White, Font = new Font("Segoe UI", 17, FontStyle.Bold), Location = new Point(108, 46) };
        var description = new Label { Text = "Conecta con el servidor web local de REAPER para abrir tu estudio.", AutoSize = true, ForeColor = Color.FromArgb(185, 188, 193), Font = new Font("Segoe UI", 10), Location = new Point(44, 120) };
        var urlLabel = new Label { Text = "DIRECCIÓN DEL SERVIDOR", AutoSize = true, ForeColor = Color.FromArgb(142, 151, 164), Font = new Font("Segoe UI", 8, FontStyle.Bold), Location = new Point(44, 154) };
        urlBox.Location = new Point(44, 178);
        var connect = new Button { Text = "Conectar", Location = new Point(44, 232), Size = new Size(152, 44), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(53, 157, 100), ForeColor = Color.White, Font = new Font("Segoe UI", 10, FontStyle.Bold), Cursor = Cursors.Hand };
        connect.FlatAppearance.BorderSize = 0;
        connect.FlatAppearance.MouseOverBackColor = Color.FromArgb(65, 178, 116);
        connect.FlatAppearance.MouseDownBackColor = Color.FromArgb(41, 129, 81);
        ApplyRoundedCorners(connect, 9);
        ApplyRoundedCorners(urlBox, 6);
        connect.Click += async (_, _) => await ConnectAsync();
        urlBox.KeyDown += async (_, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; await ConnectAsync(); } };
        autoOpen.Location = new Point(44, 302);
        autoOpen.CheckedChanged += (_, _) => SaveSettings();
        status.Location = new Point(44, 330);
        var help = new Label { Text = "Ejemplo: http://127.0.0.1:8080/\nAsegúrate de que REAPER esté abierto y tenga activo su servidor web.", AutoSize = true, ForeColor = Color.FromArgb(143, 147, 153), Font = new Font("Segoe UI", 9), Location = new Point(44, 385) };
        card.Controls.AddRange([logo, title, description, urlLabel, urlBox, connect, autoOpen, status, help]);
        entry.Controls.Add(card);
        entry.Resize += (_, _) => card.Location = new Point((entry.ClientSize.Width - card.Width) / 2, (entry.ClientSize.Height - card.Height) / 2);
    }

    private static GraphicsPath RoundedPath(Rectangle bounds, int radius)
    {
        var diameter = radius * 2;
        var path = new GraphicsPath();
        path.AddArc(bounds.Left, bounds.Top, diameter, diameter, 180, 90);
        path.AddArc(bounds.Right - diameter, bounds.Top, diameter, diameter, 270, 90);
        path.AddArc(bounds.Right - diameter, bounds.Bottom - diameter, diameter, diameter, 0, 90);
        path.AddArc(bounds.Left, bounds.Bottom - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
        return path;
    }

    private static void ApplyRoundedCorners(Control control, int radius)
    {
        void UpdateRegion()
        {
            if (control.Width <= 0 || control.Height <= 0) return;
            using var path = RoundedPath(new Rectangle(0, 0, control.Width, control.Height), radius);
            var old = control.Region;
            control.Region = new Region(path);
            old?.Dispose();
        }
        control.Resize += (_, _) => UpdateRegion();
        UpdateRegion();
    }

    private async Task StartAsync()
    {
        try
        {
            Directory.CreateDirectory(appDataDirectory);
            Directory.CreateDirectory(webViewDataDirectory);
            if (File.Exists(settingsPath))
            {
                var settings = JsonSerializer.Deserialize<Settings>(await File.ReadAllTextAsync(settingsPath));
                urlBox.Text = settings?.Url ?? "";
                autoOpen.Checked = settings?.AutoOpen ?? false;
            }
            await EnsureBrowserAsync();
            browser.CoreWebView2.DocumentTitleChanged += (_, _) => { if (browser.Visible) Text = "Reaper Control Studio"; };
            browser.CoreWebView2.NavigationCompleted += async (_, e) =>
            {
                if (e.IsSuccess)
                {
                    entry.Visible = false; browser.Visible = true; status.Text = "";
                    var iconData = JsonSerializer.Serialize(GetAppIconDataUri());
                    await browser.CoreWebView2.ExecuteScriptAsync($"window.setAppLogo?.({iconData});(()=>{{const i=document.getElementById('rl_appLogo');if(i)i.src={iconData};}})()");
                    if (showingConnectionMenu && !string.IsNullOrWhiteSpace(pendingConnectionError))
                    {
                        var message = pendingConnectionError;
                        pendingConnectionError = null;
                        await browser.CoreWebView2.ExecuteScriptAsync("window.showConnectionError?.(" + JsonSerializer.Serialize(message) + ")");
                    }
                }
                else await ShowConnectionMenuAsync("No se pudo conectar con REAPER. Verifica que REAPER este abierto y que la direccion del servidor sea correcta.");
            };
            browser.CoreWebView2.NavigationStarting += (_, e) =>
            {
                if (Uri.TryCreate(e.Uri, UriKind.Absolute, out var navigationUri)
                    && navigationUri.Host.Equals(LauncherHost, StringComparison.OrdinalIgnoreCase))
                {
                    if (showingConnectionMenu && navigationUri.Query == "?nativeLauncher=1") return;
                    e.Cancel = true;
                    _ = browser.CoreWebView2.ExecuteScriptAsync("window.showConnectionError?.('Esta direccion no es un servidor de REAPER. Revisa la URL de la conexion.')");
                    return;
                }
                if (!IsAllowed(e.Uri)) { e.Cancel = true; status.Text = "Blocked navigation outside the local network."; }
                else showingConnectionMenu = false;
            };
            browser.CoreWebView2.NewWindowRequested += (_, e) => { e.Handled = true; if (IsAllowed(e.Uri)) browser.CoreWebView2.Navigate(e.Uri); };
            browser.CoreWebView2.ProcessFailed += async (_, _) => await ShowConnectionMenuAsync();
            browser.CoreWebView2.FrameCreated += (_, e) =>
            {
                var frame = e.Frame;
                frame.WebMessageReceived += async (_, message) => await HandleWebMessageAsync(message, frame);
            };
            browser.CoreWebView2.WebMessageReceived += async (_, e) => await HandleWebMessageAsync(e, null);
            browser.CoreWebView2.Settings.AreDefaultScriptDialogsEnabled = false;
            browser.CoreWebView2.ScriptDialogOpening += (_, e) =>
            {
                if (ShowStyledDialog(e.Kind, e.Message, e.DefaultText, out var result)) { e.ResultText = result; e.Accept(); }
            };
            browser.CoreWebView2.Settings.AreDefaultContextMenusEnabled = true;
            browser.CoreWebView2.Settings.IsStatusBarEnabled = false;
            // No navegues primero al launcher y luego al estudio: ambas navegaciones
            // compiten durante el inicio y WebView2 puede cancelar la segunda dejando
            // la ventana en negro. En autoapertura, valida y carga directamente REAPER.
            if (autoOpen.Checked && !string.IsNullOrWhiteSpace(urlBox.Text))
                await ConnectAsync();
            else
                await ShowConnectionMenuAsync();
        }
        catch (Exception ex)
        {
            ShowEntry("No se pudo iniciar el navegador integrado. Instala Microsoft Edge WebView2 Runtime y vuelve a abrir el programa. " + ex.Message);
        }
    }

    private async Task EnsureBrowserAsync()
    {
        Directory.CreateDirectory(webViewDataDirectory);
        webViewEnvironment ??= await CoreWebView2Environment.CreateAsync(userDataFolder: webViewDataDirectory);
        await browser.EnsureCoreWebView2Async(webViewEnvironment);
    }

    private async Task HandleWebMessageAsync(CoreWebView2WebMessageReceivedEventArgs e, CoreWebView2Frame? sourceFrame)
    {
        var action = e.TryGetWebMessageAsString();
        if (action.StartsWith("saveExitResult|", StringComparison.Ordinal))
        {
            var parts = action.Split('|', 3);
            if (parts.Length == 3)
                exitSaveCompletion?.TrySetResult((parts[1] == "1", Uri.UnescapeDataString(parts[2])));
            return;
        }
        async Task ReplyAsync(string script)
        {
            try
            {
                if (sourceFrame is not null) await sourceFrame.ExecuteScriptAsync(script);
                else if (browser.CoreWebView2 is not null) await browser.CoreWebView2.ExecuteScriptAsync(script);
            }
            catch (Exception ex) { status.Text = "No se pudo devolver la ruta a la página: " + ex.Message; }
        }

        if (action == "logout")
        {
            await ShowConnectionMenuAsync();
        }
        else if (action.StartsWith("exportSequence|", StringComparison.Ordinal))
        {
            await ExportSequenceAsync(action.Substring("exportSequence|".Length), ReplyAsync);
        }
        else if (action == "openProject")
        {
            using var dialog = new OpenFileDialog { Title = "Abrir proyecto o secuencia", Filter = "Proyectos y secuencias (*.rpp;*.rpseq;*.zip)|*.rpp;*.rpseq;*.zip|Proyectos REAPER (*.rpp)|*.rpp|Secuencias (*.rpseq;*.zip)|*.rpseq;*.zip|Todos los archivos (*.*)|*.*", CheckFileExists = true };
            if (dialog.ShowDialog(this) == DialogResult.OK)
            {
                var picked = dialog.FileName;
                var pickedExt = Path.GetExtension(picked).ToLowerInvariant();
                if (pickedExt == ".zip" || pickedExt == ".rpseq")
                {
                    List<string> projects; List<string> warnings;
                    try
                    {
                        var warn = new List<string>();
                        projects = await Task.Run(() => { var r = SequenceToRpp.Convert(picked, out var w); warn = w; return r; });
                        warnings = warn;
                    }
                    catch (Exception ex)
                    {
                        await ReplyAsync("window.receiveExportResult(false," + JsonSerializer.Serialize("No se pudo importar la secuencia: " + ex.Message) + ")");
                        return;
                    }
                    var first = true;
                    foreach (var rpp in projects)
                    {
                        if (!first) await Task.Delay(2500);
                        first = false;
                        await ReplyAsync($"window.receiveLocalPath('open',{JsonSerializer.Serialize(rpp)})");
                    }
                    if (warnings.Count > 0)
                        await ReplyAsync("window.receiveExportResult(false," + JsonSerializer.Serialize(string.Join(" · ", warnings)) + ")");
                }
                else
                    await ReplyAsync($"window.receiveLocalPath('open',{JsonSerializer.Serialize(picked)})");
            }
        }
        else if (action == "importMedia")
        {
            using var dialog = new OpenFileDialog { Title = "Importar archivos a REAPER", Filter = "Audio y MIDI|*.wav;*.wave;*.mp3;*.flac;*.ogg;*.aif;*.aiff;*.mid;*.midi;*.mp4;*.mov|Todos los archivos (*.*)|*.*", CheckFileExists = true, Multiselect = true };
            if (dialog.ShowDialog(this) == DialogResult.OK)
                await ReplyAsync($"window.receiveLocalFiles({JsonSerializer.Serialize(dialog.FileNames)})");
        }
        else if (action == "saveProject")
        {
            using var dialog = new SaveFileDialog { Title = "Guardar proyecto de REAPER", Filter = "Proyecto REAPER (*.RPP)|*.RPP", DefaultExt = "RPP", AddExtension = true, OverwritePrompt = true, FileName = "Nuevo proyecto.RPP" };
            if (dialog.ShowDialog(this) == DialogResult.OK)
                await ReplyAsync($"window.receiveSavePath({JsonSerializer.Serialize(dialog.FileName)})");
            else
                await ReplyAsync("window.receiveSavePath(null)");
        }
    }

    private async Task ExportSequenceAsync(string json, Func<string, Task> reply)
    {
        static string Js(string text) => JsonSerializer.Serialize(text);
        ExportRequest? request;
        try { request = JsonSerializer.Deserialize<ExportRequest>(json, new JsonSerializerOptions { PropertyNameCaseInsensitive = true }); }
        catch { request = null; }
        var songs = request?.Songs?.Where(x => !string.IsNullOrWhiteSpace(x.Path)).ToList() ?? new List<ExportSong>();
        if (songs.Count == 0)
        {
            await reply("window.receiveExportResult(false," + Js("No hay secuencias guardadas en disco para exportar. Guarda los proyectos primero.") + ")");
            return;
        }
        var defaultName = SequenceExporter.SafeName(string.IsNullOrWhiteSpace(request?.Name) ? (songs[0].Name ?? "Secuencia") : request!.Name!);
        using var dialog = new SaveFileDialog
        {
            Title = "Exportar secuencia para REAPER Playback",
            Filter = "Paquete de secuencia (*.zip)|*.zip",
            DefaultExt = "zip",
            AddExtension = true,
            OverwritePrompt = true,
            FileName = defaultName + ".zip",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            await reply("window.receiveExportResult(null,'')");
            return;
        }
        var target = dialog.FileName;
        var inputs = songs.Select(x => new SequenceExporter.SongInput(x.Name ?? "", x.Path ?? "", x.Cover)).ToList();
        try
        {
            var report = await Task.Run(() => SequenceExporter.Export(Path.GetFileNameWithoutExtension(target), inputs, target));
            var msg = $"Secuencia exportada: {report.Songs} canción(es), {report.AudioFiles} audio(s)";
            if (report.Missing.Count > 0) msg += $". {report.Missing.Count} audio(s) no se encontraron";
            if (report.Skipped.Count > 0) msg += $". Omitidas: {string.Join(", ", report.Skipped)}";
            await reply("window.receiveExportResult(" + (report.Missing.Count > 0 || report.Skipped.Count > 0 ? "'warn'" : "true") + "," + Js(msg) + ")");
        }
        catch (Exception ex)
        {
            await reply("window.receiveExportResult(false," + Js("No se pudo exportar: " + ex.Message) + ")");
        }
    }

    private async Task ConnectAsync()
    {
        var raw = urlBox.Text.Trim();
        if (!Uri.TryCreate(raw, UriKind.Absolute, out var uri) || (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps) || !IsAllowed(uri.AbsoluteUri))
        {
            await ShowConnectionMenuAsync("La direccion no es valida. Introduce una URL HTTP o HTTPS de REAPER.");
            return;
        }
        urlBox.Text = uri.ToString();
        status.ForeColor = Color.FromArgb(133, 205, 163);
        status.Text = "Conectando con REAPER…";
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(6));
            using var client = new HttpClient();
            using var response = await client.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, timeout.Token);
            if (!response.IsSuccessStatusCode)
            {
                await ShowConnectionMenuAsync("No se pudo conectar con REAPER. Verifica que REAPER este abierto y que su servidor web este activo.");
                return;
            }
            SaveSettings();
            if (browser.CoreWebView2 is null) await EnsureBrowserAsync();
            showingConnectionMenu = false;
            pendingConnectionError = null;
            browser.CoreWebView2.Navigate(uri.ToString());
        }
        catch
        {
            await ShowConnectionMenuAsync("No se pudo conectar con REAPER. Verifica que REAPER este abierto y que su servidor web este activo.");
        }
    }

    private async Task ShowConnectionMenuAsync(string? connectionError = null)
    {
        try
        {
            var launcherPrefix = "https://" + LauncherHost + "/REAPER_CONTROL_STUDIO_3.html?nativeLauncher=1";
            if (showingConnectionMenu && browser.Visible && browser.CoreWebView2?.Source.StartsWith(launcherPrefix, StringComparison.OrdinalIgnoreCase) == true)
            {
                if (!string.IsNullOrWhiteSpace(connectionError))
                {
                    pendingConnectionError = connectionError;
                    var result = await browser.CoreWebView2.ExecuteScriptAsync("(()=>{if(typeof window.showConnectionError!=='function')return false;window.showConnectionError(" + JsonSerializer.Serialize(connectionError) + ");return true;})()");
                    if (result == "true") pendingConnectionError = null;
                }
                return;
            }
            if (browser.CoreWebView2 is null) await EnsureBrowserAsync();
            var assembly = typeof(MainWindow).Assembly;
            using var stream = assembly.GetManifestResourceStream("REAPERControlStudio.Studio.html");
            if (stream is null) throw new InvalidOperationException("No se encontro la interfaz de conexiones integrada.");
            using var reader = new StreamReader(stream, Encoding.UTF8);
            var html = await reader.ReadToEndAsync();
            var seedUrl = urlBox.Text.Trim();
            if (Uri.TryCreate(seedUrl, UriKind.Absolute, out var seedUri) && seedUri.Host.Equals(LauncherHost, StringComparison.OrdinalIgnoreCase)) seedUrl = "";
            var initialUrl = JsonSerializer.Serialize(seedUrl);
            html = html.Replace("</head>", "<script>window.__RCS_INITIAL_URL=" + initialUrl + ";</script></head>", StringComparison.OrdinalIgnoreCase);
            var launcherDirectory = Path.Combine(appDataDirectory, "Launcher");
            Directory.CreateDirectory(launcherDirectory);
            using (var iconStream = assembly.GetManifestResourceStream("REAPERControlStudio.app-icon.png"))
            {
                if (iconStream is null) throw new InvalidOperationException("No se encontro el icono PNG de la aplicacion.");
                using var iconFile = File.Create(Path.Combine(launcherDirectory, "app-icon.png"));
                await iconStream.CopyToAsync(iconFile);
            }
            var launcherFile = Path.Combine(launcherDirectory, "REAPER_CONTROL_STUDIO_3.html");
            await File.WriteAllTextAsync(launcherFile, html, new UTF8Encoding(false));
            if (!launcherHostMapped)
            {
                browser.CoreWebView2.SetVirtualHostNameToFolderMapping(
                    LauncherHost, launcherDirectory, CoreWebView2HostResourceAccessKind.DenyCors);
                launcherHostMapped = true;
            }
            showingConnectionMenu = true;
            entry.Visible = false;
            browser.Visible = true;
            browser.BringToFront();
            Text = "Reaper Control Studio";
            pendingConnectionError = connectionError;
            browser.CoreWebView2.Navigate("https://" + LauncherHost + "/REAPER_CONTROL_STUDIO_3.html?nativeLauncher=1");
        }
        catch (Exception ex)
        {
            ShowEntry("No se pudo abrir el menu de conexiones. " + ex.Message);
        }
    }

    private static bool IsAllowed(string value)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || (uri.Scheme != "http" && uri.Scheme != "https")) return false;
        var host = uri.Host;
        return host.Equals("localhost", StringComparison.OrdinalIgnoreCase) || host.EndsWith(".localhost", StringComparison.OrdinalIgnoreCase) ||
               System.Net.IPAddress.TryParse(host, out var ip) && (System.Net.IPAddress.IsLoopback(ip) || ip.IsIPv6LinkLocal || ip.IsIPv6SiteLocal || IsPrivateIpv4(ip));
    }

    private static bool IsPrivateIpv4(System.Net.IPAddress ip)
    {
        var b = ip.GetAddressBytes();
        return b.Length == 4 && (b[0] == 10 || b[0] == 192 && b[1] == 168 || b[0] == 172 && b[1] >= 16 && b[1] <= 31 || b[0] == 169 && b[1] == 254);
    }

    private void SaveSettings()
    {
        try
        {
            Directory.CreateDirectory(appDataDirectory);
            File.WriteAllText(settingsPath, JsonSerializer.Serialize(new Settings(urlBox.Text.Trim(), autoOpen.Checked)));
        }
        catch (Exception ex) { status.Text = "No se pudieron guardar las preferencias: " + ex.Message; }
    }

    private void ShowEntry(string message)
    {
        status.ForeColor = string.IsNullOrWhiteSpace(message) ? Color.FromArgb(133, 205, 163) : Color.FromArgb(255, 155, 145);
        status.Text = message;
        browser.Visible = false;
        entry.Visible = true;
        Text = "Reaper Control Studio";
    }

    private bool ShowStyledDialog(CoreWebView2ScriptDialogKind kind, string message, string defaultText, out string result)
    {
        result = defaultText;
        if (kind == CoreWebView2ScriptDialogKind.Alert)
        {
        using var info = new Form { Text = "Reaper Control Studio", FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, ClientSize = new Size(440, 160), BackColor = Color.FromArgb(27, 29, 33), MinimizeBox = false, MaximizeBox = false };
            var label = new Label { Text = message, ForeColor = Color.Gainsboro, Font = new Font("Segoe UI", 10), Location = new Point(22, 22), Size = new Size(396, 78) };
            var ok = new Button { Text = "Aceptar", DialogResult = DialogResult.OK, Location = new Point(324, 112), Size = new Size(94, 32), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(59, 158, 255), ForeColor = Color.White };
            info.Controls.AddRange([label, ok]); info.AcceptButton = ok; info.ShowDialog(this); return true;
        }
        using var question = new Form { Text = "Reaper Control Studio", FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, ClientSize = new Size(440, 170), BackColor = Color.FromArgb(27, 29, 33), MinimizeBox = false, MaximizeBox = false };
        question.Controls.Add(new Label { Text = message, ForeColor = Color.Gainsboro, Font = new Font("Segoe UI", 10), Location = new Point(22, 20), Size = new Size(396, 88) });
        var prompt = kind == CoreWebView2ScriptDialogKind.Prompt;
        var input = new TextBox { Text = defaultText, Visible = prompt, Location = new Point(22, 92), Size = new Size(396, 26), BackColor = Color.FromArgb(18, 20, 23), ForeColor = Color.White, BorderStyle = BorderStyle.FixedSingle };
        var cancel = new Button { Text = "Cancelar", DialogResult = DialogResult.Cancel, Location = new Point(214, 128), Size = new Size(96, 32), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(55, 59, 67), ForeColor = Color.White };
        var accept = new Button { Text = "Continuar", DialogResult = DialogResult.OK, Location = new Point(322, 128), Size = new Size(96, 32), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(59, 158, 255), ForeColor = Color.White };
        question.Controls.AddRange([input, cancel, accept]); question.AcceptButton = accept; question.CancelButton = cancel;
        if (prompt) question.Controls[0].Height = 60;
        var accepted = question.ShowDialog(this) == DialogResult.OK;
        if (accepted && prompt) result = input.Text;
        return accepted;
    }

    private async void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        if (closingAfterSave) { browser.Dispose(); return; }
        if (closingCheckInProgress) { e.Cancel = true; return; }
        e.Cancel = true;
        closingCheckInProgress = true;
        try
        {
            SaveSettings();
            var dirty = await GetProjectDirtyAsync();
            for (var i = 0; dirty is null && i < 12; i++) { await Task.Delay(200); dirty = await GetProjectDirtyAsync(); }
            if (dirty is not true) { closingAfterSave = true; Close(); return; }
            var choice = AskSaveChanges(); // Save, discard, or cancel
            if (choice == DialogResult.Cancel) return;
            if (choice == DialogResult.Yes)
            {
                var saved = await RequestProjectSaveAsync();
                if (!saved.Ok)
                {
                    MessageBox.Show(this, saved.Message, "No se pudo confirmar el guardado", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                for (var i = 0; i < 50; i++)
                {
                    await Task.Delay(200);
                    if (await GetProjectDirtyAsync() == false) { closingAfterSave = true; Close(); return; }
                }
                // El helper confirmó que Main_SaveProject terminó; el valor dirty puede quedar desfasado.
                closingAfterSave = true; Close(); return;
            }
            try
            {
                await browser.CoreWebView2.ExecuteScriptAsync("sendCmd('discard',false)");
                for (var i = 0; i < 50; i++)
                {
                    await Task.Delay(200);
                    if (await GetProjectDirtyAsync() == false) { closingAfterSave = true; Close(); return; }
                }
                MessageBox.Show(this, "REAPER no confirmó que los cambios se descartaran. La ventana seguirá abierta.", "Reaper Control Studio", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            catch { MessageBox.Show(this, "No se pudieron deshacer los cambios del proyecto.", "Reaper Control Studio", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        }
        finally { if (!closingAfterSave) closingCheckInProgress = false; }
    }

    private async Task<(bool Ok, string Message)> RequestProjectSaveAsync()
    {
        var completion = new TaskCompletionSource<(bool Ok, string Message)>(TaskCreationOptions.RunContinuationsAsynchronously);
        exitSaveCompletion = completion;
        try
        {
            await browser.CoreWebView2.ExecuteScriptAsync("window.requestSaveForExit?.()");
            var finished = await Task.WhenAny(completion.Task, Task.Delay(TimeSpan.FromSeconds(15)));
            return finished == completion.Task
                ? await completion.Task
                : (false, "REAPER no respondió al guardado. El programa seguirá abierto para no perder los cambios.");
        }
        catch (Exception ex) { return (false, "No se pudo enviar el guardado a REAPER: " + ex.Message); }
        finally { exitSaveCompletion = null; }
    }

    private async Task<bool?> GetProjectDirtyAsync()
    {
        try
        {
            var value = await browser.CoreWebView2.ExecuteScriptAsync("window.projectDirtyForHost===null?null:window.projectDirtyForHost===true");
            if (value == "true") return true;
            if (value == "false") return false;
        }
        catch { }
        return null;
    }

    private DialogResult AskSaveChanges()
    {
        using var form = new Form { Text = "Cambios sin guardar", FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, ClientSize = new Size(470, 188), BackColor = Color.FromArgb(27, 29, 33), MinimizeBox = false, MaximizeBox = false };
        form.Controls.Add(new Label { Text = "El proyecto de REAPER tiene cambios sin guardar. ¿Qué quieres hacer?", ForeColor = Color.Gainsboro, Font = new Font("Segoe UI", 10), Location = new Point(22, 25), Size = new Size(426, 54) });
        var save = new Button { Text = "Guardar", DialogResult = DialogResult.Yes, Location = new Point(22, 112), Size = new Size(116, 38), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(59, 158, 255), ForeColor = Color.White };
        var discard = new Button { Text = "Deshacer cambios", DialogResult = DialogResult.No, Location = new Point(148, 112), Size = new Size(150, 38), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(55, 59, 67), ForeColor = Color.White };
        var cancel = new Button { Text = "Cancelar", DialogResult = DialogResult.Cancel, Location = new Point(308, 112), Size = new Size(116, 38), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(55, 59, 67), ForeColor = Color.White };
        form.Controls.AddRange([save, discard, cancel]); form.AcceptButton = save; form.CancelButton = cancel;
        return form.ShowDialog(this);
    }

    private sealed record Settings(string Url, bool AutoOpen);
    private sealed class ExportSong { public string? Name { get; set; } public string? Path { get; set; } public string? Cover { get; set; } }
    private sealed class ExportRequest { public string? Name { get; set; } public List<ExportSong>? Songs { get; set; } }
}
