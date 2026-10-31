param([switch]$Silent)
$ErrorActionPreference = 'Stop'
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\StageTracks'
if (-not $Silent) {
  Add-Type -AssemblyName PresentationFramework
  $answer = [System.Windows.MessageBox]::Show("Instalar StageTracks en:`n$installDir`n`n¿Continuar?", 'StageTracks Setup', 'YesNo', 'Question')
  if ($answer -ne 'Yes') { exit 0 }
}
$payload = Join-Path $PSScriptRoot 'StageTracks-portable-windows-x64.zip'
if (-not (Test-Path -LiteralPath $payload)) { throw 'No se encontró el paquete portable junto al instalador.' }
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Expand-Archive -LiteralPath $payload -DestinationPath $installDir -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination (Join-Path $installDir 'uninstall.ps1') -Force
$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\StageTracks'
New-Item -ItemType Directory -Force -Path $startMenu | Out-Null
$ws = New-Object -ComObject WScript.Shell
foreach ($target in @(@{Path=(Join-Path $startMenu 'StageTracks.lnk'); Target=(Join-Path $installDir 'StageTracks.exe')}, @{Path=(Join-Path ([Environment]::GetFolderPath('Desktop')) 'StageTracks.lnk'); Target=(Join-Path $installDir 'StageTracks.exe')})) {
  $shortcut = $ws.CreateShortcut($target.Path); $shortcut.TargetPath = $target.Target; $shortcut.WorkingDirectory = $installDir; $shortcut.IconLocation = (Join-Path $installDir 'app-icon.ico') + ',0'; $shortcut.Save()
}
if (-not $Silent) { [System.Windows.MessageBox]::Show('StageTracks quedó instalado. Los datos del usuario se guardan por separado en AppData.', 'StageTracks Setup', 'OK', 'Information') | Out-Null }
