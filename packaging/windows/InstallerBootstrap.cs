using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("StageTracks Setup")]
[assembly: AssemblyProduct("StageTracks")]
[assembly: AssemblyCompany("StageTracks")]
[assembly: AssemblyVersion("0.5.34.0")]
[assembly: AssemblyFileVersion("0.5.34.0")]

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
    private readonly Label setupHeading = new Label();
    private readonly Label explanation = new Label();
    private readonly Label successPath = new Label();
    private readonly Panel setupCard = new Panel();
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
        BackColor = Color.FromArgb(25, 27, 30);
        ForeColor = Color.FromArgb(239, 242, 245);
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
        header.BackColor = Color.FromArgb(20, 22, 24);
        header.ForeColor = Color.White;
        Controls.Add(header);

        Panel accent = new Panel();
        accent.Location = new Point(0, 0);
        accent.Size = new Size(760, 4);
        accent.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        accent.BackColor = Color.FromArgb(53, 190, 126);
        header.Controls.Add(accent);

        setupCard.Location = new Point(20, 140);
        setupCard.Size = new Size(720, 180);
        setupCard.BackColor = Color.FromArgb(31, 34, 38);
        Controls.Add(setupCard);
        ApplyRoundedCorners(setupCard, 14);

        Panel cardAccent = new Panel();
        cardAccent.Location = new Point(0, 12);
        cardAccent.Size = new Size(4, 156);
        cardAccent.BackColor = Color.FromArgb(53, 190, 126);
        setupCard.Controls.Add(cardAccent);
        ApplyRoundedCorners(cardAccent, 2);

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
        version.Text = "Windows x64 · versión 0.5.34";
        version.ForeColor = Color.FromArgb(190, 202, 211);
        version.AutoSize = true;
        version.Location = new Point(108, 91);
        header.Controls.Add(version);

        setupHeading.Text = "Elige dónde instalar StageTracks";
        setupHeading.Font = new Font("Segoe UI Semibold", 12F, FontStyle.Bold);
        setupHeading.ForeColor = Color.FromArgb(245, 247, 249);
        setupHeading.AutoSize = true;
        setupHeading.Location = new Point(32, 151);
        Controls.Add(setupHeading);

        locationLabel.Text = "Carpeta de destino";
        locationLabel.Font = new Font("Segoe UI Semibold", 10F, FontStyle.Bold);
        locationLabel.ForeColor = Color.FromArgb(225, 230, 235);
        locationLabel.AutoSize = true;
        locationLabel.Location = new Point(32, 181);
        Controls.Add(locationLabel);

        pathBox.Text = defaultPath;
        pathBox.Location = new Point(32, 208);
        pathBox.Size = new Size(570, 32);
        pathBox.BackColor = Color.FromArgb(17, 19, 21);
        pathBox.ForeColor = Color.White;
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
        explanation.ForeColor = Color.FromArgb(195, 203, 210);
        explanation.AutoSize = true;
        explanation.Location = new Point(32, 250);
        Controls.Add(explanation);

        desktopShortcut.Text = "Crear un acceso directo en el escritorio";
        desktopShortcut.Checked = true;
        desktopShortcut.AutoSize = true;
        desktopShortcut.Location = new Point(32, 284);
        desktopShortcut.ForeColor = Color.FromArgb(225, 230, 235);
        Controls.Add(desktopShortcut);

        status.Text = "Elige dónde instalar StageTracks y pulsa Instalar.";
        status.ForeColor = Color.FromArgb(205, 213, 220);
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
        percent.ForeColor = Color.FromArgb(190, 200, 208);
        percent.Visible = false;
        Controls.Add(percent);

        launchAfterInstall.Text = "Abrir StageTracks al finalizar";
        launchAfterInstall.Checked = true;
        launchAfterInstall.AutoSize = true;
        launchAfterInstall.Location = new Point(28, 178);
        launchAfterInstall.Visible = false;

        successPanel.Location = new Point(26, 142);
        successPanel.Size = new Size(708, 274);
        successPanel.BackColor = Color.FromArgb(29, 41, 34);
        successPanel.Visible = false;
        Controls.Add(successPanel);
        ApplyRoundedCorners(successPanel, 16);
        Label successIcon = new Label();
        successIcon.Text = "✓";
        successIcon.Font = new Font("Segoe UI", 32F, FontStyle.Bold);
        successIcon.ForeColor = Color.FromArgb(70, 205, 139);
        successIcon.Size = new Size(60, 64);
        successIcon.Location = new Point(28, 27);
        successPanel.Controls.Add(successIcon);
        Label successTitle = new Label();
        successTitle.Text = "StageTracks está instalado";
        successTitle.Font = new Font("Segoe UI Semibold", 16F, FontStyle.Bold);
        successTitle.ForeColor = Color.FromArgb(241, 245, 242);
        successTitle.AutoSize = true;
        successTitle.Location = new Point(101, 29);
        successPanel.Controls.Add(successTitle);
        Label successCopy = new Label();
        successCopy.Text = "La instalación terminó correctamente. Ya puedes abrir el programa.";
        successCopy.Font = new Font("Segoe UI", 9.5F);
        successCopy.ForeColor = Color.FromArgb(195, 207, 198);
        successCopy.AutoSize = false;
        successCopy.Location = new Point(102, 70);
        successCopy.Size = new Size(570, 38);
        successPanel.Controls.Add(successCopy);
        successPath.Text = "Ubicación: " + defaultPath;
        successPath.Font = new Font("Segoe UI", 9F);
        successPath.ForeColor = Color.FromArgb(168, 185, 174);
        successPath.AutoEllipsis = true;
        successPath.Location = new Point(102, 116);
        successPath.Size = new Size(570, 28);
        successPanel.Controls.Add(successPath);
        Panel successDivider = new Panel();
        successDivider.Location = new Point(26, 171);
        successDivider.Size = new Size(656, 1);
        successDivider.BackColor = Color.FromArgb(55, 75, 62);
        successPanel.Controls.Add(successDivider);
        launchAfterInstall.Font = new Font("Segoe UI", 9.5F);
        launchAfterInstall.ForeColor = Color.FromArgb(225, 232, 227);
        launchAfterInstall.Location = new Point(29, 198);
        successPanel.Controls.Add(launchAfterInstall);

        Panel footer = new Panel();
        footer.Dock = DockStyle.Bottom;
        footer.Height = 62;
        footer.BackColor = Color.FromArgb(20, 22, 24);
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
        setupCard.SendToBack();
    }

    private static void StyleButton(Button button, bool primary)
    {
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = primary ? 0 : 1;
        button.FlatAppearance.BorderColor = Color.FromArgb(70, 77, 84);
        button.FlatAppearance.MouseOverBackColor = primary ? Color.FromArgb(58, 178, 119) : Color.FromArgb(52, 58, 64);
        button.FlatAppearance.MouseDownBackColor = primary ? Color.FromArgb(37, 139, 89) : Color.FromArgb(31, 35, 39);
        button.BackColor = primary ? Color.FromArgb(50, 162, 105) : Color.FromArgb(39, 43, 47);
        button.ForeColor = Color.White;
        button.UseVisualStyleBackColor = false;
        button.Cursor = Cursors.Hand;
        button.Font = new Font("Segoe UI Semibold", 9F, FontStyle.Bold);
        ApplyRoundedCorners(button, 8);
    }

    private static void ApplyRoundedCorners(Control control, int radius)
    {
        Rectangle bounds = control.ClientRectangle;
        int diameter = radius * 2;
        if (bounds.Width < diameter || bounds.Height < diameter) return;
        using (GraphicsPath path = new GraphicsPath())
        {
            path.AddArc(bounds.X, bounds.Y, diameter, diameter, 180, 90);
            path.AddArc(bounds.Right - diameter, bounds.Y, diameter, diameter, 270, 90);
            path.AddArc(bounds.Right - diameter, bounds.Bottom - diameter, diameter, diameter, 0, 90);
            path.AddArc(bounds.X, bounds.Bottom - diameter, diameter, diameter, 90, 90);
            path.CloseFigure();
            control.Region = new Region(path);
        }
    }

    [DllImport("dwmapi.dll", PreserveSig = true)]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int valueSize);

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        try
        {
            int dark = 1, caption = ColorTranslator.ToWin32(Color.FromArgb(20, 22, 24));
            int text = ColorTranslator.ToWin32(Color.FromArgb(239, 242, 245));
            int rounded = 2;
            DwmSetWindowAttribute(Handle, 20, ref dark, sizeof(int));
            DwmSetWindowAttribute(Handle, 35, ref caption, sizeof(int));
            DwmSetWindowAttribute(Handle, 36, ref text, sizeof(int));
            DwmSetWindowAttribute(Handle, 33, ref rounded, sizeof(int));
        }
        catch { }
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
            RegisterSequenceFileAssociation();
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
            key.SetValue("DisplayVersion", "0.5.34");
            key.SetValue("Publisher", "StageTracks");
            key.SetValue("InstallLocation", installPath);
            key.SetValue("DisplayIcon", Path.Combine(installPath, "app-icon.ico"));
            key.SetValue("UninstallString", uninstall);
            key.SetValue("NoModify", 1, RegistryValueKind.DWord);
            key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        }
        finally { key.Close(); }
    }

    private void RegisterSequenceFileAssociation()
    {
        const string progId = "StageTracks.Sequence";
        const string extensionPath = "Software\\Classes\\.rpseq";
        const string backupPath = "Software\\StageTracks\\FileAssociationBackup";
        const string capabilitiesPath = "Software\\StageTracks\\Capabilities";
        string iconPath = Path.Combine(installPath, "app-icon.ico");
        string executable = Path.Combine(installPath, "StageTracks.exe");

        using (RegistryKey extension = Registry.CurrentUser.CreateSubKey(extensionPath))
        using (RegistryKey backup = Registry.CurrentUser.CreateSubKey(backupPath))
        {
            if (extension == null || backup == null) throw new InvalidOperationException("No se pudo registrar el tipo de archivo .rpseq.");
            if (backup.GetValue("Saved") == null)
            {
                object previous = extension.GetValue("");
                backup.SetValue("HadDefault", previous == null ? 0 : 1, RegistryValueKind.DWord);
                if (previous != null) backup.SetValue("OriginalDefault", previous.ToString());
                backup.SetValue("Saved", 1, RegistryValueKind.DWord);
            }
            extension.SetValue("", progId, RegistryValueKind.String);
            using (RegistryKey openWith = extension.CreateSubKey("OpenWithProgids"))
                if (openWith != null) openWith.SetValue(progId, "", RegistryValueKind.String);
        }

        using (RegistryKey type = Registry.CurrentUser.CreateSubKey("Software\\Classes\\" + progId))
        {
            if (type == null) throw new InvalidOperationException("No se pudo registrar StageTracks como aplicación para secuencias.");
            type.SetValue("", "StageTracks", RegistryValueKind.String);
            type.SetValue("FriendlyTypeName", "StageTracks", RegistryValueKind.String);
            using (RegistryKey icon = type.CreateSubKey("DefaultIcon"))
                if (icon != null) icon.SetValue("", iconPath + ",0", RegistryValueKind.String);
            using (RegistryKey command = type.CreateSubKey("shell\\open\\command"))
                if (command != null) command.SetValue("", "\"" + executable + "\" \"%1\"", RegistryValueKind.String);
        }

        using (RegistryKey registered = Registry.CurrentUser.CreateSubKey("Software\\RegisteredApplications"))
            if (registered != null) registered.SetValue("StageTracks", capabilitiesPath, RegistryValueKind.String);
        using (RegistryKey capabilities = Registry.CurrentUser.CreateSubKey(capabilitiesPath))
        {
            if (capabilities != null)
            {
                capabilities.SetValue("ApplicationName", "StageTracks", RegistryValueKind.String);
                capabilities.SetValue("ApplicationDescription", "Reproduce secuencias de StageTracks", RegistryValueKind.String);
                capabilities.SetValue("ApplicationIcon", iconPath + ",0", RegistryValueKind.String);
                using (RegistryKey associations = capabilities.CreateSubKey("FileAssociations"))
                    if (associations != null) associations.SetValue(".rpseq", progId, RegistryValueKind.String);
            }
        }

        SHChangeNotify(0x08000000, 0x0000, IntPtr.Zero, IntPtr.Zero);
    }

    [DllImport("shell32.dll", CharSet = CharSet.Auto, PreserveSig = true)]
    private static extern void SHChangeNotify(uint eventId, uint flags, IntPtr item1, IntPtr item2);

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
        setupCard.Visible = false;
        setupHeading.Visible = false;
        successPanel.Visible = true;
        successPanel.BringToFront();
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
        status.ForeColor = Color.FromArgb(255, 140, 140);
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
