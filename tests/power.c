/* Simulated driver: no handles are opened and no real disk receives commands. */
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <assert.h>
#include <stdio.h>
#include <wchar.h>

static int scenario, ataCalls, scsiCalls;
static BOOL WINAPI simulatedIo(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
#define DeviceIoControl simulatedIo
#include "../power.c"
#undef DeviceIoControl

static BOOL WINAPI simulatedIo(HANDLE disk, DWORD code, LPVOID input, DWORD inputBytes,
                               LPVOID output, DWORD outputBytes, LPDWORD bytes, LPOVERLAPPED overlapped) {
    (void)disk; (void)outputBytes; (void)overlapped;
    *bytes = 0;
    if (code == IOCTL_STORAGE_QUERY_PROPERTY) {
        if (scenario == 5) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        STORAGE_DEVICE_DESCRIPTOR *d = output;
        ZeroMemory(d, sizeof(*d));
        d->BusType = scenario == 3 ? BusTypeNvme : BusTypeSata;
        *bytes = scenario == 8 ? 1 : sizeof(*d);
        return TRUE;
    }
    if (code == IOCTL_ATA_PASS_THROUGH) {
        ++ataCalls;
        ATA_PASS_THROUGH_EX *ata = input;
        assert(input == output && inputBytes == sizeof(*ata));
        /* Regression: command belongs in CURRENT, never PREVIOUS registers. */
        assert(ata->CurrentTaskFile[6] == 0xE0 && ata->CurrentTaskFile[5] == 0xA0);
        for (int i = 0; i < 8; ++i) assert(ata->PreviousTaskFile[i] == 0);
        assert(!(ata->AtaFlags & (ATA_FLAGS_DATA_IN | ATA_FLAGS_DATA_OUT | ATA_FLAGS_48BIT_COMMAND)));
        assert(ata->DataTransferLength == 0 && ata->DataBufferOffset == 0);
        if (scenario == 1 || scenario == 6 || scenario == 7) {
            SetLastError(ERROR_IO_DEVICE); return FALSE;
        }
        ata->CurrentTaskFile[6] = scenario == 2 ? 0x51 : 0x50;
        ata->CurrentTaskFile[0] = scenario == 2 ? 4 : 0;
        *bytes = scenario == 4 ? 1 : sizeof(*ata);
        return TRUE;
    }
    assert(code == IOCTL_SCSI_PASS_THROUGH);
    ++scsiCalls;
    StopRequest *scsi = input;
    assert(input == output && inputBytes == sizeof(*scsi));
    assert(scsi->header.Length == sizeof(scsi->header));
    assert(scsi->header.CdbLength == 6 && scsi->header.Cdb[0] == 0x1B);
    /* No eject, no no-flush, no data transfer; completion waits for STOP. */
    for (int i = 1; i < 16; ++i) assert(scsi->header.Cdb[i] == 0);
    assert(scsi->header.DataTransferLength == 0);
    assert(scsi->header.SenseInfoOffset == offsetof(StopRequest, sense));
    if (scenario == 6) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    scsi->header.ScsiStatus = (scenario == 2 || scenario == 7) ? 2 : 0;
    if (scenario == 2) {
        scsi->sense[0] = 0x70; scsi->sense[2] = 5; scsi->sense[12] = 0x20;
    } else if (scenario == 7) {
        scsi->sense[0] = 0x72; scsi->sense[1] = 5; scsi->sense[2] = 0x24;
    }
    *bytes = scenario == 4 ? 1 : sizeof(*scsi);
    return TRUE;
}

int main(void) {
    for (scenario = 0; scenario <= 8; ++scenario) {
        WCHAR report[512] = L"";
        ataCalls = scsiCalls = 0;
        BOOL ok = diskStandby(INVALID_HANDLE_VALUE, report, 512);
        assert(ok == (scenario == 0 || scenario == 1));
        assert(report[0]);
        if (scenario == 0) assert(ataCalls == 1 && scsiCalls == 0);
        if (scenario == 1) assert(scsiCalls == 1 && wcsstr(report, L"SCSI START STOP UNIT"));
        if (scenario == 2) assert(GetLastError() == ERROR_IO_DEVICE && wcsstr(report, L"05/20/00"));
        if (scenario == 3 || scenario == 5 || scenario == 8) assert(ataCalls == 0 && scsiCalls == 0);
        if (scenario == 4) assert(GetLastError() == ERROR_INVALID_DATA);
        if (scenario == 6) assert(GetLastError() == ERROR_ACCESS_DENIED);
        if (scenario == 7) assert(wcsstr(report, L"05/24/00"));
    }
    puts("PASS: 9 simulated driver cases (ATA registers, fallback, bus restriction, errors and short responses)");
    return 0;
}
