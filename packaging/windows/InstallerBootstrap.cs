using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("StageTracks Setup")]
[assembly: AssemblyProduct("StageTracks")]
[assembly: AssemblyCompany("StageTracks")]
[assembly: AssemblyVersion("0.5.30.0")]
[assembly: AssemblyFileVersion("0.5.30.0")]

internal static class InstallerBootstrap
{
    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        bool autoUpdate = false;
        foreach (string argument in Environment.GetCommandLineArgs())
            if (String.Equals(argument, "--update", StringComparison.OrdinalIgnoreCase)) { autoUpdate = true; break; }
        Application.Run(new SetupForm(autoUpdate));
    }
}

internal sealed class SetupForm : Form
{
    private readonly string defaultPath;
    private readonly TextBox pathBox = new TextBox();
    private readonly CheckBox desktopShortcut = new CheckBox();
    private readonly CheckBox launchAfterInstall = new CheckBox();
    private readonly Label status = new Label();
    private readonly Label percent = new Label();
    private readonly ProgressBar progress = new ProgressBar();
    private readonly Label locationLabel = new Label();
    private readonly Label explanation = new Label();
    private readonly Label successPath = new Label();
    private readonly Panel successPanel = new Panel();
    private readonly Button browseButton = new Button();
    private readonly Button installButton = new Button();
    private readonly Button cancelButton = new Button();
    private readonly Button finishButton = new Button();
    private bool installing;
    private bool completed;
    private readonly bool autoUpdate;
    private bool createDesktopShortcut;
    private string installPath;

    public SetupForm(bool startUpdateAutomatically)
    {
        autoUpdate = startUpdateAutomatically;
        string standardPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "StageTracks");
        using (RegistryKey key = Registry.CurrentUser.OpenSubKey("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\StageTracks"))
            defaultPath = Convert.ToString(key == null ? null : key.GetValue("InstallLocation")) ?? standardPath;
        Text = "StageTracks Setup";
        ClientSize = new Size(760, 500);
        MinimumSize = new Size(760, 500);
        MaximumSize = new Size(760, 500);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterScreen;
        MaximizeBox = false;
        MinimizeBox = true;
        DoubleBuffered = true;
        BackColor = Color.FromArgb(244, 246, 248);
        ForeColor = Color.FromArgb(31, 39, 46);
        Font = new Font("Segoe UI", 9F);
        Icon = GetSetupIcon();
        BuildInterface();
        if (autoUpdate)
        {
            launchAfterInstall.Checked = true;
            Shown += delegate { BeginInstall(this, EventArgs.Empty); };
        }
        FormClosing += OnFormClosing;
    }

    private static Icon GetSetupIcon()
    {
        try
        {
            using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("StageTracks.app-icon.ico"))
                return stream == null ? SystemIcons.Application : new Icon(stream);
        }
        catch { return SystemIcons.Application; }
    }

    private void BuildInterface()
    {
        Panel header = new Panel();
        header.Dock = DockStyle.Top;
        header.Height = 124;
        header.BackColor = Color.FromArgb(31, 39, 46);
        header.ForeColor = Color.White;
        Controls.Add(header);

        Panel accent = new Panel();
        accent.Location = new Point(0, 0);
        accent.Size = new Size(760, 4);
        accent.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        accent.BackColor = Color.FromArgb(53, 190, 126);
        header.Controls.Add(accent);

        Panel setupCard = new Panel();
        setupCard.Location = new Point(20, 140);
        setupCard.Size = new Size(720, 180);
        setupCard.BackColor = Color.White;
        Controls.Add(setupCard);

        Panel cardAccent = new Panel();
        cardAccent.Location = new Point(0, 0);
        cardAccent.Size = new Size(4, 180);
        cardAccent.BackColor = Color.FromArgb(53, 190, 126);
        setupCard.Controls.Add(cardAccent);

        PictureBox logo = new PictureBox();
        logo.Size = new Size(56, 56);
        logo.Location = new Point(30, 30);
        logo.SizeMode = PictureBoxSizeMode.Zoom;
        try
        {
            using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("StageTracks.app-icon.png"))
                if (stream != null)
                    using (Bitmap source = new Bitmap(stream)) logo.Image = new Bitmap(source);
        }
        catch { }
        header.Controls.Add(logo);

        Label title = new Label();
        title.Text = "Instalar StageTracks";
        title.Font = new Font("Segoe UI Semibold", 21F, FontStyle.Bold);
        title.ForeColor = Color.White;
        title.AutoSize = true;
        title.Location = new Point(105, 26);
        header.Controls.Add(title);

        Label subtitle = new Label();
        subtitle.Text = "Secuencias y pads listos para tocar en vivo.";
        subtitle.ForeColor = Color.FromArgb(222, 229, 234);
        subtitle.AutoSize = true;
        subtitle.Location = new Point(108, 68);
        header.Controls.Add(subtitle);

        Label version = new Label();
        version.Text = "Windows x64 · versión 0.5.30";
        version.ForeColor = Color.FromArgb(190, 202, 211);
        version.AutoSize = true;
        version.Location = new Point(108, 91);
        header.Controls.Add(version);

        Label setupHeading = new Label();
        setupHeading.Text = "Elige dónde instalar StageTracks";
        setupHeading.Font = new Font("Segoe UI Semibold", 12F, FontStyle.Bold);
        setupHeading.ForeColor = Color.FromArgb(31, 39, 46);
        setupHeading.AutoSize = true;
        setupHeading.Location = new Point(32, 151);
        Controls.Add(setupHeading);

        locationLabel.Text = "Carpeta de destino";
        locationLabel.Font = new Font("Segoe UI Semibold", 10F, FontStyle.Bold);
        locationLabel.ForeColor = Color.FromArgb(31, 39, 46);
        locationLabel.AutoSize = true;
        locationLabel.Location = new Point(32, 181);
        Controls.Add(locationLabel);

        pathBox.Text = defaultPath;
        pathBox.Location = new Point(32, 208);
        pathBox.Size = new Size(570, 32);
        pathBox.BackColor = Color.White;
        pathBox.ForeColor = Color.FromArgb(31, 39, 46);
        pathBox.Font = new Font("Segoe UI", 10F);
        pathBox.BorderStyle = BorderStyle.FixedSingle;
        Controls.Add(pathBox);

        browseButton.Text = "Examinar…";
        browseButton.Location = new Point(615, 207);
        browseButton.Size = new Size(112, 35);
        StyleButton(browseButton, false);
        browseButton.Click += Browse;
        Controls.Add(browseButton);

        explanation.Text = "El instalador copiará StageTracks, los pads y los archivos necesarios a esta carpeta.";
        explanation.ForeColor = Color.FromArgb(74, 88, 100);
        explanation.AutoSize = true;
        explanation.Location = new Point(32, 250);
        Controls.Add(explanation);

        desktopShortcut.Text = "Crear un acceso directo en el escritorio";
        desktopShortcut.Checked = true;
        desktopShortcut.AutoSize = true;
        desktopShortcut.Location = new Point(32, 284);
        desktopShortcut.ForeColor = Color.FromArgb(43, 54, 63);
        Controls.Add(desktopShortcut);

        status.Text = "Elige dónde instalar StageTracks y pulsa Instalar.";
        status.ForeColor = Color.FromArgb(57, 72, 84);
        status.AutoSize = false;
        status.Location = new Point(32, 329);
        status.Size = new Size(690, 24);
        Controls.Add(status);

        progress.Location = new Point(32, 361);
        progress.Size = new Size(630, 18);
        progress.Style = ProgressBarStyle.Continuous;
        progress.Visible = false;
        Controls.Add(progress);

        percent.Text = "";
        percent.TextAlign = ContentAlignment.MiddleRight;
        percent.Location = new Point(670, 357);
        percent.Size = new Size(57, 24);
        percent.ForeColor = Color.FromArgb(57, 72, 84);
        percent.Visible = false;
        Controls.Add(percent);

        launchAfterInstall.Text = "Abrir StageTracks al finalizar";
        launchAfterInstall.Checked = true;
        launchAfterInstall.AutoSize = true;
        launchAfterInstall.Location = new Point(28, 178);
        launchAfterInstall.Visible = false;

        successPanel.Location = new Point(32, 150);
        successPanel.Size = new Size(695, 275);
        successPanel.BackColor = Color.FromArgb(231, 244, 236);
        successPanel.Visible = false;
        Controls.Add(successPanel);
        Label successIcon = new Label();
        successIcon.Text = "✓";
        successIcon.Font = new Font("Segoe UI", 36F, FontStyle.Bold);
        successIcon.ForeColor = Color.FromArgb(70, 205, 139);
        successIcon.Size = new Size(60, 64);
        successIcon.Location = new Point(25, 24);
        successPanel.Controls.Add(successIcon);
        Label successTitle = new Label();
        successTitle.Text = "StageTracks está instalado";
        successTitle.Font = new Font("Segoe UI Semibold", 17F, FontStyle.Bold);
        successTitle.ForeColor = Color.FromArgb(28, 52, 39);
        successTitle.AutoSize = true;
        successTitle.Location = new Point(96, 31);
        successPanel.Controls.Add(successTitle);
        Label successCopy = new Label();
        successCopy.Text = "La instalación terminó correctamente. Ya puedes abrir el programa.";
        successCopy.ForeColor = Color.FromArgb(55, 75, 63);
        successCopy.AutoSize = true;
        successCopy.Location = new Point(98, 69);
        successPanel.Controls.Add(successCopy);
        successPath.Text = "Ubicación: " + defaultPath;
        successPath.ForeColor = Color.FromArgb(68, 87, 76);
        successPath.AutoEllipsis = true;
        successPath.Location = new Point(98, 98);
        successPath.Size = new Size(550, 22);
        successPanel.Controls.Add(successPath);
        successPanel.Controls.Add(launchAfterInstall);

        Panel footer = new Panel();
        footer.Dock = DockStyle.Bottom;
        footer.Height = 62;
        footer.BackColor = Color.FromArgb(232, 237, 241);
        Controls.Add(footer);

        cancelButton.Text = "Cancelar";
        cancelButton.Size = new Size(105, 35);
        cancelButton.Location = new Point(510, 14);
        StyleButton(cancelButton, false);
        cancelButton.Click += delegate { Close(); };
        footer.Controls.Add(cancelButton);

        installButton.Text = "Instalar";
        installButton.Size = new Size(105, 35);
        installButton.Location = new Point(622, 14);
        StyleButton(installButton, true);
        installButton.Click += BeginInstall;
        footer.Controls.Add(installButton);

        finishButton.Text = "Finalizar";
        finishButton.Size = new Size(105, 35);
        finishButton.Location = new Point(622, 14);
        StyleButton(finishButton, true);
        finishButton.Visible = false;
        finishButton.Click += Finish;
        footer.Controls.Add(finishButton);
    }

    private static void StyleButton(Button button, bool primary)
    {
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = primary ? 0 : 1;
        button.FlatAppearance.BorderColor = Color.FromArgb(187, 198, 207);
        button.BackColor = primary ? Color.FromArgb(32, 137, 84) : Color.FromArgb(225, 231, 236);
        button.ForeColor = primary ? Color.White : Color.FromArgb(35, 48, 58);
        button.UseVisualStyleBackColor = false;
        button.Cursor = Cursors.Hand;
        button.Font = new Font("Segoe UI Semibold", 9F, FontStyle.Bold);
    }

    private void Browse(object sender, EventArgs e)
    {
        using (FolderBrowserDialog dialog = new FolderBrowserDialog())
        {
            dialog.Description = "Elige la carpeta donde se instalará StageTracks.";
            dialog.ShowNewFolderButton = true;
            if (Directory.Exists(pathBox.Text)) dialog.SelectedPath = pathBox.Text;
            if (dialog.ShowDialog(this) == DialogResult.OK) pathBox.Text = dialog.SelectedPath;
        }
    }

    private void BeginInstall(object sender, EventArgs e)
    {
        if (installing) return;
        try
        {
            installPath = Path.GetFullPath(pathBox.Text.Trim());
            if (String.IsNullOrWhiteSpace(pathBox.Text)) throw new InvalidOperationException("Elige una carpeta de instalación.");
            if (installPath.Length < 4 || Path.GetPathRoot(installPath) == installPath) throw new InvalidOperationException("Elige una subcarpeta, no la raíz de una unidad.");
            Directory.CreateDirectory(installPath);
        }
        catch (Exception ex) { MessageBox.Show(this, ex.Message, "Ubicación no válida", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }

        Process[] running = Process.GetProcessesByName("StageTracks");
        foreach (Process process in running)
        {
            try
            {
                string runningPath = process.MainModule == null ? "" : process.MainModule.FileName;
                if (!String.Equals(Path.GetFullPath(runningPath), Path.Combine(installPath, "StageTracks.exe"), StringComparison.OrdinalIgnoreCase)) continue;
                if (process.MainWindowHandle != IntPtr.Zero)
                {
                    process.CloseMainWindow();
                    if (!process.WaitForExit(60000))
                    {
                        MessageBox.Show(this, "Cierra StageTracks antes de continuar con la instalación.", "StageTracks está abierto", MessageBoxButtons.OK, MessageBoxIcon.Information);
                        return;
                    }
                }
            }
            catch { }
            finally { process.Dispose(); }
        }

        installing = true;
        createDesktopShortcut = desktopShortcut.Checked;
        pathBox.Enabled = false;
        browseButton.Enabled = false;
        desktopShortcut.Enabled = false;
        cancelButton.Enabled = false;
        installButton.Enabled = false;
        installButton.Text = "Instalando…";
        progress.Visible = true;
        progress.Value = 0;
        percent.Visible = true;
        percent.Text = "0%";
        status.Text = "Preparando los archivos…";
        Thread worker = new Thread(InstallWorker);
        worker.IsBackground = true;
        worker.Start();
    }

    private void InstallWorker()
    {
        try
        {
            Assembly assembly = Assembly.GetExecutingAssembly();
            using (Stream zipStream = assembly.GetManifestResourceStream("StageTracks.StageTracks-portable-windows-x64.zip"))
            {
                if (zipStream == null) throw new InvalidOperationException("No se encontró el paquete incluido en el instalador.");
                using (ZipArchive archive = new ZipArchive(zipStream, ZipArchiveMode.Read))
                {
                    long total = 0;
                    foreach (ZipArchiveEntry entry in archive.Entries) if (!entry.FullName.EndsWith("/", StringComparison.Ordinal)) total += entry.Length;
                    long copied = 0;
                    int lastReported = -1;
                    foreach (ZipArchiveEntry entry in archive.Entries)
                    {
                        string relative = entry.FullName.Replace('/', Path.DirectorySeparatorChar).Replace('\\', Path.DirectorySeparatorChar);
                        if (String.IsNullOrEmpty(relative)) continue;
                        string root = Path.GetFullPath(installPath).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
                        string output = Path.GetFullPath(Path.Combine(root, relative));
                        if (!output.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("El paquete contiene una ruta no válida.");
                        if (entry.FullName.EndsWith("/", StringComparison.Ordinal)) { Directory.CreateDirectory(output); continue; }
                        string parent = Path.GetDirectoryName(output);
                        if (!String.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
                        using (Stream input = entry.Open())
                        using (FileStream destination = new FileStream(output, FileMode.Create, FileAccess.Write, FileShare.None))
                        {
                            byte[] buffer = new byte[65536]; int read;
                            while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                            {
                                destination.Write(buffer, 0, read); copied += read;
                                int value = total <= 0 ? 100 : (int)Math.Min(100, copied * 100L / total);
                                if (value != lastReported)
                                {
                                    SetProgress(value, "Instalando archivos… " + Path.GetFileName(output));
                                    lastReported = value;
                                }
                            }
                        }
                    }
                }
            }

            CopyResource(assembly, "StageTracks.uninstall.ps1", Path.Combine(installPath, "uninstall.ps1"));
            CopyResource(assembly, "StageTracks.app-icon.ico", Path.Combine(installPath, "app-icon.ico"));
            CreateShortcuts();
            RegisterUninstaller();
            SetProgress(100, "StageTracks quedó instalado.");
            BeginInvoke((MethodInvoker)delegate { InstallationCompleted(); });
        }
        catch (Exception ex)
        {
            string message = ex.Message;
            BeginInvoke((MethodInvoker)delegate { InstallationFailed(message); });
        }
    }

    private static void CopyResource(Assembly assembly, string name, string path)
    {
        using (Stream input = assembly.GetManifestResourceStream(name))
        {
            if (input == null) return;
            using (FileStream output = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None)) input.CopyTo(output);
        }
    }

    private void SetProgress(int value, string message)
    {
        if (IsDisposed || !IsHandleCreated) return;
        try
        {
            BeginInvoke((MethodInvoker)delegate
            {
                progress.Value = Math.Max(0, Math.Min(100, value));
                percent.Text = value.ToString() + "%";
                status.Text = message;
            });
        }
        catch { }
    }

    private void CreateShortcuts()
    {
        string executable = Path.Combine(installPath, "StageTracks.exe");
        string startMenu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.StartMenu), "Programs", "StageTracks");
        Directory.CreateDirectory(startMenu);
        MakeShortcut(Path.Combine(startMenu, "StageTracks.lnk"), executable, Path.Combine(installPath, "app-icon.ico"));
        if (createDesktopShortcut)
            MakeShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "StageTracks.lnk"), executable, Path.Combine(installPath, "app-icon.ico"));
    }

    private void RegisterUninstaller()
    {
        RegistryKey key = Registry.CurrentUser.CreateSubKey("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\StageTracks");
        if (key == null) return;
        try
        {
            string executable = Path.Combine(installPath, "StageTracks.exe");
            string uninstall = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + Path.Combine(installPath, "uninstall.ps1") + "\"";
            key.SetValue("DisplayName", "StageTracks");
            key.SetValue("DisplayVersion", "0.5.30");
            key.SetValue("Publisher", "StageTracks");
            key.SetValue("InstallLocation", installPath);
            key.SetValue("DisplayIcon", Path.Combine(installPath, "app-icon.ico"));
            key.SetValue("UninstallString", uninstall);
            key.SetValue("NoModify", 1, RegistryValueKind.DWord);
            key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        }
        finally { key.Close(); }
    }

    private static void MakeShortcut(string shortcutPath, string executable, string iconPath)
    {
        Type shellType = Type.GetTypeFromProgID("WScript.Shell");
        if (shellType == null) return;
        object shell = null; object shortcut = null;
        try
        {
            shell = Activator.CreateInstance(shellType);
            shortcut = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { shortcutPath });
            Type type = shortcut.GetType();
            type.InvokeMember("TargetPath", BindingFlags.SetProperty, null, shortcut, new object[] { executable });
            type.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, shortcut, new object[] { Path.GetDirectoryName(executable) });
            type.InvokeMember("IconLocation", BindingFlags.SetProperty, null, shortcut, new object[] { iconPath + ",0" });
            type.InvokeMember("Save", BindingFlags.InvokeMethod, null, shortcut, null);
        }
        finally
        {
            if (shortcut != null && Marshal.IsComObject(shortcut)) Marshal.ReleaseComObject(shortcut);
            if (shell != null && Marshal.IsComObject(shell)) Marshal.ReleaseComObject(shell);
        }
    }

    private void InstallationCompleted()
    {
        installing = false; completed = true;
        progress.Value = 100; percent.Text = "100%";
        successPath.Text = "Ubicación: " + installPath;
        status.Text = "Instalación completada. Puedes abrir StageTracks ahora o cerrar el instalador.";
        locationLabel.Visible = false;
        pathBox.Visible = false;
        browseButton.Visible = false;
        explanation.Visible = false;
        desktopShortcut.Visible = false;
        status.Visible = false;
        progress.Visible = false;
        percent.Visible = false;
        successPanel.Visible = true;
        launchAfterInstall.Visible = true;
        launchAfterInstall.BringToFront();
        cancelButton.Visible = false;
        installButton.Visible = false;
        finishButton.Visible = true;
        finishButton.Parent.Controls.Add(finishButton);
        finishButton.BringToFront();
    }

    private void InstallationFailed(string message)
    {
        installing = false;
        status.Text = "No se pudo completar la instalación.";
        status.ForeColor = Color.FromArgb(164, 43, 43);
        pathBox.Enabled = true; browseButton.Enabled = true; desktopShortcut.Enabled = true;
        cancelButton.Enabled = true;
        installButton.Enabled = true; installButton.Text = "Reintentar";
        MessageBox.Show(this, "No se pudo instalar StageTracks:\n" + message, "Error de instalación", MessageBoxButtons.OK, MessageBoxIcon.Error);
    }

    private void Finish(object sender, EventArgs e)
    {
        Close();
    }

    private void OnFormClosing(object sender, FormClosingEventArgs e)
    {
        if (installing) { e.Cancel = true; return; }
        if (completed && launchAfterInstall.Checked)
        {
            try { Process.Start(new ProcessStartInfo(Path.Combine(installPath, "StageTracks.exe")) { WorkingDirectory = installPath, UseShellExecute = true }); }
            catch (Exception ex) { MessageBox.Show(this, "StageTracks quedó instalado, pero no se pudo abrir:\n" + ex.Message, "StageTracks", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        }
    }
}
