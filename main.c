#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include "power.h"
#include <wchar.h>
#include <stdio.h>
#include <stddef.h>

/* Older MinGW headers omit these Windows 7+ SDK declarations. */
#ifndef IOCTL_DISK_GET_DISK_ATTRIBUTES
#define IOCTL_DISK_GET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_SET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define DISK_ATTRIBUTE_OFFLINE 0x0000000000000001ULL
typedef struct { DWORD Version; DWORD Reserved1; DWORDLONG Attributes; } GET_DISK_ATTRIBUTES;
typedef struct {
    DWORD Version;
    BOOLEAN Persist;
    BYTE Reserved1[3];
    DWORDLONG Attributes;
    DWORDLONG AttributesMask;
    DWORD Reserved2[4];
} SET_DISK_ATTRIBUTES;
#endif

static const GUID diskGuid = {0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
typedef struct {
    DWORD number;
    WCHAR path[1024];
    WCHAR title[384];
    WCHAR instanceId[512];
} Disk;
static Disk disks[256];
static int diskCount;
static HWND combo, statusText, onlineButton, offlineButton, sleepButton;
static WCHAR standbyPath[1024];
static WCHAR standbyReport[512];
static HFONT font;

#ifndef STANDBY_CACHE_KEY
#define STANDBY_CACHE_KEY L"Software\\DiskSwitcher\\StandbySession"
#endif
#ifndef UPDATE_GUARD_CACHE_KEY
#define UPDATE_GUARD_CACHE_KEY L"Software\\DiskSwitcherUpdateGuard"
#endif
typedef struct {
    DWORD version;
    Disk disk;
    WCHAR report[512];
    DWORD deviceDisabled;
} StandbyCache;
static StandbyCache savedStandby;
static BOOL savedStandbyValid;

#define UPDATE_GUARD_VALUE L"OriginalServiceState"

static BOOL validStandbyCache(const StandbyCache *cache) {
    return cache->version == 2 && cache->deviceDisabled <= 1 &&
        cache->disk.path[0] && cache->disk.instanceId[0] &&
        wmemchr(cache->disk.path, 0, 1024) && wmemchr(cache->disk.instanceId, 0, 512) &&
        wmemchr(cache->disk.title, 0, 384) && wmemchr(cache->report, 0, 512);
}

static BOOL loadStandbyCache(void) {
    HKEY key;
    StandbyCache candidate = {0};
    DWORD bytes = sizeof(candidate), type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STANDBY_CACHE_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    LONG result = RegQueryValueExW(key, L"LastAcceptedStop", NULL, &type, (BYTE *)&candidate, &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_BINARY || bytes != sizeof(candidate) ||
        !validStandbyCache(&candidate)) return FALSE;
    savedStandby = candidate;
    savedStandbyValid = TRUE;
    wcscpy(standbyPath, candidate.disk.path);
    wcscpy(standbyReport, candidate.report);
    return TRUE;
}

static BOOL saveStandbyCache(const Disk *disk, BOOL deviceDisabled) {
    savedStandby.version = 2;
    savedStandby.disk = *disk;
    wcscpy(savedStandby.report, standbyReport);
    savedStandby.deviceDisabled = deviceDisabled ? 1 : 0;
    savedStandbyValid = TRUE;
    HKEY key;
    /* Volatile: no cache file on a potentially sleeping HDD, and no stale
       state retained after the user hive is unloaded. */
    LONG opened = RegCreateKeyExW(HKEY_CURRENT_USER, STANDBY_CACHE_KEY, 0, NULL,
                                  REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
    if (opened != ERROR_SUCCESS) { SetLastError(opened); return FALSE; }
    LONG result = RegSetValueExW(key, L"LastAcceptedStop", 0, REG_BINARY,
                               (const BYTE *)&savedStandby, sizeof(savedStandby));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) SetLastError(result);
    return result == ERROR_SUCCESS;
}

typedef struct { DWORD version, startType, wasRunning; } LegacyUpdateGuard;

/* v1.3 migration: restore Windows Update once, then remove its old guard. */
static void restoreLegacyUpdateGuard(void) {
    HKEY key;
    LegacyUpdateGuard old = {0};
    DWORD bytes = sizeof(old), type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, UPDATE_GUARD_CACHE_KEY, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    LONG result = RegQueryValueExW(key, UPDATE_GUARD_VALUE, NULL, &type, (BYTE *)&old, &bytes);
    BOOL restored = FALSE;
    if (result == ERROR_SUCCESS && type == REG_BINARY && bytes == sizeof(old) && old.version == 1 &&
        (old.startType == SERVICE_AUTO_START || old.startType == SERVICE_DEMAND_START) && old.wasRunning <= 1) {
        SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        SC_HANDLE service = manager ? OpenServiceW(manager, L"wuauserv",
            SERVICE_CHANGE_CONFIG | SERVICE_START) : NULL;
        if (service && ChangeServiceConfigW(service, SERVICE_NO_CHANGE, old.startType,
                SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            if (old.wasRunning) StartServiceW(service, 0, NULL);
            RegDeleteValueW(key, UPDATE_GUARD_VALUE);
            restored = TRUE;
        }
        if (service) CloseServiceHandle(service);
        if (manager) CloseServiceHandle(manager);
    }
    RegCloseKey(key);
    if (restored) RegDeleteKeyW(HKEY_CURRENT_USER, UPDATE_GUARD_CACHE_KEY);
}

static BOOL setDeviceEnabled(const WCHAR *instanceId, BOOL enabled) {
    HDEVINFO info = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (info == INVALID_HANDLE_VALUE) return FALSE;
    SP_DEVINFO_DATA device = {0};
    device.cbSize = sizeof(device);
    BOOL ok = SetupDiOpenDeviceInfoW(info, instanceId, NULL, 0, &device);
    if (ok) {
        SP_PROPCHANGE_PARAMS change = {0};
        change.ClassInstallHeader.cbSize = sizeof(change.ClassInstallHeader);
        change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        change.StateChange = enabled ? DICS_ENABLE : DICS_DISABLE;
        change.Scope = DICS_FLAG_GLOBAL;
        change.HwProfile = 0;
        ok = SetupDiSetClassInstallParamsW(info, &device,
            &change.ClassInstallHeader, sizeof(change)) &&
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, info, &device);
    }
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    SetupDiDestroyDeviceInfoList(info);
    SetLastError(error);
    return ok;
}

static HANDLE openDiskWithRetry(const WCHAR *path, DWORD access) {
    for (int i = 0; i < 40; ++i) {
        HANDLE h = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        Sleep(250);
    }
    return INVALID_HANDLE_VALUE;
}

static void clearStandbyCache(void) {
    HKEY key;
    standbyPath[0] = 0;
    savedStandbyValid = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STANDBY_CACHE_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"LastAcceptedStop");
        RegCloseKey(key);
    }
}

static void showError(HWND owner, const WCHAR *action, DWORD code) {
    WCHAR detail[512] = L"", message[900];
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, detail, 512, NULL);
    swprintf(message, 900, L"%ls\n\nError %lu: %ls", action, code, detail);
    MessageBoxW(owner, message, L"Disk Switcher", MB_OK | MB_ICONERROR);
}

/* Include every physical extent, also for volumes spanning several disks. */
static BOOL volumeDisks(const WCHAR *volume, DWORD *numbers, DWORD capacity, DWORD *count) {
    DWORD bytes;
    *count = 0;
    HANDLE h = CreateFileW(volume, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = 65536;
    VOLUME_DISK_EXTENTS *extents = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    BOOL ok = extents && DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                        NULL, 0, extents, size, &bytes, NULL);
    if (ok) {
        DWORD n = extents->NumberOfDiskExtents;
        ok = n > 0 && n <= capacity && bytes >= offsetof(VOLUME_DISK_EXTENTS, Extents)
            && n <= (bytes - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT);
        if (ok) {
            for (DWORD i = 0; i < n; ++i) numbers[i] = extents->Extents[i].DiskNumber;
            *count = n;
        }
    }
    if (extents) HeapFree(GetProcessHeap(), 0, extents);
    CloseHandle(h);
    return ok;
}

/* Failure is deliberately treated as unsafe by the callers. */
static BOOL windowsDisks(DWORD *numbers, DWORD capacity, DWORD *count) {
    WCHAR windows[MAX_PATH], root[MAX_PATH], volume[MAX_PATH];
    DWORD length = GetWindowsDirectoryW(windows, MAX_PATH);
    *count = 0;
    if (!length || length >= MAX_PATH ||
        !GetVolumePathNameW(windows, root, MAX_PATH) ||
        !GetVolumeNameForVolumeMountPointW(root, volume, MAX_PATH)) return FALSE;
    length = (DWORD)wcslen(volume);
    if (length && volume[length - 1] == L'\\') volume[length - 1] = 0;
    return volumeDisks(volume, numbers, capacity, count);
}

static BOOL containsDisk(DWORD number, const DWORD *numbers, DWORD count) {
    for (DWORD i = 0; i < count; ++i) if (numbers[i] == number) return TRUE;
    return FALSE;
}

static void diskLetters(DWORD number, WCHAR letters[128]) {
    DWORD mask = GetLogicalDrives();
    letters[0] = 0;
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1UL << i))) continue;
        WCHAR volume[] = L"\\\\.\\A:";
        volume[4] = (WCHAR)(L'A' + i);
        DWORD numbers[256], count;
        if (volumeDisks(volume, numbers, 256, &count) && containsDisk(number, numbers, count)) {
            size_t used = wcslen(letters);
            swprintf(letters + used, 128 - used, L"%ls%lc:", used ? L", " : L"", L'A' + i);
        }
    }
}

static BOOL getAttributes(HANDLE h, GET_DISK_ATTRIBUTES *attrs) {
    DWORD bytes;
    ZeroMemory(attrs, sizeof(*attrs));
    attrs->Version = sizeof(*attrs);
    return DeviceIoControl(h, IOCTL_DISK_GET_DISK_ATTRIBUTES, NULL, 0,
                           attrs, sizeof(*attrs), &bytes, NULL);
}

static void releaseVolumeLocks(HANDLE *volumes, DWORD *count) {
    while (*count) CloseHandle(volumes[--*count]);
}

/* FSCTL_LOCK_VOLUME flushes pending writes and fails if files or a pagefile
   still use a partition. Hold every lock until Offline has been confirmed. */
static BOOL lockDiskVolumes(DWORD number, HANDLE volumes[256], DWORD *count) {
    WCHAR volume[MAX_PATH];
    *count = 0;
    HANDLE search = FindFirstVolumeW(volume, MAX_PATH);
    if (search == INVALID_HANDLE_VALUE) return FALSE;
    DWORD error = ERROR_SUCCESS;
    do {
        size_t length = wcslen(volume);
        if (length && volume[length - 1] == L'\\') volume[length - 1] = 0;
        DWORD numbers[256], n;
        if (!volumeDisks(volume, numbers, 256, &n)) {
            error = ERROR_NOT_READY;
            break; /* Cannot prove which disk an unidentifiable volume uses. */
        }
        if (!containsDisk(number, numbers, n)) continue;
        if (*count == 256) { error = ERROR_BUFFER_OVERFLOW; break; }
        HANDLE h = CreateFileW(volume, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { error = GetLastError(); break; }
        DWORD bytes;
        if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytes, NULL)) {
            error = GetLastError(); CloseHandle(h); break;
        }
        volumes[(*count)++] = h;
    } while (FindNextVolumeW(search, volume, MAX_PATH));
    if (error == ERROR_SUCCESS && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
    FindVolumeClose(search);
    if (error == ERROR_SUCCESS) return TRUE;
    releaseVolumeLocks(volumes, count);
    SetLastError(error);
    return FALSE;
}

static void selectionChanged(void) {
    int index = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    EnableWindow(onlineButton, FALSE);
    EnableWindow(offlineButton, FALSE);
    EnableWindow(sleepButton, FALSE);
    if (index < 0 || index >= diskCount) {
        SetWindowTextW(statusText, L"No disks found. Click Refresh.");
        return;
    }
    if (standbyPath[0] && wcscmp(standbyPath, disks[index].path) == 0) {
        WCHAR text[900];
        swprintf(text, 900, L"Last request: Offline + standby. Live status is not queried.\r\n%ls\r\nWakeup block: %ls. Use Bring Online to reconnect.", standbyReport,
                 savedStandby.deviceDisabled ? L"ACTIVE (PnP device disabled)" : L"not active");
        SetWindowTextW(statusText, text);
        EnableWindow(onlineButton, TRUE);
        EnableWindow(sleepButton, !savedStandby.deviceDisabled);
        return;
    }
    HANDLE h = CreateFileW(disks[index].path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    GET_DISK_ATTRIBUTES attrs;
    BOOL known = h != INVALID_HANDLE_VALUE && getAttributes(h, &attrs);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!known) {
        SetWindowTextW(statusText, L"Status unavailable. The disk is absent or its driver does not support switching.");
        return;
    }
    DWORD os[256], count;
    BOOL safe = windowsDisks(os, 256, &count);
    BOOL protectedDisk = safe && containsDisk(disks[index].number, os, count);
    BOOL offline = (attrs.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
    WCHAR text[512];
    swprintf(text, 512, L"Status: %ls\r\n\r\n%ls",
        offline ? L"Offline" : L"Online",
        protectedDisk ? L"This disk contains the running Windows installation. Taking it offline is blocked." :
        !safe ? L"The Windows system disk could not be identified. Taking disks offline is blocked." :
        L"The entire physical disk can be switched Online or Offline.");
    SetWindowTextW(statusText, text);
    EnableWindow(onlineButton, offline);
    EnableWindow(offlineButton, !offline && safe && !protectedDisk);
    EnableWindow(sleepButton, safe && !protectedDisk);
}

static void refreshDisks(void) {
    Disk sleeping = savedStandby.disk;
    BOOL keepSleeping = savedStandbyValid;
    for (int i = 0; i < diskCount; ++i) {
        if (standbyPath[0] && wcscmp(standbyPath, disks[i].path) == 0) {
            sleeping = disks[i]; keepSleeping = TRUE; break;
        }
    }
    DWORD previous = MAXDWORD;
    int selected = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected >= 0 && selected < diskCount) previous = disks[selected].number;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    diskCount = 0;
    HDEVINFO info = SetupDiGetClassDevsW(&diskGuid, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) { selectionChanged(); return; }
    SP_DEVICE_INTERFACE_DATA iface = {0};
    iface.cbSize = sizeof(iface);
    int restore = 0;
    BOOL sleepingAdded = FALSE;
    for (DWORD i = 0; diskCount < 256 &&
         SetupDiEnumDeviceInterfaces(info, NULL, &diskGuid, i, &iface); ++i) {
        DWORD needed = 0, bytes;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, NULL, 0, &needed, NULL);
        if (!needed) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA device = {0};
        device.cbSize = sizeof(device);
        if (SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, needed, NULL, &device)) {
            if (keepSleeping && wcscmp(detail->DevicePath, sleeping.path) == 0) {
                disks[diskCount] = sleeping;
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)sleeping.title);
                if (sleeping.number == previous) restore = diskCount;
                ++diskCount;
                sleepingAdded = TRUE;
                HeapFree(GetProcessHeap(), 0, detail);
                continue; /* Preserve the entry without opening or querying this disk. */
            }
            HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            STORAGE_DEVICE_NUMBER number;
            if (h != INVALID_HANDLE_VALUE &&
                DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                                &number, sizeof(number), &bytes, NULL) &&
                number.DeviceType == FILE_DEVICE_DISK && wcslen(detail->DevicePath) < 1024) {
                Disk *d = &disks[diskCount];
                ZeroMemory(d, sizeof(*d));
                d->number = number.DeviceNumber;
                wcscpy(d->path, detail->DevicePath);
                SetupDiGetDeviceInstanceIdW(info, &device, d->instanceId,
                                            sizeof(d->instanceId) / sizeof(d->instanceId[0]), NULL);
                WCHAR name[160] = L"Physical disk";
                if (!SetupDiGetDeviceRegistryPropertyW(info, &device, SPDRP_FRIENDLYNAME,
                        NULL, (BYTE *)name, sizeof(name), NULL)) {
                    wcscpy(name, L"Physical disk");
                }
                name[159] = 0;
                WCHAR letters[128];
                diskLetters(d->number, letters);
                swprintf(d->title, 384, L"Disk %lu [%ls] - %ls", d->number,
                         letters[0] ? letters : L"no available drive letters", name);
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)d->title);
                if (d->number == previous) restore = diskCount;
                ++diskCount;
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }
    if (keepSleeping && !sleepingAdded && diskCount < 256) {
        disks[diskCount] = sleeping;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)sleeping.title);
        if (sleeping.number == previous) restore = diskCount;
        ++diskCount;
    }
    SetupDiDestroyDeviceInfoList(info);
    if (diskCount) SendMessageW(combo, CB_SETCURSEL, restore, 0);
    selectionChanged();
}

static void changeDisk(HWND owner, BOOL offline, BOOL standby) {
    int index = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0 || index >= diskCount) return;
    Disk disk = disks[index];
    if (offline) {
        WCHAR question[768];
        swprintf(question, 768, L"Take %ls Offline%ls?\n\nClose files and applications that use this disk. All of its partitions will become unavailable until it is brought Online again.", disk.title,
                 standby ? L" and stop the HDD spindle" : L"");
        if (MessageBoxW(owner, question, L"Take disk Offline",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) return;
    }
    if (!offline && savedStandbyValid && savedStandby.deviceDisabled &&
        wcscmp(standbyPath, disk.path) == 0) {
        if (!setDeviceEnabled(disk.instanceId, TRUE)) {
            showError(owner, L"Could not re-enable the disk device.", GetLastError());
            return;
        }
    }
    HANDLE h = openDiskWithRetry(disk.path, GENERIC_READ | GENERIC_WRITE);
    if (h == INVALID_HANDLE_VALUE) {
        showError(owner, L"Could not open the disk.", GetLastError());
        refreshDisks(); return;
    }
    DWORD bytes;
    STORAGE_DEVICE_NUMBER current;
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                         &current, sizeof(current), &bytes, NULL) ||
        current.DeviceNumber != disk.number || current.DeviceType != FILE_DEVICE_DISK) {
        CloseHandle(h);
        MessageBoxW(owner, L"The disk changed or disappeared. Refresh the list.", L"Disk Switcher", MB_OK | MB_ICONERROR);
        refreshDisks(); return;
    }
    /* Recheck after confirmation, against the same open device handle used below. */
    if (offline) {
        DWORD os[256], count;
        if (!windowsDisks(os, 256, &count) || containsDisk(current.DeviceNumber, os, count)) {
            CloseHandle(h);
            MessageBoxW(owner, L"Operation blocked: the disk contains the running Windows installation or the safety check is unavailable.",
                        L"Windows disk protection", MB_OK | MB_ICONSTOP);
            refreshDisks(); return;
        }
    }
    HANDLE volumeLocks[256];
    DWORD lockCount = 0;
    GET_DISK_ATTRIBUTES before;
    if (offline && (!getAttributes(h, &before) ||
        (!(before.Attributes & DISK_ATTRIBUTE_OFFLINE) && !lockDiskVolumes(current.DeviceNumber, volumeLocks, &lockCount)))) {
        DWORD lockError = GetLastError();
        CloseHandle(h);
        showError(owner, L"Could not safely lock the disk volumes. Close files and applications using the disk. No Offline or standby command was sent.", lockError);
        return;
    }
    SET_DISK_ATTRIBUTES attrs = {0};
    attrs.Version = sizeof(attrs);
    attrs.Persist = TRUE;
    attrs.Attributes = offline ? DISK_ATTRIBUTE_OFFLINE : 0;
    attrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    BOOL ok = DeviceIoControl(h, IOCTL_DISK_SET_DISK_ATTRIBUTES,
        &attrs, sizeof(attrs), NULL, 0, &bytes, NULL);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    GET_DISK_ATTRIBUTES actual;
    BOOL verified = ok && getAttributes(h, &actual) &&
        (((actual.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0) == offline);
    releaseVolumeLocks(volumeLocks, &lockCount);
    BOOL slept = FALSE;
    DWORD sleepError = ERROR_SUCCESS;
    if (standby && verified) {
        wcscpy(standbyReport, L"The stop command was blocked by the Windows disk safety check.");
        /* Recheck protection immediately before the power command as well. */
        DWORD os[256], count;
        if (!windowsDisks(os, 256, &count) || containsDisk(current.DeviceNumber, os, count)) {
            sleepError = ERROR_ACCESS_DENIED;
        } else {
            slept = diskStandby(h, standbyReport, 512);
            if (!slept) sleepError = GetLastError();
        }
    }
    CloseHandle(h);
    if (!offline && verified && wcscmp(standbyPath, disk.path) == 0) clearStandbyCache();
    if (slept) {
        BOOL disabled = setDeviceEnabled(disk.instanceId, FALSE);
        DWORD disableError = disabled ? ERROR_SUCCESS : GetLastError();
        if (disabled) {
            wcsncat(standbyReport, L"\r\nPnP device disabled: system wakeups are blocked.",
                    511 - wcslen(standbyReport));
        } else {
            wcsncat(standbyReport, L"\r\nPnP disable failed: the HDD may still be woken.",
                    511 - wcslen(standbyReport));
        }
        wcscpy(standbyPath, disk.path);
        BOOL saved = saveStandbyCache(&disk, disabled);
        selectionChanged();
        if (!saved) MessageBoxW(owner, L"The stop command was accepted, but its cached state could not be saved. Keep this window open to avoid a fresh disk query at next launch.",
                               L"Disk Switcher", MB_OK | MB_ICONWARNING);
        if (!disabled) showError(owner, L"The HDD stop command was accepted, but Windows did not disable its PnP device.", disableError);
        return; /* Do not follow the standby command with disk queries. */
    }
    if (standby && verified) {
        WCHAR failure[768];
        swprintf(failure, 768, L"The disk remains Offline. The stop command was not confirmed.\r\n\r\n%ls", standbyReport);
        showError(owner, failure, sleepError);
    }
    if (!ok) showError(owner, L"Windows did not allow the disk status to be changed.", error);
    else if (!verified) MessageBoxW(owner, L"The command was sent, but the new status was not confirmed. Refresh the list.",
                                    L"Disk Switcher", MB_OK | MB_ICONWARNING);
    refreshDisks();
}

static HWND control(HWND owner, const WCHAR *cls, const WCHAR *text, DWORD style,
                    int x, int y, int w, int h, int id) {
    HWND child = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, owner, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return child;
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        restoreLegacyUpdateGuard();
        loadStandbyCache();
        font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        control(hwnd, L"STATIC", L"Physical disk:", 0, 22, 18, 500, 25, 0);
        combo = control(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                        22, 48, 556, 260, 101);
        statusText = control(hwnd, L"STATIC", L"", 0, 22, 93, 556, 95, 0);
        onlineButton = control(hwnd, L"BUTTON", L"Bring Online", WS_TABSTOP, 22, 202, 170, 38, 102);
        offlineButton = control(hwnd, L"BUTTON", L"Take Offline", WS_TABSTOP, 210, 202, 170, 38, 103);
        control(hwnd, L"BUTTON", L"Refresh", WS_TABSTOP, 398, 202, 180, 38, 104);
        sleepButton = control(hwnd, L"BUTTON", L"Stop HDD + block system wakeups", WS_TABSTOP,
                              22, 252, 556, 38, 105);
        control(hwnd, L"STATIC",
            L"Deep sleep disables the selected PnP disk device after ATA standby.\r\nThe disk stays in this list; Bring Online re-enables it.",
            0, 22, 304, 556, 55, 0);
        refreshDisks();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 101 && HIWORD(wp) == CBN_SELCHANGE) selectionChanged();
        if (LOWORD(wp) == 102) changeDisk(hwnd, FALSE, FALSE);
        if (LOWORD(wp) == 103) changeDisk(hwnd, TRUE, FALSE);
        if (LOWORD(wp) == 104) refreshDisks();
        if (LOWORD(wp) == 105) changeDisk(hwnd, TRUE, TRUE);
        return 0;
    case WM_DEVICECHANGE: if (!standbyPath[0]) PostMessageW(hwnd, WM_APP, 0, 0); return 0;
    case WM_APP: if (!standbyPath[0]) refreshDisks(); return 0;
    case WM_DESTROY: DeleteObject(font); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    (void)previous; (void)command;
    HANDLE singleInstance = CreateMutexW(NULL, FALSE, L"Local\\DiskSwitcher.SingleInstance");
    if (!singleInstance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"DiskSwitcherWindow", NULL);
        if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        CloseHandle(singleInstance);
        return 0;
    }
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    cls.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    cls.lpszClassName = L"DiskSwitcherWindow";
    if (!RegisterClassW(&cls)) return 1;
    RECT rect = {0, 0, 600, 402};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"Disk Switcher", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, instance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    MSG msg;
    int result;
    while ((result = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    CloseHandle(singleInstance);
    return result == -1 ? 1 : (int)msg.wParam;
}
