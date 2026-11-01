/* Uses an isolated registry key; never opens or changes a real disk. */
#define STANDBY_CACHE_KEY L"Software\\DiskSwitcher\\TestStandbyCache"
#define UPDATE_GUARD_CACHE_KEY L"Software\\DiskSwitcherTestUpdateGuardCache"
#include "../main.c"
#include <assert.h>
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL at line %d (Win32 %lu)\n", __LINE__, GetLastError()); \
    return 1; \
} } while (0)

int wmain(void) {
    Disk sample = {0};
    sample.number = 1;
    wcscpy(sample.path, L"test-device-path");
    wcscpy(sample.instanceId, L"test-instance-id");
    wcscpy(sample.title, L"Disk 1 [F:] - test HDD");
    wcscpy(standbyReport, L"ATA: accepted");
    assert(saveStandbyCache(&sample, TRUE));
    ZeroMemory(&savedStandby, sizeof(savedStandby));
    standbyPath[0] = standbyReport[0] = 0;
    savedStandbyValid = FALSE;
    assert(loadStandbyCache());
    assert(savedStandbyValid && savedStandby.disk.number == 1);
    assert(wcscmp(standbyPath, sample.path) == 0);
    assert(wcscmp(savedStandby.disk.instanceId, sample.instanceId) == 0);
    assert(wcscmp(standbyReport, L"ATA: accepted") == 0);
    assert(savedStandby.deviceDisabled == TRUE);
    puts("PASS: standby identity, command result and disabled-device state survive a fresh process state");
    StandbyCache invalid = savedStandby;
    invalid.version = 99;
    assert(!validStandbyCache(&invalid));
    invalid = savedStandby;
    wmemset(invalid.disk.path, L'x', 1024);
    assert(!validStandbyCache(&invalid));
    invalid = savedStandby;
    invalid.disk.instanceId[0] = 0;
    assert(!validStandbyCache(&invalid));
    HKEY key;
    assert(RegOpenKeyExW(HKEY_CURRENT_USER, STANDBY_CACHE_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS);
    BYTE malformed = 1;
    assert(RegSetValueExW(key, L"LastAcceptedStop", 0, REG_BINARY, &malformed, 1) == ERROR_SUCCESS);
    RegCloseKey(key);
    assert(!loadStandbyCache());
    puts("PASS: invalid versions, identities, unterminated strings and short cache records rejected");
    clearStandbyCache();
    assert(!savedStandbyValid && !standbyPath[0] && !loadStandbyCache());
    RegDeleteKeyW(HKEY_CURRENT_USER, STANDBY_CACHE_KEY);
    puts("PASS: reconnect clears the remembered standby request");
    return 0;
}
