/* Read-only checks: never calls changeDisk or the disk attribute setter. */
#define STANDBY_CACHE_KEY L"Software\\DiskSwitcher\\TestReadOnlyCache"
#include "../main.c"

int wmain(void) {
    DWORD sample[] = {2, 7};
    if (!containsDisk(2, sample, 2) || !containsDisk(7, sample, 2) ||
        containsDisk(1, sample, 2) || containsDisk(2, sample, 0)) return 1;
    if (sizeof(GET_DISK_ATTRIBUTES) != 16 || sizeof(SET_DISK_ATTRIBUTES) != 40 ||
        IOCTL_DISK_GET_DISK_ATTRIBUTES != 0x700f0 ||
        IOCTL_DISK_SET_DISK_ATTRIBUTES != 0x7c0f4) return 2;
    puts("PASS: protected-disk matching and Windows SDK structure layout");

    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"DiskSwitcherReadOnlyTest";
    if (!RegisterClassW(&cls)) return 3;
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"Read-only test", WS_OVERLAPPED,
                              0, 0, 600, 326, NULL, NULL, instance, NULL);
    if (!hwnd || !combo || !onlineButton || !offlineButton || !sleepButton || !statusText) return 4;
    printf("PASS: window and controls created; discovered %d disk(s)\n", diskCount);
    DWORD os[256], count;
    BOOL known = windowsDisks(os, 256, &count);
    printf("Windows disk resolution: %s\n", known ? "available" : "unavailable (fail closed)");
    int protectedCount = 0;
    for (int i = 0; i < diskCount; ++i) {
        SendMessageW(combo, CB_SETCURSEL, i, 0);
        selectionChanged();
        if (!known || containsDisk(disks[i].number, os, count)) {
            if (IsWindowEnabled(offlineButton) || IsWindowEnabled(sleepButton)) return 5;
            ++protectedCount;
        }
    }
    printf("PASS: offline disabled for %d protected/unknown disk(s)\n", protectedCount);
    if (diskCount) {
        /* Simulate only the cached UI state; never send a power command. */
        wcscpy(standbyPath, disks[0].path);
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        selectionChanged();
        if (!IsWindowEnabled(onlineButton) || IsWindowEnabled(offlineButton) ||
            !IsWindowEnabled(sleepButton)) return 6;
        puts("PASS: cached standby UI permits reconnect and manual sleep retry");
    }
    DestroyWindow(hwnd);
    return 0;
}
