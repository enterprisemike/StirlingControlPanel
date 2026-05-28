param(
  [ValidateSet("small", "large")]
  [string]$Display = "large"
)

$platformioPath = Join-Path $HOME ".platformio/penv/bin/platformio"
if (-not (Test-Path $platformioPath)) {
  $platformioPath = "platformio"
}

$environmentName = switch ($Display) {
  "small" { "esp32s3dev-small" }
  "large" { "esp32s3dev-large" }
}

Push-Location (Join-Path $PSScriptRoot "..")
try {
  Write-Host "Uploading firmware for $Display OLED ($environmentName)..."
  & $platformioPath run -e $environmentName --target upload
} finally {
  Pop-Location
}