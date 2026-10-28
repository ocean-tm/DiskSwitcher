/* One-shot repair diagnostic for the user's identified, already-offline HDD.
   It refuses every other serial, size, disk number or Windows system disk. */
#include "../main.c"

int wmain(void) {
    HANDLE h = CreateFileW(L"\\\\.\\PhysicalDrive1", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("Open failed: %lu\n", GetLastError()); return 1; }
    int result = 1;
    DWORD bytes, os[256], osCount;
    STORAGE_DEVICE_NUMBER number;
    GET_LENGTH_INFORMATION length;
    GET_DISK_ATTRIBUTES attrs;
    STORAGE_PROPERTY_QUERY query = {0};
    BYTE buffer[1024] = {0};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                         &number, sizeof(number), &bytes, NULL) ||
        number.DeviceType != FILE_DEVICE_DISK || number.DeviceNumber != 1 ||
        !DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                         &length, sizeof(length), &bytes, NULL) ||
        length.Length.QuadPart != 2000398934016LL ||
        !getAttributes(h, &attrs) || !(attrs.Attributes & DISK_ATTRIBUTE_OFFLINE) ||
        !windowsDisks(os, 256, &osCount) || containsDisk(number.DeviceNumber, os, osCount)) {
        puts("REFUSED: target identity, offline state or Windows protection check failed.");
        goto done;
    }
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                         buffer, sizeof(buffer), &bytes, NULL) || bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        puts("REFUSED: cannot verify serial."); goto done;
    }
    STORAGE_DEVICE_DESCRIPTOR descriptor;
    CopyMemory(&descriptor, buffer, sizeof(descriptor));
    DWORD offset = descriptor.SerialNumberOffset;
    if (!offset || offset >= bytes || bytes > sizeof(buffer) ||
        !memchr(buffer + offset, 0, bytes - offset)) {
        puts("REFUSED: invalid serial response."); goto done;
    }
    char *serial = (char *)buffer + offset;
    while (*serial == ' ') ++serial;
    size_t size = strlen(serial);
    while (size && serial[size - 1] == ' ') serial[--size] = 0;
    if (strcmp(serial, "WFL4Q67Z") != 0) { puts("REFUSED: serial mismatch."); goto done; }
    WCHAR report[512];
    BOOL accepted = diskStandby(h, report, 512);
    wprintf(L"%ls\n", report);
    FILE *log = _wfopen(L"D:\\www\\disc-switcher\\build\\target-stop-result.txt", L"w");
    if (log) { fwprintf(log, L"%ls\n", report); fclose(log); }
    result = accepted ? 0 : 2;
done:
    CloseHandle(h);
    return result;
}
