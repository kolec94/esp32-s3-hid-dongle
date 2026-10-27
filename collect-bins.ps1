# Builds the firmware profiles and collects the flashable
# binaries into web\<board>\ for the ESP Web Tools installer.
#
# Requires arduino-cli + the esp32 core + Adafruit GFX/ST7735/ST7789 libraries.

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sketch = Join-Path $root 'firmware\keyboard-dongle'
# 16MB-flash boards (LilyGo, Geek) vs the 8MB AtomS3U
$fqbn16 = "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB,FlashSize=16M"
$fqbn8  = "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PartitionScheme=default_8MB,FlashSize=8M"

# boot_app0 ships with the esp32 core (same for every build)
$pkg = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\hardware\esp32'
$bootApp0 = Get-ChildItem -Path $pkg -Recurse -Filter 'boot_app0.bin' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $bootApp0) { throw "boot_app0.bin not found under $pkg" }

$boards = @{ '1' = 'lilygo'; '2' = 'geek'; '3' = 'atoms3u'; '4' = 'atoms3u-gamepad' }
foreach ($id in $boards.Keys) {
  $name = $boards[$id]
  $build = Join-Path $sketch "build-$name"
  $web   = Join-Path $root "web\$name"
  $fqbn  = if ($name -in @('atoms3u','atoms3u-gamepad')) { $fqbn8 } else { $fqbn16 }
  Write-Host "=== Building $name (BOARD=$id) ==="
  arduino-cli compile --fqbn $fqbn --build-property "compiler.cpp.extra_flags=-DBOARD=$id" --output-dir $build $sketch
  New-Item -ItemType Directory -Force $web | Out-Null
  Copy-Item (Join-Path $build '*.ino.bin')            (Join-Path $web 'firmware.bin')   -Force
  Copy-Item (Join-Path $build '*.ino.bootloader.bin') (Join-Path $web 'bootloader.bin') -Force
  Copy-Item (Join-Path $build '*.ino.partitions.bin') (Join-Path $web 'partitions.bin') -Force
  Copy-Item $bootApp0.FullName                        (Join-Path $web 'boot_app0.bin') -Force
}
Write-Host "Done. Each web firmware profile contains the 4 flashable binaries."
