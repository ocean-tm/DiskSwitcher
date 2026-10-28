#include "power.h"
#include <winioctl.h>
#include <ntddscsi.h>
#include <wchar.h>

typedef struct {
    SCSI_PASS_THROUGH header;
    BYTE sense[32];
} StopRequest;

BOOL diskStandby(HANDLE disk, WCHAR *report, size_t capacity) {
    DWORD bytes = 0;
    STORAGE_PROPERTY_QUERY query = {0};
    BYTE buffer[1024] = {0};
    STORAGE_DEVICE_DESCRIPTOR descriptor;
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                         buffer, sizeof(buffer), &bytes, NULL)) {
        DWORD error = GetLastError();
        swprintf(report, capacity, L"Device properties: Win32 %lu", error);
        SetLastError(error);
        return FALSE;
    }
    if (bytes < sizeof(descriptor)) {
        swprintf(report, capacity, L"Device properties: incomplete response");
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    CopyMemory(&descriptor, buffer, sizeof(descriptor));
    if (descriptor.BusType != BusTypeSata && descriptor.BusType != BusTypeAta &&
        descriptor.BusType != BusTypeScsi && descriptor.BusType != BusTypeRAID) {
        swprintf(report, capacity, L"Unsupported bus type: %u", (unsigned)descriptor.BusType);
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    ATA_PASS_THROUGH_EX ata = {0};
    ata.Length = sizeof(ata);
    ata.AtaFlags = ATA_FLAGS_DRDY_REQUIRED;
    ata.TimeOutValue = 10;
    /* Microsoft: CurrentTaskFile is BOTH input and output. PreviousTaskFile
       is only used for 48-bit commands. E0h is a non-data, non-48-bit command. */
    ata.CurrentTaskFile[5] = 0xA0;
    ata.CurrentTaskFile[6] = 0xE0;
    BOOL ok = DeviceIoControl(disk, IOCTL_ATA_PASS_THROUGH, &ata, sizeof(ata),
                             &ata, sizeof(ata), &bytes, NULL);
    DWORD ataError = ok ? ERROR_SUCCESS : GetLastError();
    BYTE ataStatus = ata.CurrentTaskFile[6], ataDeviceError = ata.CurrentTaskFile[0];
    if (ok && bytes >= sizeof(ata) && !(ataStatus & 0xA9)) {
        swprintf(report, capacity, L"ATA STANDBY IMMEDIATE: accepted (status 0x%02X)", ataStatus);
        return TRUE;
    }
    if (ok) ataError = bytes < sizeof(ata) ? ERROR_INVALID_DATA : ERROR_IO_DEVICE;

    /* Send through the disk class device, never directly to the SATA adapter.
       START=0, LOEJ=0, IMMED=0: stop the spindle, without eject or no-flush. */
    StopRequest scsi = {0};
    scsi.header.Length = sizeof(scsi.header);
    scsi.header.CdbLength = 6;
    scsi.header.SenseInfoLength = sizeof(scsi.sense);
    scsi.header.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    scsi.header.TimeOutValue = 20;
    scsi.header.SenseInfoOffset = offsetof(StopRequest, sense);
    scsi.header.Cdb[0] = 0x1B;
    ok = DeviceIoControl(disk, IOCTL_SCSI_PASS_THROUGH, &scsi, sizeof(scsi),
                         &scsi, sizeof(scsi), &bytes, NULL);
    DWORD scsiError = ok ? ERROR_SUCCESS : GetLastError();
    if (ok && bytes >= sizeof(scsi.header) && scsi.header.ScsiStatus == 0) {
        swprintf(report, capacity, L"SCSI START STOP UNIT: accepted (ATA Win32 %lu)", ataError);
        return TRUE;
    }
    if (ok) scsiError = bytes < sizeof(scsi.header) ? ERROR_INVALID_DATA : ERROR_IO_DEVICE;
    BYTE key = 0, asc = 0, ascq = 0;
    if (bytes > offsetof(StopRequest, sense) && scsi.header.SenseInfoLength) {
        DWORD available = bytes - (DWORD)offsetof(StopRequest, sense);
        if (available > sizeof(scsi.sense)) available = sizeof(scsi.sense);
        if (available > scsi.header.SenseInfoLength) available = scsi.header.SenseInfoLength;
        BYTE format = scsi.sense[0] & 0x7F;
        if ((format == 0x70 || format == 0x71) && available >= 14) {
            key = scsi.sense[2] & 0x0F; asc = scsi.sense[12]; ascq = scsi.sense[13];
        } else if ((format == 0x72 || format == 0x73) && available >= 4) {
            key = scsi.sense[1] & 0x0F; asc = scsi.sense[2]; ascq = scsi.sense[3];
        }
    }
    swprintf(report, capacity,
        L"ATA: Win32 %lu, status 0x%02X, error 0x%02X\r\n"
        L"SCSI: Win32 %lu, status 0x%02X, sense %02X/%02X/%02X",
        ataError, ataStatus, ataDeviceError, scsiError, scsi.header.ScsiStatus, key, asc, ascq);
    SetLastError(scsiError);
    return FALSE;
}
