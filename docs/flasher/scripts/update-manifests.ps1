param(
  [Parameter(Mandatory = $true)]
  [string]$Version,

  [string]$Repo = "PcDeaDZ/RiftLink"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$manifests = @{
  "heltec-v3.json"           = "../firmware/heltec_v3_full.bin"
  "heltec-v4.json"           = "../firmware/heltec_v4_full.bin"
  "heltec-v3-paper.json"     = "../firmware/heltec_v3_paper_full.bin"
  "lilygo-t-lora-pager.json" = "../firmware/lilygo_t_lora_pager_full.bin"
  "lilygo-t-beam.json"       = "../firmware/lilygo_t_beam_full.bin"
}

foreach ($file in $manifests.Keys) {
  $path = Join-Path $root "manifests\$file"
  if (-not (Test-Path $path)) {
    Write-Warning "Пропущен отсутствующий файл: $path"
    continue
  }

  $json = Get-Content -Path $path -Raw | ConvertFrom-Json
  $json.version = $Version
  $json.builds[0].parts[0].path = $manifests[$file]

  $json | ConvertTo-Json -Depth 20 | Set-Content -Path $path -Encoding utf8
  Write-Host "Обновлен: $file"
}

Write-Host "Готово. Проверьте изменения в docs/flasher/manifests/"

