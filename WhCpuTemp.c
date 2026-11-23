// WhCpuTemp.sys — минимальный драйвер только для ЧТЕНИЯ температуры CPU.
// Безопасность: нет произвольного доступа к MSR/PCI. Драйвер сам читает
// только фиксированные регистры температуры и отдаёт готовые градусы.
// Intel: IA32_THERM_STATUS (0x19C) по каждому ядру, IA32_PACKAGE_THERM_STATUS (0x1B1), TjMax из 0x1A2.
// AMD Zen: Tctl и температуры CCD через SMN (PCI 0:0.0, регистры 0x60/0x64).
// Сборка: WDK, проект "Kernel Mode Driver, Empty (KMDF/WDM)", x64.
#include <ntddk.h>
#include <intrin.h>
#include "WhCpuTemp.h"

#define MSR_THERM_STATUS     0x19C
#define MSR_TEMP_TARGET      0x1A2
#define MSR_PKG_THERM_STATUS 0x1B1
#define SMN_TCTL             0x00059800
#define SMN_CCD_BASE         0x00059954

static FAST_MUTEX g_smnLock;
static int g_vendor; // 1 Intel, 2 AMD

static int CpuVendor(void) {
  int r[4]; __cpuid(r, 0);
  if (r[1] == 0x756E6547) return 1;                       // "GenuineIntel"
  if (r[1] == 0x68747541) return 2;                       // "AuthenticAMD"
  return 0;
}

static ULONG SmnRead(ULONG addr) {
  ULONG v = 0;
  ExAcquireFastMutex(&g_smnLock);
  HalSetBusDataByOffset(PCIConfiguration, 0, 0, &addr, 0x60, 4);
  HalGetBusDataByOffset(PCIConfiguration, 0, 0, &v, 0x64, 4);
  ExReleaseFastMutex(&g_smnLock);
  return v;
}

static void ReadIntel(WH_CPU_TEMPS* o) {
  ULONG n = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
  if (n > WH_MAX_CORES) n = WH_MAX_CORES;
  for (ULONG i = 0; i < n; i++) {
    PROCESSOR_NUMBER pn; GROUP_AFFINITY ga = {0}, old;
    if (!NT_SUCCESS(KeGetProcessorNumberFromIndex(i, &pn))) continue;
    ga.Group = pn.Group; ga.Mask = (KAFFINITY)1 << pn.Number;
    KeSetSystemGroupAffinityThread(&ga, &old);
    __try {
      ULONG tj = (ULONG)((__readmsr(MSR_TEMP_TARGET) >> 16) & 0xFF);
      if (tj < 70 || tj > 130) tj = 100;
      ULONG64 st = __readmsr(MSR_THERM_STATUS);
      if (st & (1ull << 31)) o->core[i] = (LONG)(tj - ((st >> 16) & 0x7F)) * 10;
      if (i == 0) {
        ULONG64 pk = __readmsr(MSR_PKG_THERM_STATUS);
        o->package = (LONG)(tj - ((pk >> 16) & 0x7F)) * 10;
        o->tjMax = tj;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    KeRevertToUserGroupAffinityThread(&old);
  }
  o->count = n;
}

static void ReadAmd(WH_CPU_TEMPS* o) {
  ULONG t = SmnRead(SMN_TCTL);
  LONG tc = (LONG)((t >> 21) & 0x7FF) * 125 / 100;        // 0.125 °C -> десятые доли
  if (t & (1u << 19)) tc -= 490;                          // диапазон -49 °C
  o->package = tc;
  ULONG n = 0;
  for (ULONG c = 0; c < 8 && n < WH_MAX_CORES; c++) {     // CCD (Zen2+); на одном CCD будет 1 значение
    ULONG v = SmnRead(SMN_CCD_BASE + c * 4);
    if (!(v & 0x800)) continue;                           // бит valid
    o->core[n++] = (LONG)(v & 0x7FF) * 125 / 100 - 490;
  }
  o->count = n;
  o->perCcd = 1;
}

static NTSTATUS Dispatch(PDEVICE_OBJECT d, PIRP irp) {
  UNREFERENCED_PARAMETER(d);
  PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
  NTSTATUS st = STATUS_SUCCESS; ULONG_PTR info = 0;
  if (s->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
    if (s->Parameters.DeviceIoControl.IoControlCode != IOCTL_WH_READ_TEMPS ||
        s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(WH_CPU_TEMPS)) {
      st = STATUS_INVALID_PARAMETER;
    } else {
      WH_CPU_TEMPS* o = (WH_CPU_TEMPS*)irp->AssociatedIrp.SystemBuffer;
      RtlZeroMemory(o, sizeof(*o));
      o->version = WH_TEMPS_VERSION; o->vendor = g_vendor; o->package = WH_NO_TEMP;
      for (int i = 0; i < WH_MAX_CORES; i++) o->core[i] = WH_NO_TEMP;
      if (g_vendor == 1) ReadIntel(o); else if (g_vendor == 2) ReadAmd(o);
      info = sizeof(*o);
    }
  }
  irp->IoStatus.Status = st; irp->IoStatus.Information = info;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return st;
}

static void Unload(PDRIVER_OBJECT drv) {
  UNICODE_STRING l = RTL_CONSTANT_STRING(L"\\DosDevices\\WhCpuTemp");
  IoDeleteSymbolicLink(&l);
  if (drv->DeviceObject) IoDeleteDevice(drv->DeviceObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg) {
  UNREFERENCED_PARAMETER(reg);
  UNICODE_STRING n = RTL_CONSTANT_STRING(L"\\Device\\WhCpuTemp");
  UNICODE_STRING l = RTL_CONSTANT_STRING(L"\\DosDevices\\WhCpuTemp");
  PDEVICE_OBJECT dev;
  g_vendor = CpuVendor();
  if (!g_vendor) return STATUS_NOT_SUPPORTED;
  ExInitializeFastMutex(&g_smnLock);
  NTSTATUS st = IoCreateDevice(drv, 0, &n, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
  if (!NT_SUCCESS(st)) return st;
  st = IoCreateSymbolicLink(&l, &n);
  if (!NT_SUCCESS(st)) { IoDeleteDevice(dev); return st; }
  drv->MajorFunction[IRP_MJ_CREATE] = drv->MajorFunction[IRP_MJ_CLOSE] =
  drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Dispatch;
  drv->DriverUnload = Unload;
  dev->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}
