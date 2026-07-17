/*------------------------------------------------------------------------------
 * cachectl.exe — User-mode CLI management tool for the WinCache driver
 *
 * Usage:
 *   cachectl attach  <source_disk> <cache_partition> [block_size]
 *   cachectl detach  <source_disk>
 *   cachectl stats   <source_disk>
 *   cachectl config  <source_disk>
 *   cachectl preload <source_disk> <file_path>
 *   cachectl flush   <source_disk>
 *   cachectl status
 *   cachectl install
 *   cachectl uninstall
 *
 * Build:
 *   build.bat (uses cl.exe from MSVC / EWDK)
 *------------------------------------------------------------------------------
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "..\shared\protocol.h"

/*----------------------------------------------------------------------------
 * Device path and service name
 *----------------------------------------------------------------------------*/
#define CACHEFLT_DEVICE_PATH  L"\\\\.\\cacheflt"
#define CACHEFLT_SERVICE_NAME L"cacheflt"

/*----------------------------------------------------------------------------
 * Forward declarations
 *----------------------------------------------------------------------------*/
static HANDLE OpenDriver(void);
static void   CloseDriver(HANDLE hDevice);
static void   PrintUsage(void);
static int    CmdAttach(int argc, wchar_t *argv[]);
static int    CmdDetach(int argc, wchar_t *argv[]);
static int    CmdStats(int argc, wchar_t *argv[]);
static int    CmdConfig(int argc, wchar_t *argv[]);
static int    CmdPreload(int argc, wchar_t *argv[]);
static int    CmdFlush(int argc, wchar_t *argv[]);
static int    CmdStatus(void);
static int    CmdServiceInstall(void);
static int    CmdServiceUninstall(void);

/*----------------------------------------------------------------------------
 * main
 *----------------------------------------------------------------------------*/
int wmain(int argc, wchar_t *argv[])
{
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    if (_wcsicmp(argv[1], L"attach")    == 0) return CmdAttach(argc, argv);
    if (_wcsicmp(argv[1], L"detach")    == 0) return CmdDetach(argc, argv);
    if (_wcsicmp(argv[1], L"stats")     == 0) return CmdStats(argc, argv);
    if (_wcsicmp(argv[1], L"config")    == 0) return CmdConfig(argc, argv);
    if (_wcsicmp(argv[1], L"preload")   == 0) return CmdPreload(argc, argv);
    if (_wcsicmp(argv[1], L"flush")     == 0) return CmdFlush(argc, argv);
    if (_wcsicmp(argv[1], L"status")    == 0) return CmdStatus();
    if (_wcsicmp(argv[1], L"install")   == 0) return CmdServiceInstall();
    if (_wcsicmp(argv[1], L"uninstall") == 0) return CmdServiceUninstall();
    if (_wcsicmp(argv[1], L"help") == 0 ||
        _wcsicmp(argv[1], L"--help") == 0 ||
        _wcsicmp(argv[1], L"-h") == 0) {
        PrintUsage();
        return 0;
    }

    wprintf(L"Unknown command: %s\n\n", argv[1]);
    PrintUsage();
    return 1;
}

/*----------------------------------------------------------------------------
 * PrintUsage
 *----------------------------------------------------------------------------*/
static void PrintUsage(void)
{
    wprintf(L"WinCache — Block-Level Read Acceleration\n\n");
    wprintf(L"USAGE:\n");
    wprintf(L"  cachectl attach  <source_phys>  <cache_part>  [block_size]\n");
    wprintf(L"  cachectl detach  <source_phys>\n");
    wprintf(L"  cachectl stats   <source_phys>\n");
    wprintf(L"  cachectl config  <source_phys>\n");
    wprintf(L"  cachectl preload <source_phys> <file_path>\n");
    wprintf(L"  cachectl flush   <source_phys>\n");
    wprintf(L"  cachectl status\n");
    wprintf(L"  cachectl install\n");
    wprintf(L"  cachectl uninstall\n");
}

/*----------------------------------------------------------------------------
 * OpenDriver / CloseDriver
 *----------------------------------------------------------------------------*/
static HANDLE OpenDriver(void)
{
    HANDLE hDevice = CreateFileW(
        CACHEFLT_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
        wprintf(L"ERROR: Cannot open %s (error %lu)\n",
            CACHEFLT_DEVICE_PATH, GetLastError());
        wprintf(L"Is the driver loaded? Try 'cachectl install' first.\n");
    }
    return hDevice;
}

static void CloseDriver(HANDLE hDevice)
{
    if (hDevice != NULL && hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(hDevice);
    }
}

/*----------------------------------------------------------------------------
 * CmdAttach
 *----------------------------------------------------------------------------*/
static int CmdAttach(int argc, wchar_t *argv[])
{
    HANDLE              hDevice;
    CACHE_ATTACH_INPUT  input;
    DWORD               bytes_returned;
    BOOL                ok;

    if (argc < 4) {
        wprintf(L"ERROR: Missing arguments.\n");
        wprintf(L"Usage: cachectl attach <source_phys> <cache_part> [block_size]\n");
        return 1;
    }

    hDevice = OpenDriver();
    if (hDevice == NULL) return 1;

    RtlZeroMemory(&input, sizeof(input));
    wcscpy_s(input.header.source_disk, 256, argv[2]);
    wcscpy_s(input.cache_partition, 256, argv[3]);
    input.block_size = (argc >= 5) ? (UINT32)_wtol(argv[4]) : 0;

    wprintf(L"Attaching cache...\n");
    wprintf(L"  Source: %s\n", input.header.source_disk);
    wprintf(L"  Cache:  %s\n", input.cache_partition);
    if (input.block_size > 0) {
        wprintf(L"  Block:  %u bytes\n", input.block_size);
    }

    ok = DeviceIoControl(
        hDevice, IOCTL_CACHE_ATTACH,
        &input, sizeof(input),
        NULL, 0, &bytes_returned, NULL);

    CloseDriver(hDevice);

    if (!ok) {
        wprintf(L"ERROR: DeviceIoControl(ATTACH) failed (error %lu)\n", GetLastError());
        return 1;
    }

    wprintf(L"Cache attached successfully.\n");
    return 0;
}

/*----------------------------------------------------------------------------
 * CmdDetach
 *----------------------------------------------------------------------------*/
static int CmdDetach(int argc, wchar_t *argv[])
{
    HANDLE              hDevice;
    CACHE_DETACH_INPUT  input;
    DWORD               bytes_returned;
    BOOL                ok;

    if (argc < 3) {
        wprintf(L"ERROR: Missing source disk argument.\n");
        return 1;
    }

    hDevice = OpenDriver();
    if (hDevice == NULL) return 1;

    RtlZeroMemory(&input, sizeof(input));
    wcscpy_s(input.source_disk, 256, argv[2]);

    ok = DeviceIoControl(
        hDevice, IOCTL_CACHE_DETACH,
        &input, sizeof(input),
        NULL, 0, &bytes_returned, NULL);

    CloseDriver(hDevice);

    if (!ok) {
        wprintf(L"ERROR: DeviceIoControl(DETACH) failed (error %lu)\n", GetLastError());
        return 1;
    }

    wprintf(L"Cache detached.\n");
    return 0;
}

/*----------------------------------------------------------------------------
 * CmdStats
 *----------------------------------------------------------------------------*/
static int CmdStats(int argc, wchar_t *argv[])
{
    HANDLE              hDevice;
    CACHE_STATS_INPUT   input;
    CACHE_STATS_OUTPUT  stats;
    DWORD               bytes_returned;
    BOOL                ok;

    if (argc < 3) {
        wprintf(L"ERROR: Missing source disk argument.\n");
        return 1;
    }

    hDevice = OpenDriver();
    if (hDevice == NULL) return 1;

    RtlZeroMemory(&input, sizeof(input));
    wcscpy_s(input.source_disk, 256, argv[2]);

    ok = DeviceIoControl(
        hDevice, IOCTL_CACHE_GET_STATS,
        &input, sizeof(input),
        &stats, sizeof(stats), &bytes_returned, NULL);

    CloseDriver(hDevice);

    if (!ok) {
        wprintf(L"ERROR: DeviceIoControl(STATS) failed (error %lu)\n", GetLastError());
        return 1;
    }

    wprintf(L"\n");
    wprintf(L"+------------------------------------------+\n");
    wprintf(L"|        Cache Statistics                  |\n");
    wprintf(L"+------------------------------------------+\n");
    wprintf(L"| Total reads:     %12llu       |\n", stats.total_reads);
    wprintf(L"| Cache hits:      %12llu       |\n", stats.cache_hits);
    wprintf(L"| Cache misses:    %12llu       |\n", stats.cache_misses);
    wprintf(L"| Hit rate:        %11llu.%%      |\n",
        stats.hit_rate_percent_x100 / 100);
    wprintf(L"+------------------------------------------+\n");
    wprintf(L"| Bytes read:      %12llu       |\n", stats.bytes_read_total);
    wprintf(L"| From cache:      %12llu       |\n", stats.bytes_read_from_cache);
    wprintf(L"+------------------------------------------+\n");
    wprintf(L"| Blocks cached:   %12llu       |\n", stats.blocks_cached);
    wprintf(L"| Blocks total:    %12llu       |\n", stats.blocks_total);
    wprintf(L"| Populations:     %12llu       |\n", stats.populations_completed);
    wprintf(L"| Evictions:       %12llu       |\n", stats.evictions);
    wprintf(L"+------------------------------------------+\n");

    return 0;
}

/*----------------------------------------------------------------------------
 * CmdConfig
 *----------------------------------------------------------------------------*/
static int CmdConfig(int argc, wchar_t *argv[])
{
    HANDLE              hDevice;
    CACHE_CONFIG_INPUT  input;
    CACHE_CONFIG_OUTPUT config;
    DWORD               bytes_returned;
    BOOL                ok;

    if (argc < 3) {
        wprintf(L"ERROR: Missing source disk argument.\n");
        return 1;
    }

    hDevice = OpenDriver();
    if (hDevice == NULL) return 1;

    RtlZeroMemory(&input, sizeof(input));
    wcscpy_s(input.source_disk, 256, argv[2]);

    ok = DeviceIoControl(
        hDevice, IOCTL_CACHE_GET_CONFIG,
        &input, sizeof(input),
        &config, sizeof(config), &bytes_returned, NULL);

    CloseDriver(hDevice);

    if (!ok) {
        wprintf(L"ERROR: DeviceIoControl(CONFIG) failed (error %lu)\n", GetLastError());
        return 1;
    }

    wprintf(L"Cache Configuration:\n");
    wprintf(L"  Active:        %s\n", config.is_active ? L"YES" : L"NO");
    wprintf(L"  Block size:    %u bytes\n", config.block_size);
    wprintf(L"  Cache blocks:  %llu / %llu\n",
        config.cached_blocks, config.total_cache_blocks);

    return 0;
}

/*----------------------------------------------------------------------------
 * CmdPreload
 *----------------------------------------------------------------------------*/
static int CmdPreload(int argc, wchar_t *argv[])
{
    HANDLE              hDevice;
    HANDLE              hFile;
    CACHE_PRELOAD_INPUT preload;
    DWORD               bytes_returned;
    BOOL                ok;
    STARTING_VCN_INPUT_BUFFER  start_vcn;
    RETRIEVAL_POINTERS_BUFFER  extents[256];
    DWORD               i, extent_count;

    if (argc < 4) {
        wprintf(L"ERROR: Missing arguments.\n");
        wprintf(L"Usage: cachectl preload <source_phys> <file_path>\n");
        return 1;
    }

    /* Open the file to get its disk extents */
    hFile = CreateFileW(
        argv[3], GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        wprintf(L"ERROR: Cannot open file: %s (error %lu)\n",
            argv[3], GetLastError());
        return 1;
    }

    /* Get file extents via FSCTL_GET_RETRIEVAL_POINTERS */
    start_vcn.StartingVcn.QuadPart = 0;

    ok = DeviceIoControl(
        hFile, FSCTL_GET_RETRIEVAL_POINTERS,
        &start_vcn, sizeof(start_vcn),
        extents, sizeof(extents), &bytes_returned, NULL);

    CloseHandle(hFile);

    if (!ok) {
        wprintf(L"ERROR: Cannot get file extents (error %lu). "
                L"Try without FILE_FLAG_NO_BUFFERING.\n", GetLastError());
        return 1;
    }

    extent_count = (bytes_returned - sizeof(LARGE_INTEGER)) /
                   sizeof(RETRIEVAL_POINTERS_BUFFER);

    hDevice = OpenDriver();
    if (hDevice == NULL) return 1;

    RtlZeroMemory(&preload, sizeof(preload));
    wcscpy_s(preload.header.source_disk, 256, argv[2]);

    for (i = 0; i < extent_count; i++) {
        preload.start_lba = extents[i].Lcn.QuadPart;
        preload.end_lba   = extents[i].Lcn.QuadPart +
                            extents[i].NextVcn.QuadPart -
                            extents[i].StartingVcn.QuadPart - 1;

        wprintf(L"  Preloading extent %lu: LBA %llu - %llu\n",
            i, preload.start_lba, preload.end_lba);

        DeviceIoControl(
            hDevice, IOCTL_CACHE_PRELOAD_RANGE,
            &preload, sizeof(preload),
            NULL, 0, &bytes_returned, NULL);
    }

    CloseDriver(hDevice);
    wprintf(L"Preload complete.\n");
    return 0;
}

/*----------------------------------------------------------------------------
 * CmdFlush
 *----------------------------------------------------------------------------*/
static int CmdFlush(int argc, wchar_t *argv[])
{
    HANDLE              hDevice;
    CACHE_FLUSH_INPUT   input;
    DWORD               bytes_returned;
    BOOL                ok;

    if (argc < 3) {
        wprintf(L"ERROR: Missing source disk argument.\n");
        return 1;
    }

    hDevice = OpenDriver();
    if (hDevice == NULL) return 1;

    RtlZeroMemory(&input, sizeof(input));
    wcscpy_s(input.source_disk, 256, argv[2]);

    ok = DeviceIoControl(
        hDevice, IOCTL_CACHE_FLUSH,
        &input, sizeof(input),
        NULL, 0, &bytes_returned, NULL);

    CloseDriver(hDevice);

    if (!ok) {
        wprintf(L"ERROR: DeviceIoControl(FLUSH) failed (error %lu)\n", GetLastError());
        return 1;
    }

    wprintf(L"Cache index flushed to SSD.\n");
    return 0;
}

/*----------------------------------------------------------------------------
 * CmdStatus
 *----------------------------------------------------------------------------*/
static int CmdStatus(void)
{
    SC_HANDLE hSCM, hService;

    hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCM == NULL) {
        wprintf(L"ERROR: Cannot open SCM (error %lu). Run as Administrator.\n",
            GetLastError());
        return 1;
    }

    hService = OpenServiceW(hSCM, CACHEFLT_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (hService == NULL) {
        wprintf(L"Driver '%s' is not installed.\n", CACHEFLT_SERVICE_NAME);
        wprintf(L"Run 'cachectl install' to install it.\n");
        CloseServiceHandle(hSCM);
        return 0;
    }

    {
        SERVICE_STATUS status;
        if (QueryServiceStatus(hService, &status)) {
            switch (status.dwCurrentState) {
            case SERVICE_RUNNING:
                wprintf(L"Driver '%s' is RUNNING.\n", CACHEFLT_SERVICE_NAME);
                break;
            case SERVICE_STOPPED:
                wprintf(L"Driver '%s' is STOPPED.\n", CACHEFLT_SERVICE_NAME);
                break;
            default:
                wprintf(L"Driver '%s' state: %lu\n",
                    CACHEFLT_SERVICE_NAME, status.dwCurrentState);
                break;
            }
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return 0;
}

/*----------------------------------------------------------------------------
 * CmdServiceInstall
 *----------------------------------------------------------------------------*/
static int CmdServiceInstall(void)
{
    SC_HANDLE   hSCM, hService;
    wchar_t     driver_path[MAX_PATH];
    DWORD       len;

    len = GetSystemDirectoryW(driver_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        wprintf(L"ERROR: Cannot get system directory.\n");
        return 1;
    }
    wcscat_s(driver_path, MAX_PATH, L"\\drivers\\cacheflt.sys");

    if (GetFileAttributesW(driver_path) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"ERROR: Driver file not found at %s\n", driver_path);
        wprintf(L"Copy cacheflt.sys to %%SystemRoot%%\\system32\\drivers\\ first.\n");
        return 1;
    }

    hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (hSCM == NULL) {
        wprintf(L"ERROR: OpenSCManager failed (error %lu). Run as Administrator.\n",
            GetLastError());
        return 1;
    }

    hService = CreateServiceW(
        hSCM, CACHEFLT_SERVICE_NAME,
        L"WinCache Filter Driver",
        SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS,
        SERVICE_FILE_SYSTEM_DRIVER,
        SERVICE_BOOT_START,
        SERVICE_ERROR_NORMAL,
        driver_path,
        L"PNP Filter", NULL, NULL, NULL, NULL);

    if (hService == NULL) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            wprintf(L"Service already exists. Try 'cachectl uninstall' first.\n");
        } else {
            wprintf(L"ERROR: CreateService failed (error %lu)\n", err);
        }
        CloseServiceHandle(hSCM);
        return 1;
    }

    wprintf(L"Driver service installed.\n");
    wprintf(L"Service path: %s\n", driver_path);

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return 0;
}

/*----------------------------------------------------------------------------
 * CmdServiceUninstall
 *----------------------------------------------------------------------------*/
static int CmdServiceUninstall(void)
{
    SC_HANDLE       hSCM, hService;
    SERVICE_STATUS  status;

    hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (hSCM == NULL) {
        wprintf(L"ERROR: OpenSCManager failed (error %lu). Run as Administrator.\n",
            GetLastError());
        return 1;
    }

    hService = OpenServiceW(hSCM, CACHEFLT_SERVICE_NAME,
        SERVICE_STOP | DELETE);
    if (hService == NULL) {
        wprintf(L"Service '%s' not found.\n", CACHEFLT_SERVICE_NAME);
        CloseServiceHandle(hSCM);
        return 1;
    }

    if (ControlService(hService, SERVICE_CONTROL_STOP, &status)) {
        wprintf(L"Service stopped.\n");
        Sleep(1000);
    }

    if (!DeleteService(hService)) {
        wprintf(L"ERROR: DeleteService failed (error %lu)\n", GetLastError());
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return 1;
    }

    wprintf(L"Driver service uninstalled.\n");
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return 0;
}
