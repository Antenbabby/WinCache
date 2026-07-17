/*------------------------------------------------------------------------------
 * cacheflt.c — Driver entry, device add, and unload
 *
 * This is a KMDF-based lower filter driver for the Disk class. It registers
 * as a class filter so the PnP manager attaches it automatically below
 * Disk.sys on every disk device. By default, caching is inactive (pass-through
 * only). The user-mode manager activates caching on specific disks via IOCTL.
 *
 * We also create a named control device (\Device\cacheflt) for the user-mode
 * manager to send IOCTLs to.
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

#include "cacheflt.h"
#include "..\shared\protocol.h"

/*----------------------------------------------------------------------------
 * Globals
 *----------------------------------------------------------------------------*/
LIST_ENTRY   g_cache_disk_list;       /* linked list of CACHE_DISK_CONTEXT.global_list_entry */
KSPIN_LOCK   g_cache_disk_list_lock;  /* protects g_cache_disk_list */
PDEVICE_OBJECT g_control_device = NULL; /* named control device \Device\cacheflt */

/*----------------------------------------------------------------------------
 * Control device dispatch (WDM-style, handles IOCTLs only)
 *----------------------------------------------------------------------------*/
DRIVER_DISPATCH CacheFilterControlDeviceDispatch;

/*
 * Device name and symbolic link for the control device.
 */
#define CONTROL_DEVICE_NAME     L"\\Device\\cacheflt"
#define CONTROL_SYMLINK_NAME    L"\\DosDevices\\cacheflt"

/*----------------------------------------------------------------------------
 * DriverEntry
 *
 * Called by the I/O manager when the driver is loaded.
 * 1. Create the KMDF driver object for filter device instances.
 * 2. Create a named WDM control device for user-mode IOCTLs.
 *----------------------------------------------------------------------------*/
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS            status;
    WDF_DRIVER_CONFIG   config;
    WDF_OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING      device_name;
    UNICODE_STRING      symlink_name;
    PDEVICE_OBJECT      control_device = NULL;

    KdPrint(("CacheFlt: DriverEntry\n"));

    /*
     * --- Part 1: Initialize global state ---
     */
    InitializeListHead(&g_cache_disk_list);
    KeInitializeSpinLock(&g_cache_disk_list_lock);

    /*
     * --- Part 2: Create the KMDF driver for filter device objects ---
     */
    WDF_DRIVER_CONFIG_INIT(&config, CacheFilterEvtDeviceAdd);
    config.EvtDriverUnload = CacheFilterEvtDriverUnload;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: WdfDriverCreate failed 0x%08x\n", status));
        return status;
    }

    /*
     * --- Part 3: Create a named control device for user-mode IOCTLs ---
     *
     * We use raw WDM APIs here because KMDF control devices are tied to
     * a PnP device stack, which we don't have. The control device is a
     * simple unnamed WDM device with a symbolic link.
     */
    RtlInitUnicodeString(&device_name, CONTROL_DEVICE_NAME);
    RtlInitUnicodeString(&symlink_name, CONTROL_SYMLINK_NAME);

    status = IoCreateDevice(
        DriverObject,
        0,                          /* no device extension needed */
        &device_name,
        FILE_DEVICE_UNKNOWN,
        0,                          /* no device characteristics */
        FALSE,                      /* not exclusive */
        &control_device);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: IoCreateDevice(control) failed 0x%08x\n", status));
        return status;
    }

    /*
     * Set up the dispatch table for the control device.
     * We only handle IRP_MJ_DEVICE_CONTROL; other IRPs get failed.
     */
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CacheFilterControlDeviceDispatch;
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = CacheFilterControlDeviceDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = CacheFilterControlDeviceDispatch;

    /*
     * Create the symbolic link so user mode can open \\.\cacheflt.
     */
    status = IoCreateSymbolicLink(&symlink_name, &device_name);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: IoCreateSymbolicLink failed 0x%08x\n", status));
        IoDeleteDevice(control_device);
        return status;
    }

    /*
     * Mark the control device as clear-initialize so buffers are zeroed.
     */
    control_device->Flags |= DO_BUFFERED_IO;
    g_control_device = control_device;

    KdPrint(("CacheFlt: Driver loaded successfully, control device created\n"));
    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheFilterControlDeviceDispatch
 *
 * WDM dispatch routine for the control device (\Device\cacheflt).
 * Handles IRP_MJ_DEVICE_CONTROL (IOCTLs) by looking up the target disk
 * context from the input buffer and routing to the appropriate handler.
 *
 * IRP_MJ_CREATE / IRP_MJ_CLOSE just succeed — they're needed so user mode
 * can open/close the device.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterControlDeviceDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    PIO_STACK_LOCATION  irp_sp;
    NTSTATUS            status;
    ULONG               ioctl_code;
    PVOID               input_buffer;
    PVOID               output_buffer;
    ULONG               input_len;
    ULONG               output_len;
    ULONG               info;
    CACHE_DISK_CONTEXT *target_ctx;

    UNREFERENCED_PARAMETER(DeviceObject);

    irp_sp = IoGetCurrentIrpStackLocation(Irp);
    info = 0;

    switch (irp_sp->MajorFunction) {

    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;

    case IRP_MJ_DEVICE_CONTROL:
        ioctl_code = irp_sp->Parameters.DeviceIoControl.IoControlCode;
        input_len  = irp_sp->Parameters.DeviceIoControl.InputBufferLength;
        output_len = irp_sp->Parameters.DeviceIoControl.OutputBufferLength;

        /*
         * METHOD_BUFFERED: input and output share the system buffer.
         */
        input_buffer  = Irp->AssociatedIrp.SystemBuffer;
        output_buffer = Irp->AssociatedIrp.SystemBuffer;

        /*
         * Route to the appropriate handler based on IOCTL code.
         *
         * For IOCTLs that target a specific disk (ATTACH, STATS, etc.),
         * the input buffer's first field is the source disk name.
         * We look up the corresponding CACHE_DISK_CONTEXT from the global list.
         */
        switch (ioctl_code) {

        case IOCTL_CACHE_ATTACH:
            if (input_buffer != NULL && input_len >= sizeof(CACHE_ATTACH_INPUT)) {
                target_ctx = CacheDiskLookupBySourceName(
                    ((CACHE_ATTACH_INPUT *)input_buffer)->header.source_disk);
                if (target_ctx != NULL) {
                    status = CacheFilterIoctlAttach(
                        target_ctx,
                        (CACHE_ATTACH_INPUT *)input_buffer,
                        input_len);
                } else {
                    /*
                     * No matching context found — the disk's filter device
                     * hasn't been added yet. For now, return error.
                     * In production, we'd queue this until the device arrives.
                     */
                    status = STATUS_DEVICE_NOT_READY;
                }
            } else {
                status = STATUS_INVALID_PARAMETER;
            }
            break;

        case IOCTL_CACHE_DETACH:
        case IOCTL_CACHE_GET_STATS:
        case IOCTL_CACHE_RESET_STATS:
        case IOCTL_CACHE_GET_CONFIG:
        case IOCTL_CACHE_FLUSH:
        case IOCTL_CACHE_PRELOAD_RANGE:
            /*
             * All these IOCTLs require a CACHE_IOCTL_HEADER as input.
             * Look up the target disk context from the global list.
             */
            if (input_buffer != NULL && input_len >= sizeof(CACHE_IOCTL_HEADER)) {
                target_ctx = CacheDiskLookupBySourceName(
                    ((CACHE_IOCTL_HEADER *)input_buffer)->source_disk);
            } else {
                target_ctx = NULL;
            }

            if (target_ctx == NULL) {
                status = STATUS_DEVICE_NOT_READY;
                break;
            }

            switch (ioctl_code) {
            case IOCTL_CACHE_DETACH:
                status = CacheFilterIoctlDetach(target_ctx);
                break;
            case IOCTL_CACHE_GET_STATS:
                status = CacheFilterIoctlGetStats(
                    target_ctx,
                    (CACHE_STATS_OUTPUT *)output_buffer,
                    output_len);
                break;
            case IOCTL_CACHE_RESET_STATS:
                status = CacheFilterIoctlResetStats(target_ctx);
                break;
            case IOCTL_CACHE_GET_CONFIG:
                status = CacheFilterIoctlGetConfig(
                    target_ctx,
                    (CACHE_CONFIG_OUTPUT *)output_buffer,
                    output_len);
                break;
            case IOCTL_CACHE_FLUSH:
                status = CacheFilterIoctlFlush(target_ctx);
                break;
            case IOCTL_CACHE_PRELOAD_RANGE:
                status = CacheFilterIoctlPreloadRange(
                    target_ctx,
                    (CACHE_PRELOAD_INPUT *)input_buffer,
                    input_len);
                break;
            }
            break;

        default:
            status = STATUS_NOT_SUPPORTED;
            break;
        }

        info = (NT_SUCCESS(status)) ? output_len : 0;
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

/*----------------------------------------------------------------------------
 * CacheDiskLookupBySourceName
 *
 * Walk the global CACHE_DISK_CONTEXT list looking for one whose source
 * physical device matches the given name.
 *
 * Must be called with g_cache_disk_list_lock held (or at IRQL where
 * acquiring the lock is safe — PASSIVE_LEVEL for IOCTL handlers).
 *----------------------------------------------------------------------------*/
CACHE_DISK_CONTEXT*
CacheDiskLookupBySourceName(
    _In_ PCWSTR source_name)
{
    PLIST_ENTRY         entry;
    CACHE_DISK_CONTEXT *ctx;
    KLOCK_QUEUE_HANDLE  lock_handle;
    CACHE_DISK_CONTEXT *found = NULL;

    UNREFERENCED_PARAMETER(source_name);

    KeAcquireInStackQueuedSpinLock(&g_cache_disk_list_lock, &lock_handle);

    for (entry = g_cache_disk_list.Flink;
         entry != &g_cache_disk_list;
         entry = entry->Flink) {

        ctx = CONTAINING_RECORD(entry, CACHE_DISK_CONTEXT, global_list_entry);

        /*
         * Compare source device name. In a full implementation, we'd get
         * the device name via IoGetDeviceProperty or WdfDeviceGetProperty
         * and compare. For now, just return the first active context.
         */
        if (ctx->caching_active || !ctx->caching_active) {
            /* placeholder — full name comparison goes here */
            found = ctx;
            break;
        }
    }

    KeReleaseInStackQueuedSpinLock(&lock_handle);
    return found;
}

/*----------------------------------------------------------------------------
 * CacheFilterEvtDeviceAdd
 *
 * Called by KMDF for each device our driver filters. We create a filter
 * device object, set up our context, create I/O queues, and register
 * this context in the global list.
 *
 * IMPORTANT: We use WdfFdoInitSetFilter() to mark ourselves as a filter.
 * The "LowerFilters" registry entry (set by our INF) causes the PnP manager
 * to call us for all Disk class devices.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterEvtDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS              status;
    WDFDEVICE             device;
    WDF_IO_QUEUE_CONFIG   queue_config;
    WDF_OBJECT_ATTRIBUTES obj_attributes;
    WDFQUEUE              queue;
    CACHE_DISK_CONTEXT   *ctx;
    KLOCK_QUEUE_HANDLE    lock_handle;

    UNREFERENCED_PARAMETER(Driver);

    /*
     * Mark us as a filter driver.
     */
    WdfFdoInitSetFilter(DeviceInit);

    /*
     * Set up device context for our CACHE_DISK_CONTEXT structure.
     */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&obj_attributes, CACHE_DISK_CONTEXT);
    obj_attributes.EvtCleanupCallback = CacheFilterEvtDeviceContextCleanup;

    /*
     * Create the filter device object.
     */
    status = WdfDeviceCreate(&DeviceInit, &obj_attributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: WdfDeviceCreate failed 0x%08x\n", status));
        return status;
    }

    /*
     * Initialize our device context.
     */
    ctx = CacheDiskGetContext(device);
    RtlZeroMemory(ctx, sizeof(CACHE_DISK_CONTEXT));
    KeInitializeSpinLock(&ctx->block_map_lock);
    KeInitializeSpinLock(&ctx->pending_lock);
    InitializeListHead(&ctx->pending_populations);
    ctx->caching_active = FALSE;

    /*
     * Store the lower device as our I/O target for pass-through.
     * WdfDeviceGetIoTarget returns the default (next-lower-driver) target.
     */
    ctx->source_io_target = WdfDeviceGetIoTarget(device);

    /*
     * The source physical device object is the device we filter.
     * We retrieve it from the WDFDEVICE for later use in direct I/O.
     */
    ctx->source_physical_device = WdfDeviceWdmGetPhysicalDevice(device);

    /*
     * Register this context in the global list so the control device
     * IOCTL handler can find it.
     */
    KeAcquireInStackQueuedSpinLock(&g_cache_disk_list_lock, &lock_handle);
    InsertTailList(&g_cache_disk_list, &ctx->global_list_entry);
    KeReleaseInStackQueuedSpinLock(&lock_handle);

    /*
     * Create a parallel I/O queue — requests come in on multiple CPUs.
     * Parallel queue means KMDF can deliver requests concurrently.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchParallel);
    queue_config.EvtIoRead          = CacheFilterEvtIoRead;
    queue_config.EvtIoWrite         = CacheFilterEvtIoWrite;
    queue_config.EvtIoDeviceControl = CacheFilterEvtIoDeviceControl;
    queue_config.PowerManaged       = WdfFalse;  /* don't power-manage I/O */

    status = WdfIoQueueCreate(
        device,
        &queue_config,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: WdfIoQueueCreate failed 0x%08x\n", status));

        /* Remove from global list on failure */
        KeAcquireInStackQueuedSpinLock(&g_cache_disk_list_lock, &lock_handle);
        RemoveEntryList(&ctx->global_list_entry);
        KeReleaseInStackQueuedSpinLock(&lock_handle);

        return status;
    }

    KdPrint(("CacheFlt: Device added successfully\n"));
    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheFilterEvtDeviceContextCleanup
 *
 * Called when the device is being removed. Free all resources and
 * remove from the global context list.
 *----------------------------------------------------------------------------*/
VOID
CacheFilterEvtDeviceContextCleanup(
    _In_ WDFOBJECT Device)
{
    CACHE_DISK_CONTEXT *ctx = CacheDiskGetContext((WDFDEVICE)Device);
    KLOCK_QUEUE_HANDLE   lock_handle;

    KdPrint(("CacheFlt: Device cleanup\n"));

    /*
     * Remove from the global list first — we don't want the control
     * device IOCTL handler to find a context that's being torn down.
     */
    KeAcquireInStackQueuedSpinLock(&g_cache_disk_list_lock, &lock_handle);
    if (!IsListEmpty(&ctx->global_list_entry)) {
        RemoveEntryList(&ctx->global_list_entry);
    }
    KeReleaseInStackQueuedSpinLock(&lock_handle);

    /*
     * Flush the index to SSD if caching was active.
     */
    if (ctx->caching_active && ctx->block_map != NULL) {
        IndexFlushToSsd(ctx);
    }

    /*
     * Free non-paged pool used for the block map.
     */
    if (ctx->block_map != NULL) {
        ExFreePoolWithTag(ctx->block_map, 'HCAC');
        ctx->block_map = NULL;
    }

    /*
     * Close the cache partition file object.
     */
    if (ctx->cache_file_object != NULL) {
        ObDereferenceObject(ctx->cache_file_object);
        ctx->cache_file_object = NULL;
    }

    /*
     * Free work items.
     */
    if (ctx->populate_work_item != NULL) {
        IoFreeWorkItem(ctx->populate_work_item);
        ctx->populate_work_item = NULL;
    }
    if (ctx->index_sync_work_item != NULL) {
        IoFreeWorkItem(ctx->index_sync_work_item);
        ctx->index_sync_work_item = NULL;
    }
}

/*----------------------------------------------------------------------------
 * CacheFilterEvtDriverUnload
 *
 * Called when the driver is being unloaded. Delete the control device
 * and symbolic link.
 *----------------------------------------------------------------------------*/
VOID
CacheFilterEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNICODE_STRING symlink_name;

    UNREFERENCED_PARAMETER(Driver);

    KdPrint(("CacheFlt: Driver unload\n"));

    /*
     * Delete the symbolic link and control device.
     */
    RtlInitUnicodeString(&symlink_name, CONTROL_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlink_name);

    if (g_control_device != NULL) {
        IoDeleteDevice(g_control_device);
        g_control_device = NULL;
    }

    KdPrint(("CacheFlt: Driver unloaded\n"));
}
