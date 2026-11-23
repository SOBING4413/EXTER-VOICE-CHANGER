#pragma once
#include "ntddk.h"
/* PortCls shim */
inline NTSTATUS PcDispatchIrp(PDEVICE_OBJECT, PIRP) { ++g_shim.pcDispatchCalls; return STATUS_SUCCESS; }
