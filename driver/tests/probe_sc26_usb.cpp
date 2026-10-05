// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Creates slot 7 as a Steam Controller (2026) on the installed driver, checks what
// Steam would see on the vendor collection (identity, report sizes, the
// feature-report control channel, the state report, haptic output -> feedback),
// then releases it. With --hold [seconds] it keeps the controller alive and
// animates it so Steam can be opened next to it; Enter stops early.
// Never installs drivers or modifies existing controllers.
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <set>
#include <string>
#include <vector>
#include "libvirtualgamepad/client.h"
#include "libvirtualgamepad/sc26_usb.h"

namespace sc = lvg::sc26_usb;

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

struct found_collection {
  std::wstring path;
  HIDD_ATTRIBUTES attributes {};
  USHORT usage_page = 0, usage = 0;
  HIDP_CAPS caps {};
};

// Every new HID path of our VID/PID, with its top-level usage. Windows splits
// the three collections of the real descriptor into three paths.
std::vector<found_collection> new_collections(const GUID &guid, const std::set<std::wstring> &before) {
  std::vector<found_collection> out;
  auto set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return out;
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
    std::wstring lower(detail->DevicePath);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (lower.find(L"vid_28de&pid_1302") == std::wstring::npos) continue;
    // Mouse/keyboard collections are owned by their class drivers: open without
    // read/write just to read attributes and capabilities.
    auto h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    found_collection c; c.path = detail->DevicePath; c.attributes.Size = sizeof(c.attributes);
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetAttributes(h, &c.attributes) && HidD_GetPreparsedData(h, &pp)) {
      if (HidP_GetCaps(pp, &c.caps) == HIDP_STATUS_SUCCESS) { c.usage_page = c.caps.UsagePage; c.usage = c.caps.Usage; out.push_back(c); }
      HidD_FreePreparsedData(pp);
    }
    CloseHandle(h);
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}

int failures = 0;
void check(bool ok, const char *label) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
  if (!ok) ++failures;
}
unsigned ule16(const unsigned char *p) { return p[0] | (p[1] << 8); }
unsigned ule32(const unsigned char *p) { return ule16(p) | (ule16(p + 2) << 16); }

lvg::input_state_request make_input(unsigned slot, std::uint32_t buttons, short lx = 0, short ly = 0, short rx = 0, short ry = 0,
                                    unsigned char lt = 0, unsigned char rt = 0) {
  lvg::input_state_request input {};
  input.header.size = sizeof(input); input.header.version = lvg::k_protocol_version;
  input.controller_id = slot; input.buttons = buttons;
  input.left_x = lx; input.left_y = ly; input.right_x = rx; input.right_y = ry;
  input.left_trigger = lt; input.right_trigger = rt;
  return input;
}

bool read_report(HANDLE handle, std::array<unsigned char, 64> &report, DWORD &bytes, DWORD timeout_ms) {
  OVERLAPPED operation {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bytes = 0;
  bool ok = false;
  bool started = ReadFile(handle, report.data(), static_cast<DWORD>(report.size()), nullptr, &operation) != FALSE;
  if (started || GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(operation.hEvent, timeout_ms) == WAIT_OBJECT_0) {
      ok = GetOverlappedResult(handle, &operation, &bytes, FALSE) != FALSE;
    } else {
      CancelIoEx(handle, &operation);
      GetOverlappedResult(handle, &operation, &bytes, TRUE);
    }
  }
  CloseHandle(operation.hEvent);
  return ok;
}

int main(int argc, char **argv) {
  constexpr unsigned slot = 7;
  int hold_seconds = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--hold") == 0) hold_seconds = (i + 1 < argc) ? std::atoi(argv[++i]) : 600;
  }
  GUID guid; HidD_GetHidGuid(&guid);
  const auto before = existing_paths(guid);
  lvg::client client;
  auto status = client.connect();
  if (status != ERROR_SUCCESS) { std::printf("connect to the VHF driver: error %lu\n", status); return 1; }
  std::printf("driver profile mask 0x%x, max controllers %u\n", client.available_profiles(), client.maximum_controllers());
  check((client.available_profiles() & lvg::profile_bit(lvg::profile::steam_controller)) != 0,
        "driver advertises profile::steam_controller (mask bit 0x100)");
  status = client.create_controller(slot, lvg::profile::steam_controller);
  check(status == ERROR_SUCCESS, "create Steam Controller slot 7");
  if (status != ERROR_SUCCESS) { std::printf("  error %lu\n", status); return 1; }

  std::vector<found_collection> collections;
  for (int retry = 0; retry < 100 && collections.size() < 3; ++retry) { Sleep(100); collections = new_collections(guid, before); }
  std::printf("new HID collections for VID 28DE PID 1302: %zu\n", collections.size());
  const found_collection *vendor = nullptr;
  bool mouse = false, keyboard = false;
  for (const auto &c : collections) {
    std::printf("  usage %04x/%04x version %04x input=%u output=%u feature=%u  %ls\n", c.usage_page, c.usage, c.attributes.VersionNumber,
                c.caps.InputReportByteLength, c.caps.OutputReportByteLength, c.caps.FeatureReportByteLength, c.path.c_str());
    if (c.usage_page == 0x0001 && c.usage == 0x0002) mouse = true;
    if (c.usage_page == 0x0001 && c.usage == 0x0006) keyboard = true;
    if (c.usage_page == 0xff00 && c.usage == 0x0001) vendor = &c;
  }
  check(mouse, "lizard-mode mouse collection enumerates (Generic Desktop / Mouse)");
  check(keyboard, "lizard-mode keyboard collection enumerates (Generic Desktop / Keyboard)");
  check(vendor != nullptr, "vendor collection FF00/0001 enumerates (what Steam opens)");

  HANDLE handle = INVALID_HANDLE_VALUE;
  if (vendor) {
    check(vendor->attributes.VendorID == sc::vendor_id && vendor->attributes.ProductID == sc::product_id, "VID 28DE PID 1302");
    check(vendor->attributes.VersionNumber == sc::version, "bcdDevice 0x0307");
    check(vendor->caps.InputReportByteLength == 54, "vendor collection input report length 54 (largest input: 0x42)");
    check(vendor->caps.OutputReportByteLength == 64, "vendor collection output report length 64 (0x87..0x89)");
    check(vendor->caps.FeatureReportByteLength == 64, "vendor collection feature report length 64");
    handle = CreateFileW(vendor->path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "vendor collection opens read/write like HIDAPI does");
  }

  if (handle != INVALID_HANDLE_VALUE) {
    std::array<unsigned char, 64> cmd {}, reply {};
    wchar_t serial[64] {};
    if (HidD_GetSerialNumberString(handle, serial, sizeof(serial))) std::printf("HID serial string: %ls\n", serial);
    else std::printf("HID serial string: (none, error %lu)\n", GetLastError());

    // GET_ATTRIBUTES_VALUES, exactly as Steam sends it: [01][83][00].
    cmd.fill(0); cmd[0] = 1; cmd[1] = sc::cmd_get_attributes_values;
    check(HidD_SetFeature(handle, cmd.data(), 64), "SetFeature GET_ATTRIBUTES_VALUES");
    reply.fill(0); reply[0] = 1;
    check(HidD_GetFeature(handle, reply.data(), 64), "GetFeature reads the reply");
    check(reply[1] == sc::cmd_get_attributes_values && reply[2] == 30, "attributes reply: type 0x83, 30 bytes");
    if (reply[2] == 30) {
      std::printf("  attributes:");
      for (unsigned i = 0; i < 30; i += 5) std::printf(" %u=0x%08x", reply[3 + i], ule32(reply.data() + 4 + i));
      std::printf("\n");
      check(reply[3] == sc::attr_product_id && ule32(reply.data() + 4) == 0x1302, "first attribute is product id 0x1302");
      check(reply[18] == sc::attr_firmware_build_time && ule32(reply.data() + 19) == sc::captured_firmware_build_time,
            "firmware build time is the captured unit's");
    }
    // Unit serial: [01][AE][15][01].
    cmd.fill(0); cmd[0] = 1; cmd[1] = sc::cmd_get_string_attribute; cmd[2] = 0x15; cmd[3] = sc::string_attr_unit_serial;
    check(HidD_SetFeature(handle, cmd.data(), 64) && HidD_GetFeature(handle, reply.data(), 64), "GET_STRING_ATTRIBUTE unit serial");
    check(reply[1] == sc::cmd_get_string_attribute && reply[2] == 20 && reply[3] == 1, "serial reply: 20 bytes, tag 1");
    std::printf("  unit serial: %.19s\n", reinterpret_cast<const char *>(reply.data() + 4));
    // Device info 0, as Steam asks: [01][F2][01][00].
    cmd.fill(0); cmd[0] = 1; cmd[1] = sc::cmd_get_device_info; cmd[2] = 1; cmd[3] = 0;
    check(HidD_SetFeature(handle, cmd.data(), 64) && HidD_GetFeature(handle, reply.data(), 64) && reply[2] == 41,
          "GET_DEVICE_INFO 0 answers 41 bytes");
    // Settings Steam writes on connect: lizard off, IMU on.
    cmd.fill(0); cmd[0] = 1; cmd[1] = sc::cmd_set_settings_values; cmd[2] = 6;
    cmd[3] = sc::setting_lizard_mode; cmd[6] = sc::setting_imu_mode; cmd[7] = 0x18;
    check(HidD_SetFeature(handle, cmd.data(), 64), "SET_SETTINGS_VALUES accepted");

    // State report after a button press.
    check(client.submit_input_state(make_input(slot, lvg::button_mask::south | lvg::button_mask::home, 1000, -2000, 0, 0, 255, 0)) == ERROR_SUCCESS,
          "submit A + Steam + full left trigger");
    bool got_state = false;
    std::array<unsigned char, 64> report {};
    DWORD bytes = 0;
    for (int attempt = 0; attempt < 10 && !got_state; ++attempt) {
      if (!read_report(handle, report, bytes, 1000)) break;
      if (bytes == 54 && report[0] == 0x42) {
        const unsigned buttons = ule32(report.data() + 2);
        got_state = (buttons & sc::btn_a) && (buttons & sc::btn_steam) && (buttons & sc::btn_left_trigger_click) &&
                    ule16(report.data() + 6) == 32767 && static_cast<short>(ule16(report.data() + 10)) == 1000;
      }
    }
    check(got_state, "state report 0x42 (54 bytes) carries A, Steam, trigger click, stick, trigger");
    if (bytes) std::printf("  last report: id 0x%02x %lu bytes, seq %u, buttons 0x%08x\n", report[0], bytes, report[1], ule32(report.data() + 2));

    // Haptic pulse as Steam's UI sends it -> generic_rumble feedback to the client.
    std::array<unsigned char, 64> out {};
    out[0] = sc::haptic_pulse_id; out[1] = 1; out[2] = 0x90; out[3] = 0x01; out[6] = 1;  // left, 400 us on, repeat 1
    DWORD written = 0;
    OVERLAPPED wop {}; wop.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool wrote = WriteFile(handle, out.data(), 64, nullptr, &wop) != FALSE || GetLastError() == ERROR_IO_PENDING;
    if (wrote) wrote = GetOverlappedResult(handle, &wop, &written, TRUE) != FALSE;
    CloseHandle(wop.hEvent);
    check(wrote, "output report 0x81 (pulse) accepted");
    lvg::feedback_event event {};
    bool got_feedback = false;
    for (int attempt = 0; attempt < 20 && !got_feedback; ++attempt) {
      if (client.poll_feedback(slot, &event) == ERROR_SUCCESS && event.type == lvg::feedback_type::generic_rumble) {
        lvg::generic_rumble_rgb_feedback rumble {};
        std::memcpy(&rumble, event.payload, sizeof(rumble));
        std::printf("  feedback: generic_rumble low=%u high=%u\n", rumble.low_frequency, rumble.high_frequency);
        got_feedback = rumble.low_frequency == 65535;
      } else Sleep(50);
    }
    check(got_feedback, "pulse became a generic_rumble feedback event (left = full)");
    out.fill(0); out[0] = sc::haptic_command_id; out[2] = 0x02; out[3] = 0xf2;  // 0x82 as captured
    wop = {}; wop.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wrote = WriteFile(handle, out.data(), 64, nullptr, &wop) != FALSE || GetLastError() == ERROR_IO_PENDING;
    if (wrote) wrote = GetOverlappedResult(handle, &wop, &written, TRUE) != FALSE;
    CloseHandle(wop.hEvent);
    check(wrote, "output report 0x82 (haptic command) accepted");

    // Battery report on a battery update.
    lvg::battery_state_request battery {};
    battery.header.size = sizeof(battery); battery.header.version = lvg::k_protocol_version;
    battery.controller_id = slot; battery.percent = 80; battery.flags = static_cast<std::uint8_t>(lvg::battery_state::charging);
    check(client.submit_battery_state(battery) == ERROR_SUCCESS, "submit battery 80 % charging");

    if (hold_seconds > 0) {
      std::printf("\nHolding the controller for up to %d s (Enter stops). Open Steam -> Settings -> Controller now.\n", hold_seconds);
      std::printf("A/B/X/Y cycle every second, sticks sweep, feedback events are printed as they arrive.\n");
      const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(hold_seconds) * 1000;
      HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
      unsigned tick = 0;
      while (GetTickCount64() < end) {
        if (WaitForSingleObject(stdin_handle, 0) == WAIT_OBJECT_0) {
          INPUT_RECORD rec; DWORD n = 0;
          if (PeekConsoleInputW(stdin_handle, &rec, 1, &n) && n && rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown &&
              rec.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) break;
          ReadConsoleInputW(stdin_handle, &rec, 1, &n);
        }
        const std::uint32_t face[] = {lvg::button_mask::south, lvg::button_mask::east, lvg::button_mask::west, lvg::button_mask::north, 0};
        const std::uint32_t buttons = ((tick / 4) % 2) ? face[(tick / 20) % 5] : 0;
        const short sweep = static_cast<short>(((tick % 80) - 40) * 800);
        (void) client.submit_input_state(make_input(slot, buttons, sweep, 0, 0, sweep, static_cast<unsigned char>((tick % 50) * 5), 0));
        if (client.poll_feedback(slot, &event) == ERROR_SUCCESS) {
          lvg::generic_rumble_rgb_feedback rumble {};
          std::memcpy(&rumble, event.payload, sizeof(rumble));
          std::printf("  [%llus] feedback type %u low=%u high=%u\n", (GetTickCount64() - (end - static_cast<ULONGLONG>(hold_seconds) * 1000)) / 1000,
                      static_cast<unsigned>(event.type), rumble.low_frequency, rumble.high_frequency);
        }
        ++tick;
        Sleep(50);
      }
      std::printf("hold finished\n");
    }
    CloseHandle(handle);
  }
  check(client.destroy_controller(slot) == ERROR_SUCCESS, "release test controller");
  std::printf("%s\n", failures ? "PROBE FAILED" : "PROBE PASSED");
  return failures ? 1 : 0;
}
