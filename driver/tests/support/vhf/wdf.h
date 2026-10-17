// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include "windows.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>

// A non-recursive real mutex: same-thread reentry fails immediately instead of
// hanging a test. Other threads block, permitting deterministic teardown tests.
struct fake_wdf_object {
  void *context {};
  fake_wdf_object *parent {};
  std::mutex mutex;
  std::atomic<int> waiters {};
  bool armed {};
};
using WDFOBJECT = fake_wdf_object *;
using WDFDEVICE = WDFOBJECT;
using WDFDRIVER = WDFOBJECT;
using WDFWAITLOCK = WDFOBJECT;
using WDFIOTARGET = WDFOBJECT;
using WDFFILEOBJECT = WDFOBJECT;
using WDFTIMER = WDFOBJECT;
using WDFREQUEST = WDFOBJECT;
using WDFQUEUE = WDFOBJECT;
using WDFCMRESLIST = WDFOBJECT;
using PWDFDEVICE_INIT = WDFOBJECT;
using EVT_WDF_DRIVER_DEVICE_ADD = NTSTATUS(WDFDRIVER, PWDFDEVICE_INIT);
using EVT_WDF_DEVICE_PREPARE_HARDWARE = NTSTATUS(WDFDEVICE, WDFCMRESLIST, WDFCMRESLIST);
using EVT_WDF_DEVICE_RELEASE_HARDWARE = NTSTATUS(WDFDEVICE, WDFCMRESLIST);
using EVT_WDF_OBJECT_CONTEXT_CLEANUP = void(WDFOBJECT);
using EVT_WDF_DEVICE_FILE_CREATE = void(WDFDEVICE, WDFREQUEST, WDFFILEOBJECT);
using EVT_WDF_FILE_CLOSE = void(WDFFILEOBJECT);
using EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL = void(WDFQUEUE, WDFREQUEST, size_t, size_t, ULONG);
using EVT_WDF_TIMER = void(WDFTIMER);
inline thread_local std::vector<WDFWAITLOCK> fake_held_locks;
inline bool fake_holds(WDFWAITLOCK lock) {
  return std::find(fake_held_locks.begin(), fake_held_locks.end(), lock) != fake_held_locks.end();
}
inline void fake_require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "WDF/VHF boundary violation: %s\n", message); std::abort(); }
}
inline NTSTATUS WdfWaitLockAcquire(WDFWAITLOCK lock, void *) {
  fake_require(lock && !fake_holds(lock), "invalid/recursive wait lock");
  ++lock->waiters;
  lock->mutex.lock();
  --lock->waiters;
  fake_held_locks.push_back(lock);
  return STATUS_SUCCESS;
}
inline void WdfWaitLockRelease(WDFWAITLOCK lock) {
  fake_require(fake_holds(lock), "release without ownership");
  fake_held_locks.erase(std::find(fake_held_locks.begin(), fake_held_locks.end(), lock));
  lock->mutex.unlock();
}
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type, name) inline type *name(WDFOBJECT o) { return static_cast<type *>(o->context); }
struct WDF_IO_TARGET_OPEN_PARAMS {};
inline void WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(WDF_IO_TARGET_OPEN_PARAMS *, void *) {}
inline NTSTATUS fake_open_status = STATUS_SUCCESS;
inline bool fake_missing_file = false;
inline int fake_opens {}, fake_closes {};
inline std::function<void()> fake_close_hook;
inline NTSTATUS WdfIoTargetOpen(WDFIOTARGET, WDF_IO_TARGET_OPEN_PARAMS *) { ++fake_opens; return fake_open_status; }
inline HANDLE WdfIoTargetWdmGetTargetFileHandle(WDFIOTARGET target) { return fake_missing_file ? nullptr : target; }
inline void WdfIoTargetClose(WDFIOTARGET) { ++fake_closes; if (fake_close_hook) fake_close_hook(); }
inline bool WdfTimerStart(WDFTIMER timer, LONG) { timer->armed = true; return true; }
inline bool WdfTimerStop(WDFTIMER timer, BOOLEAN) { timer->armed = false; return true; }
inline WDFOBJECT WdfTimerGetParentObject(WDFTIMER timer) { return timer->parent; }
#define WDF_REL_TIMEOUT_IN_MS(ms) (ms)
inline WDFDEVICE WdfFileObjectGetDevice(WDFFILEOBJECT o) { return o->parent; }
inline WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE o) { return o->parent; }
inline WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST o) { return o->parent; }

// Unexercised WDF entry scaffolding is compile-only and fails closed. These
// mocks do not claim to verify device creation, request marshalling, or IRQL.
inline NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST, size_t, PVOID *, size_t *) { return STATUS_NOT_SUPPORTED; }
inline NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST, size_t, PVOID *, size_t *) { return STATUS_NOT_SUPPORTED; }
inline void WdfRequestComplete(WDFREQUEST, NTSTATUS) {}
inline void WdfRequestCompleteWithInformation(WDFREQUEST, NTSTATUS, ULONG_PTR) {}
struct WDF_FILEOBJECT_CONFIG {};
inline void WDF_FILEOBJECT_CONFIG_INIT(WDF_FILEOBJECT_CONFIG *, EVT_WDF_DEVICE_FILE_CREATE *, EVT_WDF_FILE_CLOSE *, void *) {}
struct WDF_OBJECT_ATTRIBUTES {
  WDFOBJECT ParentObject {};
  EVT_WDF_OBJECT_CONTEXT_CLEANUP *EvtCleanupCallback {};
};
inline void WDF_OBJECT_ATTRIBUTES_INIT(WDF_OBJECT_ATTRIBUTES *a) { *a = {}; }
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(a, type) WDF_OBJECT_ATTRIBUTES_INIT(a)
struct WDF_PNPPOWER_EVENT_CALLBACKS {
  EVT_WDF_DEVICE_PREPARE_HARDWARE *EvtDevicePrepareHardware {};
  EVT_WDF_DEVICE_RELEASE_HARDWARE *EvtDeviceReleaseHardware {};
};
inline void WDF_PNPPOWER_EVENT_CALLBACKS_INIT(WDF_PNPPOWER_EVENT_CALLBACKS *p) { *p = {}; }
inline void WdfFdoInitSetFilter(PWDFDEVICE_INIT) {}
inline void WdfDeviceInitSetFileObjectConfig(PWDFDEVICE_INIT, WDF_FILEOBJECT_CONFIG *, WDF_OBJECT_ATTRIBUTES *) {}
inline void WdfDeviceInitSetPnpPowerEventCallbacks(PWDFDEVICE_INIT, WDF_PNPPOWER_EVENT_CALLBACKS *) {}
inline NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *, WDF_OBJECT_ATTRIBUTES *, WDFDEVICE *) { return STATUS_NOT_SUPPORTED; }
struct WDF_TIMER_CONFIG { bool AutomaticSerialization {}; };
inline void WDF_TIMER_CONFIG_INIT_PERIODIC(WDF_TIMER_CONFIG *, EVT_WDF_TIMER *, LONG) {}
inline NTSTATUS WdfTimerCreate(WDF_TIMER_CONFIG *, WDF_OBJECT_ATTRIBUTES *, WDFTIMER *) { return STATUS_NOT_SUPPORTED; }
inline NTSTATUS WdfWaitLockCreate(WDF_OBJECT_ATTRIBUTES *, WDFWAITLOCK *) { return STATUS_NOT_SUPPORTED; }
inline NTSTATUS WdfIoTargetCreate(WDFDEVICE, WDF_OBJECT_ATTRIBUTES *, WDFIOTARGET *) { return STATUS_NOT_SUPPORTED; }
inline NTSTATUS WdfDeviceCreateDeviceInterface(WDFDEVICE, const GUID *, void *) { return STATUS_NOT_SUPPORTED; }
struct WDF_IO_QUEUE_CONFIG {
  bool PowerManaged {};
  EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL *EvtIoDeviceControl {};
};
inline constexpr bool WdfIoQueueDispatchSequential = true, WdfTrue = true;
inline void WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(WDF_IO_QUEUE_CONFIG *, bool) {}
inline NTSTATUS WdfIoQueueCreate(WDFDEVICE, WDF_IO_QUEUE_CONFIG *, void *, void *) { return STATUS_NOT_SUPPORTED; }
struct WDF_DRIVER_CONFIG { ULONG DriverPoolTag {}; };
inline void WDF_DRIVER_CONFIG_INIT(WDF_DRIVER_CONFIG *, EVT_WDF_DRIVER_DEVICE_ADD *) {}
inline NTSTATUS WdfDriverCreate(PDRIVER_OBJECT, PUNICODE_STRING, void *, WDF_DRIVER_CONFIG *, void *) { return STATUS_NOT_SUPPORTED; }
#define WDF_NO_EVENT_CALLBACK nullptr
#define WDF_NO_OBJECT_ATTRIBUTES nullptr
#define WDF_NO_HANDLE nullptr
