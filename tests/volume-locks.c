/* Simulates volume enumeration and locks. No real disk or volume is opened. */
#include <windows.h>
#include <winioctl.h>
#include <assert.h>
#include <stdio.h>
#include <wchar.h>
static int scenario, entry, locked, released;
static HANDLE WINAPI fakeFirst(LPWSTR, DWORD);
static BOOL WINAPI fakeNext(HANDLE, LPWSTR, DWORD);
static BOOL WINAPI fakeFindClose(HANDLE);
static HANDLE WINAPI fakeOpen(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL WINAPI fakeClose(HANDLE);
static BOOL WINAPI fakeIo(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
#define FindFirstVolumeW fakeFirst
#define FindNextVolumeW fakeNext
#define FindVolumeClose fakeFindClose
#define CreateFileW fakeOpen
#define CloseHandle fakeClose
#define DeviceIoControl fakeIo
#define STANDBY_CACHE_KEY L"Software\\DiskSwitcher\\UnusedVolumeTest"
#include "../main.c"

static HANDLE WINAPI fakeFirst(LPWSTR path, DWORD size) {
    (void)size;
    entry = 0;
    if (scenario == 5) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    wcscpy(path, L"volumeA\\");
    return (HANDLE)(INT_PTR)100;
}
static BOOL WINAPI fakeNext(HANDLE h, LPWSTR path, DWORD size) {
    (void)h; (void)size;
    if (++entry == 1) { wcscpy(path, L"volumeB\\"); return TRUE; }
    SetLastError(scenario == 4 ? ERROR_BAD_FORMAT : ERROR_NO_MORE_FILES);
    return FALSE;
}
static BOOL WINAPI fakeFindClose(HANDLE h) { (void)h; return TRUE; }
static HANDLE WINAPI fakeOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD creation, DWORD flags, HANDLE template) {
    (void)share; (void)sa; (void)creation; (void)flags; (void)template;
    int index = wcsstr(path, L"volumeB") ? 1 : 0;
    return (HANDLE)(INT_PTR)((access ? 2000 : 1000) + index);
}
static BOOL WINAPI fakeClose(HANDLE h) {
    if ((INT_PTR)h >= 2000) ++released;
    return TRUE;
}
static BOOL WINAPI fakeIo(HANDLE h, DWORD code, LPVOID input, DWORD inputBytes,
                          LPVOID output, DWORD outputBytes, LPDWORD bytes, LPOVERLAPPED overlapped) {
    (void)input; (void)inputBytes; (void)outputBytes; (void)overlapped;
    int index = (int)((INT_PTR)h % 1000);
    if (code == IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) {
        if (scenario == 3 && index == 1) { SetLastError(ERROR_NOT_READY); return FALSE; }
        VOLUME_DISK_EXTENTS *extents = output;
        extents->NumberOfDiskExtents = 1;
        extents->Extents[0].DiskNumber = (index == 0 || scenario == 2) ? 1 : 0;
        *bytes = sizeof(*extents);
        return TRUE;
    }
    assert(code == FSCTL_LOCK_VOLUME);
    assert((INT_PTR)h >= 2000);
    if (scenario == 1 || (scenario == 2 && index == 1)) {
        SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    ++locked;
    return TRUE;
}
int wmain(void) {
    for (scenario = 0; scenario <= 5; ++scenario) {
        HANDLE handles[256];
        DWORD count;
        locked = released = 0;
        BOOL ok = lockDiskVolumes(1, handles, &count);
        if (scenario == 0) {
            assert(ok && count == 1 && locked == 1 && released == 0);
            releaseVolumeLocks(handles, &count);
            assert(count == 0 && released == 1);
        } else {
            assert(!ok && count == 0);
            if (scenario == 1) assert(locked == 0 && released == 1);
            if (scenario == 2) assert(locked == 1 && released == 2);
            if (scenario == 3 || scenario == 4) assert(locked == 1 && released == 1);
            if (scenario == 5) assert(locked == 0 && released == 0);
        }
    }
    puts("PASS: 6 simulated volume-lock cases, busy/unknown volumes blocked, all handles released");
    return 0;
}
