# Однократное исправление кодировки после клонирования
# build.ps1: UTF-8 с BOM (PowerShell на Windows)
# build.sh: UTF-8 без BOM, LF (bash)
$root = (Get-Item $PSScriptRoot).Parent.FullName
$utf8Bom = [System.Text.UTF8Encoding]::new($true)
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$ps1 = [IO.File]::ReadAllText("$root\build.ps1", [System.Text.Encoding]::UTF8)
$sh = [IO.File]::ReadAllText("$root\build.sh", [System.Text.Encoding]::UTF8)
[IO.File]::WriteAllText("$root\build.ps1", ($ps1 -replace "`r?`n", "`r`n"), $utf8Bom)
[IO.File]::WriteAllText("$root\build.sh", ($sh -replace "`r`n", "`n"), $utf8NoBom)
Write-Host "OK: build.ps1 (UTF-8 BOM, CRLF), build.sh (UTF-8, LF)"
