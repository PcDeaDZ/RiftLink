# Однократное исправление кодировки после клонирования
# .ps1: UTF-8 с BOM, CRLF (PowerShell на Windows)
# .sh: UTF-8 без BOM, LF (bash)
$root = if ($PSScriptRoot) { (Get-Item $PSScriptRoot).Parent.FullName } else { Get-Location }
$utf8Bom = [System.Text.UTF8Encoding]::new($true)
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

# Скрипты проекта (относительно корня)
$ps1Files = @(
    "build.ps1",
    "install.ps1",
    "scripts\fix_encoding.ps1"
)
$shFiles = @("build.sh", "install.sh")

foreach ($f in $ps1Files) {
    $path = Join-Path $root $f
    if (Test-Path $path) {
        $content = [IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
        [IO.File]::WriteAllText($path, ($content -replace "`r?`n", "`r`n"), $utf8Bom)
        Write-Host "OK: $f (UTF-8 BOM, CRLF)"
    }
}
foreach ($f in $shFiles) {
    $path = Join-Path $root $f
    if (Test-Path $path) {
        $content = [IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
        [IO.File]::WriteAllText($path, ($content -replace "`r`n", "`n"), $utf8NoBom)
        Write-Host "OK: $f (UTF-8, LF)"
    }
}
