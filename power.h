#ifndef DISK_SWITCHER_POWER_H
#define DISK_SWITCHER_POWER_H
#include <windows.h>
#include <stddef.h>

/* Caller must first verify Offline and exclude the running Windows disk.
   Success means the driver accepted a stop command, not measured spindle RPM. */
BOOL diskStandby(HANDLE disk, WCHAR *report, size_t reportCapacity);
#endif
