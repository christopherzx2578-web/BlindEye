// ═══════════════════════════════════════════════════════════════════════
//  DRIVER ENTRY & DEVICE INTERFACE (ESP-Enhanced)
//  Provides IOCTL interface for user-mode ESP overlay
// ═══════════════════════════════════════════════════════════════════════

#include "Types.h"
#include "DriverUtil.h"
#include "Hooks.h"
#include "SharedTypes.h"

using namespace DriverUtil;
using namespace Hooks;

// Global device object for user-mode communication
PDEVICE_OBJECT g_DeviceObject = nullptr;

// Cached offsets for ESP queries
static struct {
    ULONG64 gamePtr;
    ULONG64 world;
    ULONG64 playerMgr;
    ULONG64 entityList;
    ULONG64 entityCount;
    ULONG64 localCtrlWeak;
    ULONG64 charCtrl;
    ULONG64 lifeState;
    ULONG64 factionWeak;
    ULONG64 factionStr;
    ULONG64 dmgMgr;
    ULONG64 hitzone;
    ULONG64 maxHP;
    ULONG64 curHP;
    ULONG64 entityPos;
    ULONG64 meshComp;
    ULONG64 meshData;
    ULONG64 meshObj;
    ULONG64 boneArr;
    ULONG64 camMgrWeak;
    ULONG64 camWeak;
    ULONG64 camPos;
    ULONG64 camRight;
    ULONG64 camUp;
    ULONG64 camFwd;
    ULONG64 camFov;
    ULONG64 camZoom;
    ULONG64 camZoomFac;
    ULONG64 identList;
    ULONG64 identName;
    ULONG64 identEntWeak;
    ULONG64 weakObj;
    ULONG64 localEntNeg;
    ULONG   headBone;
    ULONG   boneStride;
    BOOLEAN valid;
} g_CachedOffsets = {};

static KSPIN_LOCK g_CacheLock;

// ───────────────────────────────────────────────────────────────────────
//  UTILITY: Safe memory read from target process
// ───────────────────────────────────────────────────────────────────────
static NTSTATUS SafeReadMemory(PEPROCESS Process, ULONG64 Address, PVOID Buffer, SIZE_T Length)
{
    if (!Process || !Address || !Buffer || !Length) 
        return STATUS_INVALID_PARAMETER;

    __try {
        ProbeForRead((PVOID)Address, Length, 1);
        RtlCopyMemory(Buffer, (PVOID)Address, Length);
        return STATUS_SUCCESS;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

// ───────────────────────────────────────────────────────────────────────
//  IOCTL HANDLERS
// ───────────────────────────────────────────────────────────────────────

// Generic memory read
static NTSTATUS IoctlReadMemory(PVOID InputBuf, ULONG InputLen, PVOID OutputBuf, ULONG OutputLen)
{
    if (InputLen < sizeof(BLINDEYE_READ_REQUEST) || OutputLen < sizeof(BLINDEYE_READ_RESPONSE))
        return STATUS_BUFFER_TOO_SMALL;

    auto* req = (BLINDEYE_READ_REQUEST*)InputBuf;
    auto* resp = (BLINDEYE_READ_RESPONSE*)OutputBuf;

    if (req->Size > sizeof(resp->Data))
        return STATUS_INVALID_PARAMETER;

    resp->BytesRead = 0;
    NTSTATUS status = SafeReadMemory(nullptr, req->Address, resp->Data, req->Size);
    if (NT_SUCCESS(status))
        resp->BytesRead = req->Size;

    return status;
}

// Get cached offsets
static NTSTATUS IoctlScanOffsets(PVOID InputBuf, ULONG InputLen, PVOID OutputBuf, ULONG OutputLen)
{
    UNREFERENCED_PARAMETER(InputBuf);
    UNREFERENCED_PARAMETER(InputLen);

    if (OutputLen < sizeof(BLINDEYE_SCAN_OFFSETS_RESPONSE))
        return STATUS_BUFFER_TOO_SMALL;

    auto* resp = (BLINDEYE_SCAN_OFFSETS_RESPONSE*)OutputBuf;

    KIRQL oldIrql = KeAcquireSpinLockRaiseToDpc(&g_CacheLock);
    if (g_CachedOffsets.valid) {
        resp->GamePtr       = g_CachedOffsets.gamePtr;
        resp->World         = g_CachedOffsets.world;
        resp->PlayerMgr     = g_CachedOffsets.playerMgr;
        resp->EntityList    = g_CachedOffsets.entityList;
        resp->EntityCount   = g_CachedOffsets.entityCount;
        resp->LocalCtrlWeak = g_CachedOffsets.localCtrlWeak;
        resp->CamMgrWeak    = g_CachedOffsets.camMgrWeak;
        resp->Status        = STATUS_SUCCESS;
    } else {
        resp->Status = STATUS_NOT_FOUND;
    }
    KeReleaseSpinLock(&g_CacheLock, oldIrql);

    return STATUS_SUCCESS;
}

// ───────────────────────────────────────────────────────────────────────
//  DEVICE CONTROL DISPATCH
// ───────────────────────────────────────────────────────────────────────
static NTSTATUS DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PVOID inputBuf  = Irp->AssociatedIrp.SystemBuffer;
    ULONG inputLen  = stack->Parameters.DeviceIoControl.InputBufferLength;
    PVOID outputBuf = Irp->AssociatedIrp.SystemBuffer;
    ULONG outputLen = stack->Parameters.DeviceIoControl.OutputBufferLength;

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_BLINDEYE_READ_MEMORY:
            status = IoctlReadMemory(inputBuf, inputLen, outputBuf, outputLen);
            break;
        case IOCTL_BLINDEYE_SCAN_OFFSETS:
            status = IoctlScanOffsets(inputBuf, inputLen, outputBuf, outputLen);
            break;
        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = (NT_SUCCESS(status)) ? outputLen : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// ───────────────────────────────────────────────────────────────────────
//  DRIVER UNLOAD
// ───────────────────────────────────────────────────────────────────────
void TdDeviceUnload(DRIVER_OBJECT* DriverObject)
{
    PsRemoveLoadImageNotifyRoutine(&LoadImageNotifyRoutine);
    DBG_PRINT("BlindEye unloaded.");

    if (g_DeviceObject) {
        UNICODE_STRING dosDevName = RTL_CONSTANT_STRING(L"\\DosDevices\\BlindEyeESP");
        IoDeleteSymbolicLink(&dosDevName);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = nullptr;
    }
}

// ───────────────────────────────────────────────────────────────────────
//  DRIVER ENTRY POINT
// ───────────────────────────────────────────────────────────────────────
extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DBG_PRINT("BlindEye v2 (ESP-Enhanced) loading...");

    // Initialize spinlock
    KeInitializeSpinLock(&g_CacheLock);

    // Create device for user-mode communication
    UNICODE_STRING deviceName = RTL_CONSTANT_STRING(L"\\Device\\BlindEyeESP");
    NTSTATUS status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_DeviceObject);

    if (!NT_SUCCESS(status)) {
        DBG_PRINT("IoCreateDevice failed: 0x%08X", status);
        return status;
    }

    // Create symbolic link
    UNICODE_STRING dosDevName = RTL_CONSTANT_STRING(L"\\DosDevices\\BlindEyeESP");
    status = IoCreateSymbolicLink(&dosDevName, &deviceName);
    if (!NT_SUCCESS(status)) {
        DBG_PRINT("IoCreateSymbolicLink failed: 0x%08X", status);
        IoDeleteDevice(g_DeviceObject);
        return status;
    }

    // Setup dispatch
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl;
    DriverObject->DriverUnload                         = TdDeviceUnload;

    // Install hooks
    PsSetLoadImageNotifyRoutine(&LoadImageNotifyRoutine);
    DBG_PRINT("Installed ImageNotifyRoutine... 0x%p", &LoadImageNotifyRoutine);
    DBG_PRINT("Device interface available at \\\\?\\BlindEyeESP");

    return STATUS_SUCCESS;
}
