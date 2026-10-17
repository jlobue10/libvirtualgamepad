// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include "windows.h"
struct fake_vhf;
using VHFHANDLE = fake_vhf *;
struct fake_operation { int completions {}; NTSTATUS status {STATUS_NOT_SUPPORTED}; PVOID state_lock {}; };
using VHFOPERATIONHANDLE = fake_operation *;
struct HID_XFER_PACKET { PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId; };
using PHID_XFER_PACKET = HID_XFER_PACKET *;
using EVT_VHF_ASYNC_OPERATION = void(PVOID, VHFOPERATIONHANDLE, PVOID, PHID_XFER_PACKET);
using EVT_VHF_READY_FOR_NEXT_READ_REPORT = void(PVOID);
using EVT_VHF_CLEANUP = void(PVOID);
struct VHF_CONFIG {
  HANDLE FileHandle {};
  USHORT ReportDescriptorLength {};
  PUCHAR ReportDescriptor {};
  PVOID VhfClientContext {};
  EVT_VHF_ASYNC_OPERATION *EvtVhfAsyncOperationWriteReport {};
  EVT_VHF_ASYNC_OPERATION *EvtVhfAsyncOperationGetFeature {};
  EVT_VHF_ASYNC_OPERATION *EvtVhfAsyncOperationSetFeature {};
  EVT_VHF_ASYNC_OPERATION *EvtVhfAsyncOperationGetInputReport {};
  EVT_VHF_READY_FOR_NEXT_READ_REPORT *EvtVhfReadyForNextReadReport {};
  EVT_VHF_CLEANUP *EvtVhfCleanup {};
  GUID ContainerID {};
  USHORT InstanceIDLength {}, VendorID {}, ProductID {}, VersionNumber {}, HardwareIDsLength {};
  PWSTR InstanceID {}, HardwareIDs {};
};
inline void VHF_CONFIG_INIT(VHF_CONFIG *c, HANDLE file, USHORT length, PUCHAR descriptor) {
  *c = {}; c->FileHandle = file; c->ReportDescriptorLength = length; c->ReportDescriptor = descriptor;
}
NTSTATUS VhfCreate(VHF_CONFIG *, VHFHANDLE *);
NTSTATUS VhfStart(VHFHANDLE);
void VhfDelete(VHFHANDLE, BOOLEAN);
NTSTATUS VhfAsyncOperationComplete(VHFOPERATIONHANDLE, NTSTATUS);
NTSTATUS VhfReadReportSubmit(VHFHANDLE, PHID_XFER_PACKET);
