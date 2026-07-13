// ═══════════════════════════════════════════════════════════════════════
//  BLINDEYE ESP v2 - INTEGRATION & DEPLOYMENT GUIDE
//  Complete setup for kernel-assisted anti-cheat evasion
// ═══════════════════════════════════════════════════════════════════════

# BlindEye ESP v2 - Complete Setup Guide

## Overview

This is a **kernel-assisted ESP overlay** for Arma Reforger that integrates with the BlindEye kernel driver to evade anti-cheat detection. The system operates in two layers:

1. **Kernel Driver (BlindEye.sys)** — Silences BattlEye memory allocation reports
2. **User-Mode Overlay (ESP.exe)** — Renders game entities with kernel-assisted memory reads

---

## Architecture

```
┌─────────────────────────────────────────┐
│     Arma Reforger Process (Game)        │
│  ├─ Game Memory                         │
│  ├─ Entities, Camera, Positions         │
│  └─ Protected by BattlEye               │
└─────────────────────────────────────────┘
            ↓ (Kernel Device Interface)
┌─────────────────────────────────────────┐
│     BlindEye Kernel Driver              │
│  ├─ IOCTL Memory Read Interface         │
│  ├─ Hooks ExAllocatePool*               │
│  └─ Blocks BattlEye Reports             │
└─────────────────────────────────────────┘
            ↑ (DeviceIoControl)
┌─────────────────────────────────────────┐
│     ESP Overlay (User-Mode)             │
│  ├─ Reads Memory via Kernel Driver      │
│  ├─ 3D-to-2D Projection                 │
│  └─ Renders Transparent Overlay         │
└─────────────────────────────────────────┘
```

---

## Prerequisites

### Hardware
- **OS:** Windows 10/11 x64
- **RAM:** 8GB minimum
- **Virtualization:** For testing (Hyper-V, VirtualBox)

### Software
- **Visual Studio 2022** (or 2019+)
  - Desktop development with C++
  - Windows SDK (latest)
  - WDK (Windows Driver Kit 10+)
- **Administrator access** (required to load kernel drivers)

### Downloads
1. [Visual Studio 2022 Community](https://visualstudio.microsoft.com/downloads/)
2. [Windows Driver Kit](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
3. [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/)

---

## Step 1: Install Prerequisites

### 1a. Install Visual Studio 2022
```bash
# Run installer
vs_community.exe

# During setup, select:
✓ Desktop development with C++
✓ Windows SDK (latest available)
✓ C++ v143 toolset
```

### 1b. Install Windows Driver Kit
```bash
# Download and run WDK installer
# This adds kernel-mode development templates to Visual Studio
```

### 1c. Install Windows SDK
```bash
# If not included in Visual Studio setup, install separately
# Download from: https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/
```

---

## Step 2: Clone & Open Project

```bash
# Clone the repository
git clone https://github.com/christopherzx2578-web/BlindEye.git
cd BlindEye

# Switch to integration branch
git checkout feature/kernel-esp-integration

# Open solution in Visual Studio
start BlindEye.sln
```

---

## Step 3: Build the Kernel Driver

### Build Configuration
```
Platform: x64 (64-bit)
Configuration: Release (for production)
```

### Build Steps

1. **In Visual Studio:**
   - Right-click `BlindEye` project → **Properties**
   - Verify: 
     - **Configuration:** Release|x64
     - **Platform Toolset:** WindowsKernelModeDriver10.0

2. **Build:**
   ```
   Build → Build Solution (or Ctrl+Shift+B)
   ```

3. **Output Location:**
   ```
   BlindEye\x64\Release\BlindEye.sys
   ```

4. **Verify Build Success:**
   - Check Output pane for: `Build succeeded`
   - No C1001, C2439, or linker errors

---

## Step 4: Build the ESP Overlay

### Add ESP Project to Solution

1. **Right-click Solution → Add → Existing Project**
2. **Navigate to:** `ESP/ESP.vcxproj`
3. **Click Open**

### Build Configuration
```
Platform: x64
Configuration: Release
```

### Build Steps

1. **Right-click ESP project → Set as Startup Project**
2. **Build → Build ESP (or Ctrl+Shift+B)**
3. **Output Location:**
   ```
   ESP\x64\Release\ESP.exe
   ```

---

## Step 5: Sign the Kernel Driver (Windows 11 only)

Windows 11 requires signed drivers. You have options:

### Option A: Self-Sign (Development Only)
```powershell
# Run as Administrator
cd "C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64"

# Create certificate
makecert -r -pe -n "CN=BlindEye Dev" -ss PrivateCertStore -sr LocalMachine certificate.cer

# Sign driver
signtool sign /f certificate.cer /fd sha256 /t http://timestamp.digicert.com "C:\path\to\BlindEye.sys"
```

### Option B: Enable Test Signing Mode
```powershell
# Run as Administrator
bcdedit /set testsigning on

# Reboot required
# To disable: bcdedit /set testsigning off
```

### Option C: Use production certificate
Contact your CA or use an EV certificate from Sectigo, DigiCert, etc.

---

## Step 6: Prepare Deployment

### Directory Structure
```
Deploy/
  ├── BlindEye.sys          (Kernel driver)
  ├── ESP.exe               (Overlay)
  ├── install_driver.ps1    (Installation script)
  └── README_DEPLOY.txt
```

### Create Installation Script

**File: `install_driver.ps1`**
```powershell
# Run as Administrator
param([string]$DriverPath)

if (-not ([Security.Principal.WindowsIdentity]::GetCurrent().Groups -contains 'S-1-5-32-544')) {
    Write-Host "ERROR: Must run as Administrator"
    exit 1
}

# Copy driver to System32
$SysPath = "C:\Windows\System32\drivers\BlindEyeESP.sys"
Copy-Item $DriverPath $SysPath -Force
Write-Host "Driver copied to $SysPath"

# Create registry entries
reg add "HKLM\System\CurrentControlSet\Services\BlindEyeESP" /v Type /t REG_DWORD /d 1 /f
reg add "HKLM\System\CurrentControlSet\Services\BlindEyeESP" /v Start /t REG_DWORD /d 3 /f
reg add "HKLM\System\CurrentControlSet\Services\BlindEyeESP" /v ImagePath /t REG_SZ /d $SysPath /f

Write-Host "Driver registered in registry"

# Load driver
net start BlindEyeESP
if ($LASTEXITCODE -eq 0) {
    Write-Host "Driver loaded successfully"
} else {
    Write-Host "Failed to load driver"
    exit 1
}
```

---

## Step 7: Deploy & Test

### On Target Machine (Windows 10/11 x64)

```powershell
# 1. Run as Administrator
# 2. Enable test signing (Windows 11):
bcdedit /set testsigning on

# 3. Reboot
Restart-Computer

# 4. After reboot, run installation script
.\install_driver.ps1 -DriverPath ".\BlindEye.sys"

# 5. Verify driver loaded
Get-Service BlindEyeESP
# Should show: Running

# 6. Check device exists
Get-Item \\?\BlindEyeESP
# Should show no error
```

### Launch ESP Overlay

```bash
# Launch Arma Reforger first
# Then run ESP.exe from the deployment folder
ESP.exe
```

### Verify Integration

When ESP starts, you should see in the top-left corner:
```
[REFORGER ESP v2]  Kernel-Assisted Mode
```

If it says "User-Mode Fallback", the driver isn't loaded.

---

## Step 8: Verify Anti-Cheat Evasion

### Check BattlEye Reports Are Blocked

1. **Launch the game with driver active**
2. **Run ESP overlay**
3. **Check Process Monitor (Procmon):**
   - BEDaisy.sys should NOT be reporting memory allocations
   - Our hooked ExAllocatePool should be dropping them

### Use Diagnostic Tools

```bash
# Check driver status
driverquery | findstr BlindEye

# Monitor system calls
# Use DebugView to see driver debug messages
# Administrator CMD:
DbgView.exe
# You should see: "ExAllocatePoolWithTag called from: 0xXXXX rejected!"
```

---

## Troubleshooting

### Issue: "Device not found" (Kernel driver won't load)

**Solution:**
```powershell
# 1. Check test signing is enabled (Windows 11)
bcdedit /enum | findstr "testsigning"
# Output should show: testsigning Yes

# 2. Verify driver was signed
signtool verify /pa BlindEye.sys

# 3. Manually load using OSR Loader:
# https://www.osronline.com/article.cfm?article=629
```

### Issue: "Access Denied" when launching ESP

**Solution:**
```bash
# Run ESP.exe as Administrator
# Right-click ESP.exe → Properties
# → Compatibility tab
# → Check "Run this program as an administrator"
```

### Issue: ESP overlay not rendering / no entities showing

**Solution:**
```
1. Verify game is running
2. Check game process name: "ArmaReforgerSteam.exe" or "ArmaReforger.exe"
3. Look at ESP console output for base address scan results
4. Ensure game is the foreground window
5. Check overlay window is above game window
```

### Issue: Signature scan fails, using fallback offsets

**Solution:**
```
Offsets can change between game patches. To update:
1. Analyze new game binary with IDA Pro
2. Find new signatures using SigScan
3. Update pattern strings in ESP.cpp
4. Rebuild ESP.exe
```

---

## Performance Tuning

### Reduce Latency
```cpp
// In ESP.cpp, adjust sleep times:
Sleep(jit(7,12));      // Lower = more CPU usage, lower latency
// Try: Sleep(jit(4,8));
```

### Optimize Memory Reads
```cpp
// Use kernel reads (faster when driver is loaded)
g_useKernel = true;    // Enabled by default
```

### Limit ESP Range
```cpp
g_feat.maxDist = 800;  // Reduce to 500-600 for performance
```

---

## Security Best Practices

1. **Never share compiled binaries** — Signatures change between patches
2. **Recompile regularly** — Update sigs after game patches
3. **Test in isolated environment** — Use VM for testing
4. **Disable test signing in production** — bcdedit /set testsigning off
5. **Remove driver before competitive play** — Reduces detection window
6. **Obfuscate source code** — Use tools like Themida, VMProtect

---

## Uninstall

```powershell
# Stop driver
net stop BlindEyeESP

# Remove from registry
reg delete "HKLM\System\CurrentControlSet\Services\BlindEyeESP" /f

# Delete driver file
Remove-Item "C:\Windows\System32\drivers\BlindEyeESP.sys" -Force

# Disable test signing (if enabled)
bcdedit /set testsigning off

# Reboot
Restart-Computer
```

---

## File Structure Summary

```
BlindEye/
├── BlindEye/                    # Kernel driver
│   ├── DriverEntry.cpp         # Driver entry + IOCTL handlers
│   ├── DriverUtil.h/cpp        # Logging & utilities
│   ├── Hooks.h/cpp             # Memory allocation hooks
│   ├── Memory.h/cpp            # Memory utilities
│   ├── Types.h                 # Type definitions
│   ├── SharedTypes.h           # Kernel/User shared types
│   └── BlindEye.vcxproj        # Driver project
│
├── ESP/                         # User-mode overlay
│   ├── ESP.cpp                 # Main overlay application
│   └── ESP.vcxproj             # ESP project
│
├── BlindEye.sln                # Visual Studio solution
├── LICENSE                     # MIT License
└── README.md                   # Project overview
```

---

## Additional Resources

- [Windows Driver Kit Docs](https://docs.microsoft.com/en-us/windows-hardware/drivers/)
- [GDI+ Reference](https://docs.microsoft.com/en-us/windows/win32/gdiplus/-gdiplus-gdi-start)
- [Windows API Reference](https://docs.microsoft.com/en-us/windows/win32/api/)
- [BattlEye Anti-Cheat](https://www.battleyemaster.com/)

---

## Support & Debugging

### Enable Debug Output

Modify `DriverUtil.h` to add console output:
```cpp
#define DBG_PRINT(fmt, ...) DbgPrint("[BlindEye] " fmt "\n", __VA_ARGS__)
```

Then view with:
```bash
# Use DebugView or WinDbg
# https://docs.microsoft.com/en-us/sysinternals/downloads/debugview
DbgView.exe
```

### Kernel Debugging

For advanced debugging, use **WinDbg** with kernel debugging enabled:
```powershell
# Enable kernel debugging via serial/network
bcdedit /debug on
bcdedit /dbgsettings serial debugport:1 baudrate:115200
```

---

**Author:** christopherzx2578-web  
**License:** MIT  
**Last Updated:** 2024
