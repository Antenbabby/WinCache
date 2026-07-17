# WinCache — Block-Level Read Acceleration for Windows

A transparent block-level read cache for Windows, similar to PrimoCache (read-only mode). Accelerates HDD data drives using an SSD as an L2 cache.

## How It Works

WinCache installs as a **lower disk filter driver** below Disk.sys. It intercepts block read requests, checks if the data is already in the SSD cache, and if so, serves it from the fast SSD instead of the slow HDD. Write operations pass through unchanged.

```
Application → NTFS → Disk.sys → [WinCache Filter] → SSD/HDD
```

## Features

- **Transparent**: No drive letter changes — applications see the original drive
- **Read-only**: Accelerates reads only, writes pass through untouched
- **L2 (SSD) cache**: Uses a dedicated SSD partition as cache storage
- **Block-level**: Works at the disk block level, below the filesystem
- **CLOCK LRU eviction**: Approximate LRU with O(1) amortized eviction
- **CLI management**: Simple command-line tool for configuration and monitoring

## System Requirements

- Windows 10 / 11 (x64)
- A source HDD to accelerate (data drives only, not system drive)
- A dedicated SSD or SSD partition for caching (must be raw/unformatted)
- Administrator privileges

## Project Structure

```
D:\temp\winCache\
├── driver/               Kernel driver (cacheflt.sys)
│   ├── cacheflt.c        DriverEntry, device add, unload
│   ├── cacheflt.h        Function declarations
│   ├── cache.h           Core data structures
│   ├── io.c              Read/write IRP dispatch
│   ├── ioctl.c           IOCTL dispatch (user-mode interface)
│   ├── cache.c           Cache lookup, populate, eviction
│   ├── attach.c          Disk attach/detach logic
│   ├── superblock.c      SSD superblock read/write
│   ├── index.c           Block map load/flush to SSD
│   ├── lru.c             CLOCK LRU eviction
│   ├── stats.c           Statistics counters
│   ├── cacheflt.inf      Driver installation INF
│   ├── sources           WDK build config
│   └── makefile          WDK makefile stub
├── manager/              User-mode CLI tool (cachectl.exe)
│   ├── main.c            CLI entry point and commands
│   └── build.bat         MSVC build script
├── shared/               Shared kernel/user definitions
│   └── protocol.h        IOCTL codes, superblock, block entry
├── installer/            Install/uninstall scripts
│   ├── install.bat       One-click driver installation
│   └── uninstall.bat     Driver removal
└── README.md
```

## Getting Prebuilt Binaries (GitHub Actions — No Local Build Tools Needed)

This repository includes a GitHub Actions workflow that compiles the driver and management tool automatically. **You don't need anything installed locally.**

1. **Fork or push this repository to GitHub**
2. Go to the **Actions** tab in your GitHub repo
3. Select **"Build WinCache"** from the left sidebar
4. Click **"Run workflow"** → **"Run workflow"** (manual trigger)
5. Wait ~10 minutes for the build to finish
6. Download the artifacts from the completed run:
   - `cacheflt-driver` — `cacheflt.sys` + `.inf` file
   - `cachectl-manager` — `cachectl.exe`
   - `installer` — `install.bat` / `uninstall.bat`

That's it. Skip to [Installation](#usage).

## Building Locally (Optional)

If you want to build on your own machine:

### Prerequisites: Install the EWDK (Enterprise WDK, ~800MB)

1. Download the **EWDK for Windows 11** ISO from Microsoft
2. Mount the ISO and copy contents to `C:\EWDK`
3. Launch the build environment: `C:\EWDK\LaunchBuildEnv.cmd`

### Build

```cmd
cd D:\temp\WinCache
msbuild WinCache.sln /p:Configuration=Release /p:Platform=x64
```

Outputs go to `bin\x64\Release\` — `cacheflt.sys` and `cachectl.exe`.

## Important: Driver Signing

The GitHub Actions build produces an **unsigned driver**. Windows 10/11 with Secure Boot will refuse to load unsigned kernel drivers. You have two options:

**Option A: Enable Test Signing (for personal use)**
```cmd
# Run as Administrator, then reboot:
bcdedit /set testsigning on
```
After reboot, you'll see "Test Mode" in the bottom-right corner. The unsigned driver will now load. To revert:
```cmd
bcdedit /set testsigning off
```

**Option B: Sign the driver**
Use an EV (Extended Validation) code signing certificate to sign `cacheflt.sys` and submit to Microsoft for Windows Hardware Dev Center attestation signing.

## Usage

1. **Initialize an SSD partition as cache storage** (via manager tool or manually):
   ```cmd
   cachectl init-cache \Device\Harddisk3\Partition1 256
   ```
   This writes the superblock and allocates the index + data regions on the SSD.

2. **Install the driver**:
   ```cmd
   installer\install.bat
   ```
   Or manually:
   ```cmd
   copy driver\cacheflt.sys %SystemRoot%\system32\drivers\
   sc create cacheflt type= kernel start= boot binPath= %SystemRoot%\system32\drivers\cacheflt.sys
   sc start cacheflt
   ```

3. **Attach cache to a source HDD**:
   ```cmd
   cachectl attach \Device\Harddisk2\DR2 \Device\Harddisk3\Partition1
   ```

4. **Monitor cache performance**:
   ```cmd
   cachectl stats \Device\Harddisk2\DR2
   ```

5. **Preload specific files into cache**:
   ```cmd
   cachectl preload \Device\Harddisk2\DR2 D:\Games\big_game.dat
   ```

6. **Detach cache**:
   ```cmd
   cachectl detach \Device\Harddisk2\DR2
   ```

## Architecture Details

### Cache Block Size
Default 64KB. Can be configured via the `attach` command.

### Lookup Algorithm
Direct-mapped hash with 8-way linear probing. Each source LBA maps to a preferred cache slot; if occupied by a different block, up to 7 adjacent slots are probed.

### Eviction Algorithm
CLOCK (second-chance) approximate LRU. Each block has a reference bit; on eviction, the clock hand scans for a block with the bit cleared, giving referenced blocks a "second chance" before eviction.

### SSD Cache Layout
```
Sector 0:         Superblock (magic, version, block_size, geometry)
Sector 1..N:      Block map index (N * 24 bytes per entry)
Sector M..END:    Data blocks (64KB each, aligned to 64KB)
```

## Limitations vs PrimoCache

| Feature | WinCache | PrimoCache |
|---------|----------|------------|
| L1 RAM cache | ❌ | ✅ |
| L2 SSD cache | ✅ | ✅ |
| Write acceleration | ❌ | ✅ |
| System disk support | ❌ | ✅ |
| GUI | ❌ (CLI only) | ✅ |
| Multiple cache tasks | ❌ | ✅ |
| Dynamic block size | ❌ (compile-time) | ✅ |

## License

MIT
