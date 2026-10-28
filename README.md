# Disk Switcher

Disk Switcher is a small native Windows utility for managing physical disks. It is written in C using the Win32 API and builds into a single executable of about 60 KB. It does not require .NET or installation.

## Features

- Lists physical disks and their currently assigned drive letters.
- Switches an entire physical disk between Windows Online and Offline states.
- Sends `ATA STANDBY IMMEDIATE (E0h)` to stop a supported HDD spindle.
- Falls back to `SCSI START STOP UNIT` when ATA pass-through is rejected.
- Blocks Offline and standby operations for the disk containing the running Windows installation.
- Locks and flushes every volume on an Online disk before taking it Offline. The operation stops if a volume is busy or cannot be identified.
- Remembers the last accepted standby request without reopening that physical disk on the next application launch.
- Prevents multiple application instances from querying the disks independently.

The application requires administrator privileges because Windows restricts raw disk access and disk attribute changes.

## Supported systems

- Windows 10 or Windows 11, x64
- MinGW-w64 GCC for building from source
- SATA, ATA, SCSI and RAID disk interfaces may be attempted, but standby command support depends on the drive, controller and driver

Disk Switcher changes the Windows Online/Offline state and requests a drive power state. It does not physically disconnect SATA power. Another application, Windows component or storage driver can wake a sleeping HDD. An accepted command confirms only that the driver and disk accepted the request; the program cannot measure spindle rotation.

## Usage

1. Run `DiskSwitcher.exe` and approve the Windows administrator prompt.
2. Select a physical disk from the list.
3. Use one of the available actions:
   - **Bring Online** — makes an Offline disk accessible again.
   - **Take Offline** — flushes and locks its volumes, then takes the disk Offline.
   - **Stop / repeat HDD standby** — takes the disk Offline if necessary and requests HDD standby.
   - **Refresh** — rereads the disk list. A disk remembered as sleeping is kept in the list without opening it.

Close applications and files that use the selected disk before taking it Offline. A physically disconnected disk cannot be brought Online by software.

## Build an EXE

Install MinGW-w64 GCC and make sure both `gcc.exe` and `windres.exe` are available in `PATH`:

```powershell
gcc --version
windres --version
```

From the project directory, run the build script:

```powershell
Set-Location D:\myapps\disc-switcher
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The output is:

```text
D:\myapps\disc-switcher\DiskSwitcher.exe
```

To choose a different filename:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -OutputName DiskSwitcher-English.exe
```

Equivalent manual build commands:

```powershell
New-Item -ItemType Directory -Force -Path build | Out-Null
windres app.rc -O coff -o build\app.res
gcc main.c power.c build\app.res -o DiskSwitcher.exe `
  -std=c11 -Os -s -Wall -Wextra -Werror `
  -municode -mwindows -static `
  -lsetupapi -ladvapi32 -luser32 -lgdi32 `
  '-Wl,--nxcompat' '-Wl,--dynamicbase'
```

`app.manifest` is embedded by `windres` and makes the executable request administrator privileges. The current executable version is `1.2.0`.

## Verification

The automated checks simulate driver and volume responses. They do not send Offline or standby commands to real disks.

```powershell
New-Item -ItemType Directory -Force -Path build | Out-Null

gcc tests\power.c -o build\power-check.exe `
  -std=c11 -Wall -Wextra -Werror -static
.\build\power-check.exe

gcc tests\volume-locks.c power.c -o build\volume-lock-check.exe `
  -std=c11 -Wall -Wextra -Werror -municode -static `
  -lsetupapi -ladvapi32 -luser32 -lgdi32
.\build\volume-lock-check.exe

gcc tests\read-only.c power.c -o build\read-only-check.exe `
  -std=c11 -Wall -Wextra -Werror -municode -static `
  -lsetupapi -ladvapi32 -luser32 -lgdi32
.\build\read-only-check.exe

gcc tests\standby-cache.c power.c -o build\cache-check.exe `
  -std=c11 -Wall -Wextra -Werror -municode -static `
  -lsetupapi -ladvapi32 -luser32 -lgdi32
.\build\cache-check.exe
```

The cache check creates and removes only its isolated test registry key under `HKCU\Software\DiskSwitcher\TestStandbyCache`.

`tests/target-stop.c` is a machine-specific hardware diagnostic restricted to one previously verified HDD by disk number, exact capacity and serial number. It is not part of the normal build or automated checks.

## Diagnostics

`capture-wake.ps1` can record Windows Disk I/O and File I/O events in memory to investigate which process wakes a sleeping HDD:

```powershell
powershell -ExecutionPolicy Bypass -File .\capture-wake.ps1 -DurationSeconds 1200
```

The trace is saved under `diagnostics\` and is ignored by Git.

## Implementation notes

- `main.c` contains disk enumeration, Windows disk protection, volume locking and the Win32 user interface.
- `power.c` and `power.h` contain ATA and SCSI standby commands.
- The last accepted standby request is cached in the volatile registry key `HKCU\Software\DiskSwitcher\StandbySession`. **Bring Online** clears it after a confirmed reconnect.
- The running Windows volume is resolved to all of its physical disk extents. If this safety check fails, destructive state changes are blocked.

Relevant Microsoft documentation:

- [IOCTL_DISK_SET_DISK_ATTRIBUTES](https://learn.microsoft.com/windows/win32/api/winioctl/ni-winioctl-ioctl_disk_set_disk_attributes)
- [IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS](https://learn.microsoft.com/windows/win32/api/winioctl/ni-winioctl-ioctl_volume_get_volume_disk_extents)
- [ATA_PASS_THROUGH_EX](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddscsi/ns-ntddscsi-_ata_pass_through_ex)
- [IOCTL_SCSI_PASS_THROUGH](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddscsi/ni-ntddscsi-ioctl_scsi_pass_through)
- [FSCTL_LOCK_VOLUME](https://learn.microsoft.com/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume)
