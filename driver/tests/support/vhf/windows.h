// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
// Fixed Windows ABI types for portable driver-body tests. No OS implementation.
#include <cstddef>
#include <cstdint>
#include <cwchar>
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using LONG = std::int32_t;
using ULONG_PTR = std::uintptr_t;
using USHORT = std::uint16_t;
using UCHAR = std::uint8_t;
using PUCHAR = UCHAR *;
using PWSTR = wchar_t *;
using PVOID = void *;
using HANDLE = void *;
using BOOLEAN = bool;
using NTSTATUS = std::int32_t;
using PDRIVER_OBJECT = void *;
using PUNICODE_STRING = void *;
struct GUID {
  std::uint32_t Data1;
  std::uint16_t Data2, Data3;
  std::uint8_t Data4[8];
};
inline constexpr bool TRUE = true, FALSE = false;
inline constexpr NTSTATUS STATUS_SUCCESS = 0;
inline constexpr NTSTATUS STATUS_INVALID_PARAMETER = -1;
inline constexpr NTSTATUS STATUS_NOT_SUPPORTED = -2;
inline constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -3;
inline constexpr NTSTATUS STATUS_DEVICE_BUSY = -4;
inline constexpr NTSTATUS STATUS_CANCELLED = -5;
inline constexpr NTSTATUS STATUS_ACCESS_DENIED = -6;
inline constexpr NTSTATUS STATUS_NOT_FOUND = -7;
inline constexpr NTSTATUS STATUS_INVALID_DEVICE_REQUEST = -8;
inline constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = -9;
inline constexpr NTSTATUS STATUS_NO_MORE_ENTRIES = -10;
inline constexpr NTSTATUS STATUS_INVALID_BUFFER_SIZE = -11;
inline constexpr NTSTATUS STATUS_INVALID_HANDLE = -12;
inline constexpr bool NT_SUCCESS(NTSTATUS status) { return status >= 0; }
#define RTL_NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))

// User-mode timing and threading behind the Steam Controller keep-alive worker
// (start_sc26_keepalive and friends in driver.cpp). The lifecycle fixture never
// creates the worker's timer or stop event, so no worker runs here: handle
// creation and the waits fail closed, while the performance counter (the report
// clock) and SwitchToThread (the submit drain) do their real job.
#include <chrono>
#include <thread>
using BOOL = int;
using LONGLONG = std::int64_t;
using LPVOID = void *;
using LPCWSTR = const wchar_t *;
using LPSECURITY_ATTRIBUTES = void *;
using LPTHREAD_START_ROUTINE = DWORD (*)(LPVOID);
#define WINAPI
struct LARGE_INTEGER { LONGLONG QuadPart; };
inline constexpr DWORD INFINITE = 0xFFFFFFFFu;
inline constexpr DWORD WAIT_OBJECT_0 = 0;
inline constexpr DWORD WAIT_FAILED = 0xFFFFFFFFu;
inline constexpr DWORD TIMER_ALL_ACCESS = 0x1F0003u;
inline constexpr int THREAD_PRIORITY_HIGHEST = 2;
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency) {
  using period = std::chrono::steady_clock::period;
  frequency->QuadPart = static_cast<LONGLONG>(period::den / period::num);
  return 1;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER *counter) {
  counter->QuadPart = static_cast<LONGLONG>(std::chrono::steady_clock::now().time_since_epoch().count());
  return 1;
}
inline BOOL SwitchToThread() { std::this_thread::yield(); return 1; }
inline HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR) { return nullptr; }
inline HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) { return nullptr; }
inline BOOL SetWaitableTimer(HANDLE, const LARGE_INTEGER *, LONG, void *, LPVOID, BOOL) { return 0; }
inline BOOL CancelWaitableTimer(HANDLE) { return 0; }
inline BOOL SetEvent(HANDLE) { return 0; }
inline BOOL ResetEvent(HANDLE) { return 0; }
inline HANDLE CreateThread(LPSECURITY_ATTRIBUTES, std::size_t, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD *) { return nullptr; }
inline BOOL SetThreadPriority(HANDLE, int) { return 0; }
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return WAIT_FAILED; }
inline DWORD WaitForMultipleObjects(DWORD, const HANDLE *, BOOL, DWORD) { return WAIT_FAILED; }
inline BOOL CloseHandle(HANDLE) { return 1; }
