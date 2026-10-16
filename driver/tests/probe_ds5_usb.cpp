// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Creates slot 7, checks the installed driver's native DualSense initialization path,
// then releases it. Never installs drivers or modifies existing controllers.
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <cstdio>
#include <array>
#include <vector>
#include <set>
#include <string>
#include <algorithm>
#include <cwctype>
#include "libvirtualgamepad/client.h"

// Select only a newly created HID path, including when testing an old driver
// whose pairing reply is wrong. Never write reports to an existing controller.
std::set<std::wstring> existing_paths(const GUID &guid) {
  std::set<std::wstring> paths;
  auto set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return paths;
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA item {}; item.cbSize = sizeof(item);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &item)) break;
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &size, nullptr);
    if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
    std::vector<unsigned char> storage(size);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
    detail->cbSize = sizeof(*detail);
    if (SetupDiGetDeviceInterfaceDetailW(set, &item, detail, size, nullptr, nullptr)) paths.insert(detail->DevicePath);
  }
  SetupDiDestroyDeviceInfoList(set);
  return paths;
}

int main() {
  constexpr unsigned slot = 7;
  GUID guid; HidD_GetHidGuid(&guid);
  const auto before = existing_paths(guid);
  lvg::client client;
  auto status = client.connect();
  if (status == ERROR_SUCCESS) status = client.create_controller(slot, lvg::profile::dualsense);
  if (status != ERROR_SUCCESS) {
    std::printf("Create DualSense slot %u: error %lu\n", slot, status); return 1;
  }
  HANDLE handle = INVALID_HANDLE_VALUE;
  for (int retry = 0; retry < 50 && handle == INVALID_HANDLE_VALUE; ++retry) {
    auto set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) break;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA item {}; item.cbSize = sizeof(item);
      if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &item)) break;
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<unsigned char> storage(size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(set, &item, detail, size, nullptr, nullptr)) continue;
      if (before.contains(detail->DevicePath)) continue;
      std::wstring path(detail->DevicePath);
      std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
      // Explicit native hardware IDs move the VID/PID to the front of the path.
      // Keep accepting the generic VHF path for older installed drivers.
      if (path.find(L"hid_device_system_vhf") == std::wstring::npos &&
          path.find(L"vid_054c&pid_0ce6") == std::wstring::npos) continue;
      auto h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
      if (h == INVALID_HANDLE_VALUE) continue;
      HIDD_ATTRIBUTES attributes {}; attributes.Size = sizeof(attributes);
      std::array<unsigned char, 20> pairing {}; pairing[0] = 9;
      if (HidD_GetAttributes(h, &attributes) && attributes.VendorID == 0x054c &&
          attributes.ProductID == 0x0ce6 && HidD_GetFeature(h, pairing.data(), static_cast<ULONG>(pairing.size()))) {
        handle = h;
        std::printf("DualSense %04x:%04x version %04x\n", attributes.VendorID, attributes.ProductID, attributes.VersionNumber);
        break;
      }
      CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (handle == INVALID_HANDLE_VALUE) Sleep(100);
  }
  int failures = 0;
  const auto check = [&](bool ok, const char *label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
  };
  check(handle != INVALID_HANDLE_VALUE, "created VHF device enumerates on a new HID path");
  if (handle != INVALID_HANDLE_VALUE) {
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps {};
    const bool have_caps = HidD_GetPreparsedData(handle, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
    check(have_caps, "HID capabilities readable");
    if (have_caps) {
      std::printf("input=%u output=%u feature=%u\n", caps.InputReportByteLength, caps.OutputReportByteLength, caps.FeatureReportByteLength);
      check(caps.InputReportByteLength == 64 && caps.OutputReportByteLength == 48 && caps.FeatureReportByteLength == 64,
            "native USB DualSense report-size gate");
      std::vector<HIDP_VALUE_CAPS> values(caps.NumberFeatureValueCaps);
      auto count = caps.NumberFeatureValueCaps;
      bool firmware_usage = false, usb_marker = false, calibration_usage = false, pairing_usage = false, control_usage = false;
      if (HidP_GetValueCaps(HidP_Feature, values.data(), &count, pp) == HIDP_STATUS_SUCCESS) {
        for (const auto &value : values) {
          if (value.ReportID == 0x20 && value.UsagePage == 0xff00 &&
              value.NotRange.Usage == 0x26 && value.ReportCount == 63) firmware_usage = true;
          if (value.UsagePage == 0xff00) {
            if (value.ReportID == 0x85 && value.NotRange.Usage == 0x2d && value.ReportCount == 2) usb_marker = true;
            if (value.ReportID == 5 && value.NotRange.Usage == 0x33 && value.ReportCount == 40) calibration_usage = true;
            if (value.ReportID == 9 && value.NotRange.Usage == 0x24 && value.ReportCount == 19) pairing_usage = true;
            if (value.ReportID == 8 && value.NotRange.Usage == 0x34 && value.ReportCount == 47) control_usage = true;
          }
        }
      }
      check(usb_marker, "native USB transport marker 85/FF00/2D/2");
      check(calibration_usage, "modern USB calibration descriptor");
      check(pairing_usage, "USB pairing descriptor");
      check(control_usage, "USB sensor command descriptor");
      check(firmware_usage, "native USB DualSense feature-usage gate");
    }
    if (pp) HidD_FreePreparsedData(pp);
    std::array<unsigned char, 64> firmware {}; firmware[0] = 0x20;
    check(HidD_GetFeature(handle, firmware.data(), static_cast<ULONG>(firmware.size())) &&
          (firmware[28] | (firmware[29] << 8) | (firmware[30] << 16) | (firmware[31] << 24)) >= 0x1003e, "native USB DualSense firmware gate");
    check((firmware[44] | (firmware[45] << 8)) >= 0x0390, "libScePad DualSense update version gate");
    check((firmware[44] | (firmware[45] << 8)) >= 0x0630, "Steam DualSense update version gate");
    std::array<unsigned char, 48> command {}; command[0] = 8; command[1] = 2;
    check(HidD_SetFeature(handle, command.data(), static_cast<ULONG>(command.size())), "native USB sensor initialization");
    std::array<unsigned char, 41> calibration {}; calibration[0] = 5;
    check(HidD_GetFeature(handle, calibration.data(), static_cast<ULONG>(calibration.size())), "41-byte USB calibration read");
    const auto le16 = [](const unsigned char *p) { return static_cast<short>(p[0] | (p[1] << 8)); };
    const int speed = le16(calibration.data() + 19) + le16(calibration.data() + 21);
    const int range = le16(calibration.data() + 7) - le16(calibration.data() + 9);
    check(range > 0 && 16 * speed == range, "gyro calibration matches 16 counts per degree/s");
    std::array<unsigned char, 64> identity {}; identity.fill(0xcd); identity[0] = 9;
    check(HidD_GetFeature(handle, identity.data(), static_cast<ULONG>(identity.size())) &&
          identity[1] == slot && identity[2] == 0x53 && identity[6] == 2 &&
          identity[7] == 0x08 && identity[8] == 0x25 && identity[9] == 0x00,
          "unique slot pairing address");
    bool clean_tail = true;
    for (unsigned i = 20; i < identity.size(); ++i) clean_tail &= identity[i] == 0;
    check(clean_tail, "pairing reply clears reused feature buffer tail");
    lvg::input_state_request input {};
    input.header.size = sizeof(input); input.header.version = lvg::k_protocol_version;
    input.controller_id = slot; input.buttons = lvg::button_mask::south;
    check(client.submit_input_state(input) == ERROR_SUCCESS, "submit Cross button");
    OVERLAPPED operation {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool received_cross = false;
    for (unsigned attempt = 0; attempt < 16 && operation.hEvent; ++attempt) {
      std::array<unsigned char, 64> report {};
      ResetEvent(operation.hEvent);
      DWORD bytes = 0;
      bool started = ReadFile(handle, report.data(), static_cast<DWORD>(report.size()), nullptr, &operation) != FALSE;
      if (!started && GetLastError() != ERROR_IO_PENDING) break;
      if (WaitForSingleObject(operation.hEvent, 1000) != WAIT_OBJECT_0) {
        CancelIoEx(handle, &operation);
        GetOverlappedResult(handle, &operation, &bytes, TRUE);
        break;
      }
      if (!GetOverlappedResult(handle, &operation, &bytes, FALSE)) break;
      if (bytes == 64 && report[0] == 1 && (report[8] & 0x20)) { received_cross = true; break; }
    }
    check(received_cross, "native HID reader receives Cross button report");
    if (operation.hEvent) CloseHandle(operation.hEvent);
    CloseHandle(handle);
  }
  check(client.destroy_controller(slot) == ERROR_SUCCESS, "release test controller");
  return failures ? 1 : 0;
}
