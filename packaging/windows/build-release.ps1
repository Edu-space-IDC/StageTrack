param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = Join-Path $root 'build'
$stage = Join-Path $build 'StageTracks_release_stage'
$downloads = Join-Path $root 'release-artifacts'
$site = if (Test-Path -LiteralPath (Join-Path $root 'Pagina oficial/site/package.json')) {
    Join-Path $root 'Pagina oficial/site'
} else {
    $root
}
$publicDownloads = Join-Path $site 'public/downloads'
$reaperSource = Join-Path $root 'Reaper-Control-Studio-Reaper-files'
$studioHtml = Join-Path $root 'web/studio.html'
$reaperHtml = Join-Path $reaperSource 'Reaper Control Studio.html'
$controlStudioHtml = Join-Path $PSScriptRoot 'ControlStudio/REAPER_CONTROL_STUDIO_3.html'
$artifacts = Join-Path $build 'StageTracks_artefacts/Release'
& (Join-Path $root 'tools/generate_app_icon.ps1')
Copy-Item -LiteralPath (Join-Path $root 'assets/app-icon.ico') -Destination (Join-Path $PSScriptRoot 'ControlStudio/reaper.ico') -Force
Copy-Item -LiteralPath (Join-Path $root 'assets/app-icon.ico') -Destination (Join-Path $root 'assets/reaper.ico') -Force
Copy-Item -LiteralPath (Join-Path $root 'assets/app-icon.ico') -Destination (Join-Path $root 'assets/reaper.icos') -Force
Copy-Item -LiteralPath (Join-Path $root 'assets/app-icon.png') -Destination (Join-Path $root 'assets/reaper.png') -Force
Copy-Item -LiteralPath $studioHtml -Destination $reaperHtml -Force
Copy-Item -LiteralPath $studioHtml -Destination $controlStudioHtml -Force
cmake -S $root -B $build
if ($LASTEXITCODE -ne 0) { throw 'No se pudo actualizar la configuración/iconos de CMake.' }
cmake --build $build --config $Configuration --target StageTracks
if ($LASTEXITCODE -ne 0) { throw 'Falló la compilación de StageTracks.' }
$studioProject = Join-Path $PSScriptRoot 'ControlStudio/REAPERControlStudio.csproj'
$studioPublish = Join-Path $build 'ControlStudio_publish'
New-Item -ItemType Directory -Force -Path $studioPublish | Out-Null
dotnet publish $studioProject --configuration $Configuration --output $studioPublish
if ($LASTEXITCODE -ne 0) { throw 'Falló la compilación de Reaper Control Studio.' }
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, $downloads, $publicDownloads | Out-Null
Copy-Item -LiteralPath (Join-Path $artifacts 'StageTracks.exe') -Destination $stage
Copy-Item -LiteralPath (Join-Path $artifacts 'app-icon.png') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'assets/app-icon.ico') -Destination $stage
Copy-Item -LiteralPath (Join-Path $artifacts 'Pads') -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'community-config.json') -Destination $stage
@('StageTracks para Windows x64','','Portable: ejecuta StageTracks.exe desde esta carpeta. Conserva juntos Pads y app-icon.png.','Instalador: crea accesos directos en el escritorio y menú Inicio.','Los proyectos y ajustes del usuario se conservan en %APPDATA%\StageTracks.','Requisito: Windows 10/11 x64 con Microsoft Edge WebView2 Runtime.') | Set-Content -Encoding UTF8 (Join-Path $stage 'LEEME.txt')
$portable = Join-Path $downloads 'StageTracks-portable-windows-x64.zip'
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $portable -Force
$installerStage = Join-Path $build 'StageTracks_installer_stage'
if (Test-Path -LiteralPath $installerStage) { Remove-Item -LiteralPath $installerStage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $installerStage | Out-Null
Copy-Item -LiteralPath $portable -Destination $installerStage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install.ps1') -Destination $installerStage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination $installerStage
$targetName = Join-Path $downloads 'StageTracks-Setup-windows-x64.exe'
$csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if (-not (Test-Path $csc)) { $csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe' }
$installerIcon = Join-Path $root 'assets/app-icon.ico'
$studioExe = Join-Path $downloads 'Reaper Control Studio.exe'
$studioBuilt = Join-Path $studioPublish 'Reaper Control Studio.exe'
if (-not (Test-Path -LiteralPath $studioBuilt)) { throw 'No se encontró el ejecutable compilado de Reaper Control Studio.' }
Copy-Item -LiteralPath $studioBuilt -Destination $studioExe -Force
$compilerArgs = @('/nologo','/target:winexe','/codepage:65001',('/out:' + $targetName),('/win32icon:' + $installerIcon),'/reference:System.Windows.Forms.dll','/reference:System.Drawing.dll','/reference:System.dll','/reference:System.IO.Compression.dll','/reference:System.IO.Compression.FileSystem.dll',('/resource:' + $installerIcon + ',StageTracks.app-icon.ico'),('/resource:' + (Join-Path $root 'assets/app-icon.png') + ',StageTracks.app-icon.png'),('/resource:' + (Join-Path $installerStage 'StageTracks-portable-windows-x64.zip') + ',StageTracks.StageTracks-portable-windows-x64.zip'),('/resource:' + (Join-Path $installerStage 'uninstall.ps1') + ',StageTracks.uninstall.ps1'),(Join-Path $PSScriptRoot 'InstallerBootstrap.cs'))
$compilerOutput = & $csc @compilerArgs 2>&1
$compilerExitCode = $LASTEXITCODE
if ($compilerExitCode -ne 0 -or -not (Test-Path $targetName)) {
    $compilerDiagnostic = ($compilerOutput | Out-String).Trim()
    throw "No se pudo compilar el instalador de StageTracks (csc exit code $compilerExitCode). $compilerDiagnostic"
}
[ordered]@{version='0.5.9';portable='/downloads/StageTracks-portable-windows-x64.zip';installer='/downloads/StageTracks-Setup-windows-x64.exe';installerUrl='https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.9/StageTracks-Setup-windows-x64.exe';reaperStudioExe='https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.9/Reaper.Control.Studio.exe';updated=(Get-Date).ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $publicDownloads 'release.json')
$publicAssets = Join-Path $site 'public/assets'
Copy-Item -LiteralPath (Join-Path $root 'assets/app-icon.png') -Destination (Join-Path $publicAssets 'app-icon.png') -Force
Copy-Item -LiteralPath (Join-Path $root 'assets/app-icon.png') -Destination (Join-Path $reaperSource 'app-icon.png') -Force
Push-Location $site
try {
    npm run build
    if ($LASTEXITCODE -ne 0) { throw 'No se pudo compilar el sitio oficial.' }
} finally { Pop-Location }
$reaperZip = Join-Path $publicDownloads 'Reaper-Control-Studio-Reaper-files.zip'
Compress-Archive -Path (Join-Path $reaperSource '*') -DestinationPath $reaperZip -Force
Write-Host "Paquetes listos: $downloads"
