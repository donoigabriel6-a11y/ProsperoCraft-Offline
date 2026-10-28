<#
.SYNOPSIS
    Build ProsperoCraft and create an exFAT disk image with the .pkg file for USB installation on PS5.

.DESCRIPTION
    This script builds the ProsperoCraft PS5 homebrew title and packages it as an exFAT
    disk image that can be written to a USB drive for installation via PS5's
    "Install Package Files" feature.

.REQUIREMENTS
    - WSL with PS5 SDK installed (or Linux)
    - PS5 payload SDK (~/.deps/native/ps5-payload-sdk)
    - Java 17+ JDK
    - Python 3
    - clang/clang++ 18+
    - For exFAT: exfatprogs (mkfs.exfat) or dosfstools (mkfs.vfat)

.PARAMETER SkipBuild
    Skip the build step and use existing dist/ folder

.PARAMETER SizeMB
    exFAT image size in MB (default: 512)

.PARAMETER OutputFile
    Output image file path (default: ProsperoCraft-<version>.img)

.PARAMETER UsbDevice
    If specified, write image directly to USB device (e.g., /dev/sdb or \\.\PhysicalDrive1)

.EXAMPLE
    .\build_exfat.ps1

.EXAMPLE
    .\build_exfat.ps1 -SizeMB 1024 -OutputFile "ProsperoCraft-1.0.img"

.EXAMPLE
    .\build_exfat.ps1 -UsbDevice "/dev/sdb"  # Linux/WSL
    .\build_exfat.ps1 -UsbDevice "\\.\PhysicalDrive1"  # Windows
#>

param(
    [switch]$SkipBuild,
    [int]$SizeMB = 512,
    [string]$OutputFile = "",
    [string]$UsbDevice = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$TitleDir = Join-Path $ScriptDir "title"
$RepoRoot = $ScriptDir

# Get version from param.json
$ParamJson = Get-Content (Join-Path $TitleDir "sce_sys/param.json") -Raw | ConvertFrom-Json
$Version = $ParamJson.contentVersion
$TitleId = $ParamJson.titleId
$AppName = $ParamJson.localizedParameters."en-US".titleName

if (-not $OutputFile) {
    $OutputFile = "${AppName}-${Version}.img"
}

$OutputFile = [IO.Path]::GetFullPath($OutputFile)

Write-Host "=== ProsperoCraft exFAT Image Builder ===" -ForegroundColor Cyan
Write-Host "Title ID: $TitleId"
Write-Host "Version: $Version"
Write-Host "App Name: $AppName"
Write-Host "Output: $OutputFile"
Write-Host "Image size: ${SizeMB}MB"
Write-Host ""

# Step 1: Build the project
if (-not $SkipBuild) {
    Write-Host "=== Step 1: Building .pkg file ===" -ForegroundColor Green
    Set-Location $TitleDir

    # Check for required tools
    if (-not (Get-Command make -ErrorAction SilentlyContinue)) {
        Write-Error "make not found. Install build tools in WSL."
        exit 1
    }
    if (-not (Get-Command python3 -ErrorAction SilentlyContinue)) {
        Write-Error "python3 not found."
        exit 1
    }

    # Build the .pkg file
    make ffpkg

    Write-Host "=== Build complete ===" -ForegroundColor Green
    Write-Host ""
}

# Step 2: Verify .pkg exists
$PkgFile = Join-Path $TitleDir "dist" "${TitleId}.ffpkg"
if (-not (Test-Path $PkgFile)) {
    Write-Error "Error: .pkg file not found at $PkgFile"
    Write-Error "Run without -SkipBuild or ensure build completed successfully"
    exit 1
}

$PkgSize = (Get-Item $PkgFile).Length
Write-Host "Found .pkg: $PkgFile ($PkgSize bytes)"

# Step 3: Create exFAT disk image
Write-Host ""
Write-Host "=== Step 2: Creating exFAT disk image ===" -ForegroundColor Green

# Check if running in WSL/Linux
$IsLinux = $IsLinux -or (Test-Path "/proc/version")

if ($IsLinux) {
    # Linux/WSL path - use loop devices
    CreateExfatImageLinux
} else {
    # Windows path - use PowerShell/DiskPart
    CreateExfatImageWindows
}

function CreateExfatImageLinux {
    # Check for mkfs.exfat
    $MkfsExfat = if (Get-Command mkfs.exfat -ErrorAction SilentlyContinue) { "mkfs.exfat" }
                 elseif (Get-Command mkfs.vfat -ErrorAction SilentlyContinue) { "mkfs.vfat -F 32" }
                 else {
                     Write-Error "No filesystem formatting tool found (mkfs.exfat or mkfs.vfat)"
                     Write-Error "Install exfatprogs or dosfstools"
                     exit 1
                 }

    if ($MkfsExfat -eq "mkfs.vfat -F 32") {
        Write-Warning "exFAT tools not found, using FAT32 (4GB file size limit)"
    }

    # Create raw image file
    Write-Host "Creating ${SizeMB}MB raw image..."
    dd if=/dev/zero of=$OutputFile bs=1M count=$SizeMB status=progress

    # Format as exFAT
    Write-Host "Formatting as exFAT..."
    if ($MkfsExfat -like "*vfat*") {
        mkfs.vfat -F 32 $OutputFile
    } else {
        mkfs.exfat $OutputFile
    }

    # Mount using loop device
    $MountPoint = [IO.Path]::GetTempPath() + "ps5_mount_" + [Guid]::NewGuid().ToString("N").Substring(0,8)
    New-Item -ItemType Directory -Path $MountPoint | Out-Null

    Write-Host "Mounting image..."
    $LoopDev = sudo losetup --find --show $OutputFile
    sudo partprobe $LoopDev 2>$null
    Start-Sleep 1

    sudo mount $LoopDev $MountPoint

    # Copy .pkg file
    Write-Host "Copying .pkg to image..."
    sudo cp $PkgFile (Join-Path $MountPoint "${TitleId}.pkg")

    # Create README
    $Readme = @"
ProsperoCraft for PS5
=====================
Title ID: $TitleId
Version: $Version
Build date: $(Get-Date)

Installation:
1. Insert this USB drive into your PS5
2. Go to Settings > System > System Software > Install Package Files
3. Select "Install from USB Drive"
4. Choose "${TitleId}.pkg" and install

Requirements:
- PS5 with homebrew enabled (exploit required)
- The app will appear on the home screen as "ProsperoCraft"

For offline/demo mode:
- Select "Play Offline" in the launcher
- Choose a profile from "Who is playing?"
- If signed in with Microsoft account that doesn't own Minecraft, demo mode activates automatically

Source: https://github.com/donoigabriel6-a11y/ProsperoCraft-Offline
"@
    $ReadmePath = [IO.Path]::GetTempPath() + "README_USB.txt"
    $Readme | Out-File -FilePath $ReadmePath -Encoding UTF8
    sudo cp $ReadmePath (Join-Path $MountPoint "README.txt")

    # Unmount
    Write-Host "Unmounting..."
    sync
    sudo umount $MountPoint
    sudo losetup -d $LoopDev
    Remove-Item -Path $MountPoint -Force

    Write-Host ""
    Write-Host "=== Done! ===" -ForegroundColor Cyan
    Write-Host "exFAT image created: $OutputFile"
    Write-Host ""
    if ($UsbDevice) {
        Write-Host "Writing to USB device $UsbDevice..."
        sudo dd if=$OutputFile of=$UsbDevice bs=4M status=progress conv=fsync
        Write-Host "USB ready!"
    } else {
        Write-Host "To write to USB drive:"
        Write-Host "  sudo dd if=$OutputFile of=/dev/sdX bs=4M status=progress conv=fsync"
        Write-Host "  (replace /dev/sdX with your USB device)"
    }
}

function CreateExfatImageWindows {
    Write-Host "Windows native exFAT creation not fully implemented." -ForegroundColor Yellow
    Write-Host "Recommended: Run this script in WSL instead:" -ForegroundColor Yellow
    Write-Host "  wsl bash build_exfat.sh"
    Write-Host ""
    Write-Host "Or manually:"
    Write-Host "  1. Build in WSL: wsl -d <distro> 'cd /mnt/c/.../title && make ffpkg'"
    Write-Host "  2. Copy .pkg to Windows: wsl -d <distro> 'cp /mnt/c/.../title/dist/*.ffpkg /mnt/c/.../'"
    Write-Host "  3. Format USB as exFAT in Windows (right-click > Format > exFAT)"
    Write-Host "  4. Copy .pkg file to USB"
    exit 1
}

Write-Host ""
Write-Host "Image size: $(Get-Item $OutputFile | Select-Object -ExpandProperty Length / 1MB) MB" -ForegroundColor Cyan