# Подготовка self-hosted GitHub Actions runner для сборки tdesktop под x64.
# Запускать ВНУТРИ свежеустановленной Windows Server / 11 Pro VM, от админа.
#
# Что делает:
#   1. Ставит chocolatey + Git + Python 3.11 + CMake + 7-Zip + NSIS.
#   2. Ставит VS 2022 Build Tools с C++ workload (~25 ГБ).
#   3. Качает actions-runner, конфигурирует под репу tdesktop, ставит как сервис.
#
# Перед запуском задайте две переменные:
#   $env:GH_REPO   — например 'https://github.com/zavolo/tdesktop'
#   $env:GH_TOKEN  — registration token со страницы
#                    Settings → Actions → Runners → New self-hosted runner

$ErrorActionPreference = 'Stop'

if (-not $env:GH_REPO -or -not $env:GH_TOKEN) {
    Write-Host "ERROR: set GH_REPO and GH_TOKEN env vars before running." -ForegroundColor Red
    Write-Host "  `$env:GH_REPO  = 'https://github.com/zavolo/tdesktop'"
    Write-Host "  `$env:GH_TOKEN = 'ABCDEFG...'  (Settings -> Actions -> Runners -> New)"
    exit 1
}

# --- 1. Chocolatey + базовые тулзы -----------------------------------------
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Chocolatey..."
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = 3072
    iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    $env:Path += ";$env:ProgramData\chocolatey\bin"
}

Write-Host "Installing git/python/cmake/7zip/nsis..."
choco install -y --no-progress git python311 cmake 7zip nsis
# Обновляю PATH в текущей сессии — иначе следующие шаги не увидят git/python.
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("Path","User")

# --- 2. VS 2022 Build Tools с C++ workload --------------------------------
$vsInstaller = "$env:TEMP\vs_buildtools.exe"
if (-not (Test-Path $vsInstaller)) {
    Write-Host "Downloading VS Build Tools installer..."
    Invoke-WebRequest 'https://aka.ms/vs/17/release/vs_buildtools.exe' -OutFile $vsInstaller
}
$vsArgs = @(
    '--quiet','--wait','--norestart','--nocache',
    '--add','Microsoft.VisualStudio.Workload.VCTools',
    '--add','Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '--add','Microsoft.VisualStudio.Component.VC.ATL',
    '--add','Microsoft.VisualStudio.Component.VC.ATLMFC',
    '--add','Microsoft.VisualStudio.Component.Windows11SDK.22621',
    '--includeRecommended'
)
Write-Host "Installing VS Build Tools (~25 GB, занимает 15-30 минут)..."
$proc = Start-Process -FilePath $vsInstaller -ArgumentList $vsArgs -Wait -PassThru
if ($proc.ExitCode -ne 0 -and $proc.ExitCode -ne 3010) {
    throw "VS Build Tools installer exited with $($proc.ExitCode)"
}

# --- 3. actions-runner -----------------------------------------------------
$runnerDir = 'C:\actions-runner'
New-Item -ItemType Directory -Force -Path $runnerDir | Out-Null
Set-Location $runnerDir

# Узнаю свежий URL: API GitHub отдаёт последний релиз. Грубо: фикс v2.319.x
# работает и сейчас, но беру latest, если есть интернет.
$runnerVer = '2.323.0'
try {
    $latest = (Invoke-RestMethod 'https://api.github.com/repos/actions/runner/releases/latest').tag_name -replace '^v',''
    if ($latest) { $runnerVer = $latest }
} catch { Write-Host "Couldn't query latest runner version, using $runnerVer" }

$runnerZip = "actions-runner-win-x64-$runnerVer.zip"
if (-not (Test-Path $runnerZip)) {
    Write-Host "Downloading runner $runnerVer..."
    Invoke-WebRequest "https://github.com/actions/runner/releases/download/v$runnerVer/$runnerZip" -OutFile $runnerZip
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path $runnerZip), (Get-Location).Path)

# Конфигурация. --labels добавляю чтобы матчить workflow.
$runnerName = "$env:COMPUTERNAME-tdesktop"
$labels = 'self-hosted,Windows,X64,tdesktop'
& .\config.cmd --unattended --url $env:GH_REPO --token $env:GH_TOKEN `
    --name $runnerName --labels $labels --replace
if ($LASTEXITCODE -ne 0) { throw "runner config failed ($LASTEXITCODE)" }

# Ставлю как сервис, чтобы крутился без логина.
& .\svc.cmd install
& .\svc.cmd start

Write-Host ""
Write-Host "Done. Runner '$runnerName' зарегистрирован для $env:GH_REPO." -ForegroundColor Green
Write-Host "Поменяйте в windows.yml: runs-on: [self-hosted, Windows, X64, tdesktop]"
