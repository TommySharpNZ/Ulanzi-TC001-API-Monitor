# Release Guide for Developers

Quick reference for creating a new firmware release with web installer support.

## Prerequisites

- Arduino IDE installed with ESP32 support
- All required libraries installed
- Code tested and ready for release

## Step-by-Step Release Process

### 1. Update Version Number

Update the version in `Ulanzi-TC001-API-Monitor.ino`:
```cpp
String buildNumber = "v1.0.8";  // Update this line
```

### 2. Export Binaries from Arduino IDE

1. Open `Ulanzi-TC001-API-Monitor.ino` in Arduino IDE
2. Set board configuration:
   - **Tools → Board → ESP32 Arduino → ESP32 Dev Module**
   - **Tools → Partition Scheme → Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**
   - **Tools → Flash Size → 4MB (32Mb)**
   - **Tools → Flash Mode → QIO**
   - **Tools → Flash Frequency → 80MHz**
3. **Sketch → Export Compiled Binary** (Ctrl+Alt+S)
4. Wait for compilation to complete

This creates in your sketch folder:
- `Ulanzi-TC001-API-Monitor.ino.bin`
- `Ulanzi-TC001-API-Monitor.ino.bootloader.bin`
- `Ulanzi-TC001-API-Monitor.ino.partitions.bin`

### 3. Locate boot_app0.bin

Find this file in your Arduino ESP32 installation:

**Windows:**
```
C:\Users\<YourUsername>\AppData\Local\Arduino15\packages\esp32\hardware\esp32\2.0.x\tools\partitions\boot_app0.bin
```

**Mac:**
```
~/Library/Arduino15/packages/esp32/hardware/esp32/2.0.x/tools/partitions/boot_app0.bin
```

**Linux:**
```
~/.arduino15/packages/esp32/hardware/esp32/2.0.x/tools/partitions/boot_app0.bin
```

### 4. Copy Files to Web Installer

```bash
# Navigate to project root
cd /path/to/Ulanzi-TC001-API-Monitor

# Copy and rename application binary
cp Ulanzi-TC001-API-Monitor.ino.bin docs/firmware/Ulanzi-TC001-API-Monitor.bin

# Copy bootloader
cp Ulanzi-TC001-API-Monitor.ino.bootloader.bin docs/firmware/bootloader.bin

# Copy partition table
cp Ulanzi-TC001-API-Monitor.ino.partitions.bin docs/firmware/partitions.bin

# Copy boot_app0.bin (adjust path to your Arduino15 location)
# Windows example:
cp C:/Users/YourUsername/AppData/Local/Arduino15/packages/esp32/hardware/esp32/2.0.17/tools/partitions/boot_app0.bin docs/firmware/boot_app0.bin
```

### 5. Update Documentation

Update these files with the new version number:

**docs/manifest.json:**
```json
{
  "name": "TC001 API Monitor",
  "version": "1.0.8",   ← Update this
  ...
}
```

**docs/index.html:**
```html
<title>TC001 API Monitor - Web Installer</title>
...
<p class="version">Firmware Version 1.0.8</p>   ← Update this
...
<button slot="activate">Install Firmware v1.0.8</button>   ← Update this
```

**README.md:**
- Add new "What's New in vX.X.X" section at the top
- Update version in web installer link text if needed

### 6. Test Locally

```bash
# Start local web server
cd docs
python -m http.server 8000

# Open browser to http://localhost:8000
# Test installation on actual TC001 device
# Verify settings are preserved
```

### 7. Commit and Push

```bash
git add .
git commit -m "Release v1.0.8 - Description of changes"
git tag v1.0.8
git push origin main
git push origin v1.0.8
```

### 8. Verify GitHub Pages

1. Go to repository **Settings → Pages**
2. Confirm source is set to **main branch** → **/docs folder**
3. Wait 1-2 minutes for deployment
4. Visit: https://tommysharpnz.github.io/Ulanzi-TC001-API-Monitor/
5. Test web installer with real device

## File Size Reference

Typical file sizes (may vary by version):
- `Ulanzi-TC001-API-Monitor.bin`: ~1.2 MB (application)
- `bootloader.bin`: ~24 KB
- `partitions.bin`: ~3 KB
- `boot_app0.bin`: ~4 KB

## Memory Layout

```
Address    Size        Contents
0x1000     ~24 KB      bootloader.bin
0x8000     ~3 KB       partitions.bin
0xE000     ~4 KB       boot_app0.bin
0x10000    ~1.2 MB     Ulanzi-TC001-API-Monitor.bin (app)
0x9000     variable    NVS partition (user settings - NOT overwritten)
```

## Troubleshooting

### "Binary too large" error
- Check partition scheme is set to "Default 4MB with spiffs"
- Reduce code size or remove debug statements

### Web installer fails
- Verify all 4 binary files are in `docs/firmware/`
- Check manifest.json paths are correct
- Ensure browser supports Web Serial API (Chrome/Edge)

### Settings not preserved
- Verify `"new_install_prompt_erase": false` in manifest.json
- NVS partition should NOT be in the parts list

## Quick Commands Reference

```bash
# Export from Arduino IDE location to docs/firmware
# Adjust paths for your system

# Windows PowerShell
Copy-Item "Ulanzi-TC001-API-Monitor.ino.bin" "docs\firmware\Ulanzi-TC001-API-Monitor.bin"
Copy-Item "Ulanzi-TC001-API-Monitor.ino.bootloader.bin" "docs\firmware\bootloader.bin"
Copy-Item "Ulanzi-TC001-API-Monitor.ino.partitions.bin" "docs\firmware\partitions.bin"
Copy-Item "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\2.0.17\tools\partitions\boot_app0.bin" "docs\firmware\boot_app0.bin"

# Mac/Linux
cp Ulanzi-TC001-API-Monitor.ino.bin docs/firmware/Ulanzi-TC001-API-Monitor.bin
cp Ulanzi-TC001-API-Monitor.ino.bootloader.bin docs/firmware/bootloader.bin
cp Ulanzi-TC001-API-Monitor.ino.partitions.bin docs/firmware/partitions.bin
cp ~/Library/Arduino15/packages/esp32/hardware/esp32/2.0.17/tools/partitions/boot_app0.bin docs/firmware/boot_app0.bin
```

## Checklist

Before releasing:
- [ ] Version updated in .ino file
- [ ] Code compiled without errors
- [ ] All 4 binary files copied to docs/firmware/
- [ ] Version updated in manifest.json
- [ ] Version updated in index.html (2 places)
- [ ] README.md updated with changes
- [ ] Tested locally with real device
- [ ] Settings preserved after update
- [ ] Committed and pushed to GitHub
- [ ] Git tag created
- [ ] GitHub Pages deployment verified
- [ ] Web installer tested from live URL
