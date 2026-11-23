// Общий заголовок драйвера и мода. Значения — десятые доли °C.
#pragma once
#define WH_MAX_CORES 64
#define WH_NO_TEMP (-10000)
#define WH_TEMPS_VERSION 1
#define IOCTL_WH_READ_TEMPS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct {
  ULONG version, vendor;  // vendor: 1 Intel, 2 AMD
  ULONG count, perCcd;    // perCcd=1: core[] — температуры CCD (AMD), иначе логические ядра
  ULONG tjMax;
  LONG package;           // Package (Intel) / Tctl (AMD)
  LONG core[WH_MAX_CORES];
} WH_CPU_TEMPS;
