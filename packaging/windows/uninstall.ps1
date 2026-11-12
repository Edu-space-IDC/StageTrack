$ErrorActionPreference = 'Stop'
$installDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\StageTracks'
$extensionPath = 'HKCU:\Software\Classes\.rpseq'
$openWithPath = Join-Path $extensionPath 'OpenWithProgids'
$progId = 'StageTracks.Sequence'
$backupPath = 'HKCU:\Software\StageTracks\FileAssociationBackup'
if (Test-Path -LiteralPath $extensionPath) {
  $current = (Get-Item -LiteralPath $extensionPath).GetValue('')
  if ($current -eq $progId) {
    $backup = Get-ItemProperty -LiteralPath $backupPath -ErrorAction SilentlyContinue
    if ($backup -and $backup.HadDefault -eq 1) { Set-Item -LiteralPath $extensionPath -Value $backup.OriginalDefault }
    else { Remove-ItemProperty -LiteralPath $extensionPath -Name '(default)' -ErrorAction SilentlyContinue }
  }
}
Remove-ItemProperty -LiteralPath $openWithPath -Name $progId -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "HKCU:\Software\Classes\$progId" -Recurse -Force -ErrorAction SilentlyContinue
Remove-ItemProperty -LiteralPath 'HKCU:\Software\RegisteredApplications' -Name 'StageTracks' -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'HKCU:\Software\StageTracks\Capabilities' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $backupPath -Recurse -Force -ErrorAction SilentlyContinue
try {
  Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class StageTracksShellRefresh { [DllImport("shell32.dll")] public static extern void SHChangeNotify(uint eventId, uint flags, IntPtr item1, IntPtr item2); }' -ErrorAction Stop
  [StageTracksShellRefresh]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
} catch { }
Remove-Item -LiteralPath $startMenu -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('Desktop')) 'StageTracks.lnk') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\StageTracks' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $installDir -Recurse -Force
