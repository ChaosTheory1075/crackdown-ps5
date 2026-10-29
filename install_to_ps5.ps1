# Copy Crackdown: ReActivated (the app folder) and your game files to the PS5 over FTP.
#
#   .\install_to_ps5.ps1 -AppDir <downloaded PPSA53070 folder>
#
# Uploads, under /mnt/ext1/homebrew/PPSA53070/:
#   the app folder itself (eboot.bin, sce_sys, sce_module, ...),
#   game/          the disc's files (Crackdown\Crackdown), with Title Update 6's
#                  Default.xexp beside default.xex (also as default.xexp) and
#                  its other files (Local/*/GameStrings_DLC.str) in game/update/,
#   dlc/<package>/ each DLC package (DLC\), extracted,
#   hub/music/     the menu's song (the .mp3 in the game folder), as menu.mp3.
# -SkipGame uploads only the app (for a new build over an existing install).
param(
    [Parameter(Mandatory = $true)][string]$AppDir,
    # (a folder with Crackdown\ (the disc's files), Title Update\ (TU6's package), DLC\ and a menu .mp3)
    [Parameter(Mandatory = $true)][string]$GameDir,
    [Parameter(Mandatory = $true)][string]$Ps5,
    [int]$Port = 2121,
    [string]$Dest = "/mnt/ext1/homebrew",
    [string]$TitleId = "PPSA53070",
    [switch]$SkipGame,
    [switch]$SkipDlc
)
$ErrorActionPreference = "Stop"
$base = "ftp://${Ps5}:${Port}${Dest}/${TitleId}"
$stfs = Join-Path $PSScriptRoot "tools\stfs_extract.py"

function Send-Tree([string]$From, [string]$To, [string[]]$Skip = @()) {
    $root = (Resolve-Path $From).Path.TrimEnd('\')
    # (top-level folders and files named in $Skip are left out)
    $files = Get-ChildItem $root -Recurse -File | Where-Object {
        $Skip -notcontains $_.FullName.Substring($root.Length + 1).Split('\')[0]
    }
    $i = 0
    foreach ($f in $files) {
        $i++
        $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
        $url = "$base/$To$rel" -replace '(?<!:)//', '/'
        Write-Host ("[{0}/{1}] {2} ({3:N0} MB)" -f $i, $files.Count, "$To$rel", ($f.Length / 1MB))
        & curl.exe -s -S --ftp-create-dirs -T $f.FullName $url
        if ($LASTEXITCODE -ne 0) { throw "upload failed: $($f.FullName)" }
    }
}

function Send-File([string]$From, [string]$To) {
    Write-Host "  $To"
    & curl.exe -s -S --ftp-create-dirs -T $From "$base/$To"
    if ($LASTEXITCODE -ne 0) { throw "upload failed: $From" }
}

& curl.exe -s --max-time 8 "ftp://${Ps5}:${Port}${Dest}/" > $null
if ($LASTEXITCODE -ne 0) { throw "The PS5's FTP server at ${Ps5}:${Port} doesn't answer. Is the console on with the FTP server running?" }

Write-Host "==> App folder"
# (without the game, the DLC or your data, which may be in it too: game/ and dlc/ come from
# $GameDir below, and the saves, trophies and settings on the PS5 stay as they are)
Send-Tree $AppDir "" @("game", "dlc", "saves", "UserData", "logs", "menu.ini")

if (-not $SkipGame) {
    Write-Host "==> Disc files -> game/"
    Send-Tree (Join-Path $GameDir "Crackdown") "game/"

    $tu = Join-Path $env:TEMP "crackdown-tu6"
    if (Test-Path $tu) { Remove-Item -Recurse -Force $tu }
    $pkg = Get-ChildItem (Join-Path $GameDir "Title Update") -File | Select-Object -First 1
    & py -3 -I $stfs $pkg.FullName $tu
    if ($LASTEXITCODE -ne 0) { throw "couldn't extract the title update" }
    $xexp = Join-Path $tu "Default.xexp"
    $hash = (Get-FileHash $xexp -Algorithm SHA256).Hash
    if ($hash -ne "A0D4FE06D6A4A2FD96D18F5DE426F761FE1B8CCE5D5BC7F981D738C3519C819E") {
        throw "$($pkg.Name) is not Title Update 6 (Default.xexp $hash)"
    }
    Write-Host "==> Title Update 6 -> game/Default.xexp, game/default.xexp, game/update/"
    Send-File $xexp "game/Default.xexp"
    Send-File $xexp "game/default.xexp"
    Remove-Item $xexp
    Send-Tree $tu "game/update/"
}

if (-not $SkipDlc) {
    # The runtime mounts each package as a folder of its files, named as the
    # package: dlc/<package file name>/ (as the Xbox 360's content folders).
    Write-Host "==> DLC -> dlc/<package>/"
    $dlc = Join-Path $env:TEMP "crackdown-dlc"
    if (Test-Path $dlc) { Remove-Item -Recurse -Force $dlc }
    foreach ($p in Get-ChildItem (Join-Path $GameDir "DLC") -File) {
        & py -3 -I $stfs $p.FullName (Join-Path $dlc $p.Name) | Select-Object -First 1
        if ($LASTEXITCODE -ne 0) { throw "couldn't extract DLC package $($p.Name)" }
    }
    Send-Tree $dlc "dlc/"
}

$song = Get-ChildItem $GameDir -File -Filter *.mp3 | Select-Object -First 1
if ($song) {
    Write-Host "==> Menu music -> hub/music/menu.mp3"
    Send-File $song.FullName "hub/music/menu.mp3"
}

Write-Host "==> Done: launch Crackdown: ReActivated from the PS5's home screen. Logs: $Dest/$TitleId/logs/"
