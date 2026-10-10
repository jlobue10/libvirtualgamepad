// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Creates slot 7 as a Steam Controller (2026) on the installed driver, checks what
// Steam would see on the vendor collection (identity, report sizes, the
// feature-report control channel, the state report, haptic output -> feedback),
// then releases it. With --hold [seconds] it keeps the controller alive and
// animates it so Steam can be opened next to it; Enter stops early.
// --circle N picks how the stick-circle steps of the hold are driven, to find
// what Steam's "move the stick in a full circle" step objects to in a stream:
//   0  perfect unit circle, 20 reports/s (passes)
//   1  the shape the controller sends over BLE: axes clipped at +/-32767, so the
//      magnitude reaches 1.17 on diagonals (fork.20 passed this through)
//   2  perfect unit circle at 60 reports/s (the BLE cadence)
//   3  as 2 with gyro/accel motion streaming alongside
//   4  as 2 with a 100 ms gap every second, as a Wi-Fi hiccup leaves (a jump)
//   5  everything: clipped shape, 60/s, motion, gaps
// --rate N sets the stick-phase report rate, --turn S the seconds per turn,
// --update N how many times a second the stick value may change,
// --scale S the magnification of the clipped shape (modes 1 and 5; default 1.2,
// the BLE stream measures 1.11..1.16 at the diagonals), --clip N the largest
// value an axis may carry (default 32767; a Moonlight stream stops at 32766), and
// --burst N sends the reports in back-to-back groups of N with the value
// changing once per group, as a stream does when Vibepollo submits the input
// state plus two motion states for every BLE packet (--rate 200 --burst 3 is
// 66 packets/s of three reports each, ~15 ms apart).
// With --monitor [seconds] it creates nothing: it opens the vendor collection of
// the virtual Steam Controller that already exists (the one Vibepollo made for a
// stream) and reads its state reports, printing once a second what Steam sees of
// the sticks (magnitude range, sector coverage, largest jump between reports),
// the grip and stick touch bits, and the report cadence.
// Never installs drivers or modifies existing controllers.
#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros would break std::min/std::max below (MSVC C2589)
#endif
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <timeapi.h>
#include <algorithm>
#include <array>
#include <cmath>
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

// Every present vendor collection (usage ff00/0001) of VID 28DE PID 1302.
std::vector<found_collection> present_vendor_collections(const GUID &guid) {
  std::vector<found_collection> out;
  for (const auto &c : new_collections(guid, {})) {
    if (c.attributes.VendorID == 0x28de && c.attributes.ProductID == 0x1302 && c.usage_page == 0xff00 && c.usage == 0x0001) out.push_back(c);
  }
  return out;
}

struct stick_stats {
  float mag_min = 9, mag_max = 0;        // over samples away from centre (> 0.5)
  float axis_x = 0, axis_y = 0;          // per-axis peak |x|, |y|
  unsigned sectors = 0;                  // 16 angular sectors visited (> 0.5)
  float sector_peak[16] {};
  float max_jump_deg = 0;                // largest angle change between consecutive rim samples (> 0.8)
  bool have_prev = false; double prev_angle = 0;
  void add(short x, short y) {
    const float fx = x / 32767.f, fy = y / 32767.f;
    const float m = std::hypot(fx, fy);
    if (m <= 0.5f) { have_prev = false; return; }
    mag_min = std::min(mag_min, m); mag_max = std::max(mag_max, m);
    axis_x = std::max(axis_x, std::fabs(fx)); axis_y = std::max(axis_y, std::fabs(fy));
    const double a = std::atan2(fy, fx);
    int sec = static_cast<int>(std::floor((a + 3.14159265358979) / (2 * 3.14159265358979) * 16));
    sec = std::clamp(sec, 0, 15);
    sectors |= 1u << sec; sector_peak[sec] = std::max(sector_peak[sec], m);
    if (m > 0.8f) {
      if (have_prev) {
        double d = std::fabs(a - prev_angle) * 180.0 / 3.14159265358979;
        if (d > 180) d = 360 - d;
        max_jump_deg = std::max(max_jump_deg, static_cast<float>(d));
      }
      have_prev = true; prev_angle = a;
    } else {
      have_prev = false;
    }
  }
  void print(const char *name) const {
    if (sectors == 0) { std::printf("%s: centred", name); return; }
    unsigned n = 0; for (unsigned i = 0; i < 16; ++i) n += (sectors >> i) & 1;
    std::printf("%s: mag %.2f..%.2f |x| %.2f |y| %.2f, %u/16 sectors, max jump %.0f deg, peaks", name, mag_min, mag_max, axis_x, axis_y, n, max_jump_deg);
    // Compass points as the peak of the two sectors around each (sector 0 starts at -180 deg = W; y is up-positive here, so sector 4 = S).
    static const char *const names[] = {"W", "SW", "S", "SE", "E", "NE", "N", "NW"};
    for (int i = 0; i < 8; ++i) std::printf(" %s %.2f", names[i], std::max(sector_peak[(2 * i + 15) % 16], sector_peak[(2 * i) % 16]));
  }
};

int monitor(int seconds) {
  GUID guid; HidD_GetHidGuid(&guid);
  const auto collections = present_vendor_collections(guid);
  if (collections.empty()) {
    std::printf("no virtual Steam Controller is present (VID 28DE PID 1302 vendor collection). Start the stream first.\n");
    return 1;
  }
  std::printf("%zu virtual Steam Controller(s) present; monitoring the first for %d s (Enter stops):\n", collections.size(), seconds);
  for (const auto &c : collections) std::printf("  %ls\n", c.path.c_str());
  HANDLE handle = CreateFileW(collections[0].path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    handle = CreateFileW(collections[0].path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  }
  if (handle == INVALID_HANDLE_VALUE) {
    std::printf("open failed: error %lu (another reader may hold it exclusively)\n", GetLastError());
    return 1;
  }
  std::printf("Each second: reports/s, largest gap between reports, sequence-byte gaps (count, reports lost), largest\n"
              "imu_timestamp step and backwards steps, reports whose stick value changed (the effective stick update\n"
              "rate), buttons, grip and stick touch bits seen (L/R), then left/right stick statistics over that second.\n"
              "A whole-run summary of the sticks follows at the end.\n"
              "Do not select text in this console while it runs: that pauses the process and shows up as gaps.\n");
  // The gap column is measured on arrival in this process; keep scheduling jitter out of it.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
  const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
  ULONGLONG next_print = GetTickCount64() + 1000;
  std::array<unsigned char, 64> report {};
  DWORD bytes = 0;
  unsigned count = 0, other = 0;
  double max_gap_ms = 0; LARGE_INTEGER freq, last {}; QueryPerformanceFrequency(&freq);
  unsigned grips = 0, stick_touch = 0;
  std::uint32_t last_buttons = 0;
  // Reports whose button word differs from the previous report's: a held button that flickers
  // (Steam's "hold B to exit" needs a steady hold) shows as changes while nothing is pressed.
  bool have_buttons = false; unsigned button_changes = 0;
  // The unit's sequence byte advances by exactly 1 per report and its imu_timestamp by ~4 ms;
  // a virtual device that drops or reorders reports shows here.
  int last_seq = -1; unsigned seq_gaps = 0, seq_lost = 0, seq_gaps_all = 0, seq_lost_all = 0;
  bool have_ts = false; std::uint32_t last_ts = 0; double ts_max_ms = 0; unsigned ts_back = 0;
  // Reports whose stick value differs from the previous report's: the effective stick update
  // rate, as opposed to the report rate padded by keep-alives and motion resends.
  bool have_sticks = false; short prev_lx = 0, prev_ly = 0, prev_rx = 0, prev_ry = 0; unsigned left_updates = 0, right_updates = 0;
  stick_stats left, right, left_all, right_all;
  while (GetTickCount64() < end) {
    if (WaitForSingleObject(stdin_handle, 0) == WAIT_OBJECT_0) {
      INPUT_RECORD rec; DWORD n = 0;
      if (PeekConsoleInputW(stdin_handle, &rec, 1, &n) && n && rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown &&
          rec.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) break;
      ReadConsoleInputW(stdin_handle, &rec, 1, &n);
    }
    if (read_report(handle, report, bytes, 200) && bytes >= 18 && report[0] == 0x42) {
      LARGE_INTEGER now; QueryPerformanceCounter(&now);
      if (last.QuadPart) max_gap_ms = std::max(max_gap_ms, (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart);
      last = now;
      ++count;
      const int seq = report[1];
      if (last_seq >= 0) {
        const unsigned step = static_cast<unsigned>((seq - last_seq) & 0xff);
        if (step != 1) { ++seq_gaps; ++seq_gaps_all; seq_lost += step ? step - 1 : 255; seq_lost_all += step ? step - 1 : 255; }
      }
      last_seq = seq;
      if (bytes >= 34) {
        const std::uint32_t ts = ule32(report.data() + 30);
        if (have_ts) {
          const std::uint32_t d = ts - last_ts;  // wraps correctly for a 32-bit microsecond clock
          if (d > 0x80000000u) ++ts_back; else ts_max_ms = std::max(ts_max_ms, d / 1000.0);
        }
        last_ts = ts; have_ts = true;
      }
      const std::uint32_t buttons = ule32(report.data() + 2);
      if (have_buttons && buttons != last_buttons) ++button_changes;
      last_buttons = buttons; have_buttons = true;
      if (buttons & sc::btn_left_grip_touch) grips |= 1;
      if (buttons & sc::btn_right_grip_touch) grips |= 2;
      if (buttons & sc::btn_left_stick_touch) stick_touch |= 1;
      if (buttons & sc::btn_right_stick_touch) stick_touch |= 2;
      const short lx = static_cast<short>(report[10] | (report[11] << 8)), ly = static_cast<short>(report[12] | (report[13] << 8));
      const short rx = static_cast<short>(report[14] | (report[15] << 8)), ry = static_cast<short>(report[16] | (report[17] << 8));
      if (have_sticks) {
        if (lx != prev_lx || ly != prev_ly) ++left_updates;
        if (rx != prev_rx || ry != prev_ry) ++right_updates;
      }
      prev_lx = lx; prev_ly = ly; prev_rx = rx; prev_ry = ry; have_sticks = true;
      left.add(lx, ly); right.add(rx, ry); left_all.add(lx, ly); right_all.add(rx, ry);
    } else if (bytes) {
      ++other;
    }
    if (GetTickCount64() >= next_print) {
      next_print += 1000;
      std::printf("[%3llus] %3u rep/s gap %5.1f ms seq gaps %u (%u lost) ts max %5.1f ms back %u stick upd L %u R %u buttons 0x%08x (%u changes) grips %s%s touch %s%s | ",
                  (GetTickCount64() - (end - static_cast<ULONGLONG>(seconds) * 1000)) / 1000, count, max_gap_ms, seq_gaps, seq_lost,
                  ts_max_ms, ts_back, left_updates, right_updates, last_buttons, button_changes,
                  (grips & 1) ? "L" : "-", (grips & 2) ? "R" : "-", (stick_touch & 1) ? "L" : "-", (stick_touch & 2) ? "R" : "-");
      left.print("left"); std::printf(" | "); right.print("right"); std::printf("\n");
      count = other = 0; max_gap_ms = 0; seq_gaps = seq_lost = 0; ts_max_ms = 0; ts_back = 0; left_updates = right_updates = 0;
      grips = stick_touch = 0; button_changes = 0; left = stick_stats {}; right = stick_stats {};
    }
  }
  CloseHandle(handle);
  std::printf("whole run: sequence gaps %u (%u reports lost)\n           ", seq_gaps_all, seq_lost_all);
  left_all.print("left"); std::printf("\n           "); right_all.print("right"); std::printf("\n");
  return 0;
}

int main(int argc, char **argv) {
  constexpr unsigned slot = 7;
  int hold_seconds = 0, monitor_seconds = 0, circle_mode = 0, stick_rate = 0, stick_update_rate = 0, burst = 1;
  int stick_clip = 32767;
  double turn_seconds = 3.0, circle_scale = 1.2;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--hold") == 0) hold_seconds = (i + 1 < argc) ? std::atoi(argv[++i]) : 600;
    if (std::strcmp(argv[i], "--monitor") == 0) monitor_seconds = (i + 1 < argc) ? std::atoi(argv[++i]) : 120;
    if (std::strcmp(argv[i], "--circle") == 0) circle_mode = (i + 1 < argc) ? std::atoi(argv[++i]) : 0;
    if (std::strcmp(argv[i], "--rate") == 0) stick_rate = (i + 1 < argc) ? std::atoi(argv[++i]) : 0;
    if (std::strcmp(argv[i], "--turn") == 0) turn_seconds = (i + 1 < argc) ? std::atof(argv[++i]) : 3.0;
    if (std::strcmp(argv[i], "--update") == 0) stick_update_rate = (i + 1 < argc) ? std::atoi(argv[++i]) : 0;
    if (std::strcmp(argv[i], "--burst") == 0) burst = (i + 1 < argc) ? std::atoi(argv[++i]) : 3;
    if (std::strcmp(argv[i], "--scale") == 0) circle_scale = (i + 1 < argc) ? std::atof(argv[++i]) : 1.2;
    if (std::strcmp(argv[i], "--clip") == 0) stick_clip = (i + 1 < argc) ? std::atoi(argv[++i]) : 32766;
  }
  circle_mode = std::clamp(circle_mode, 0, 5);
  // Seconds per stick turn (the wired unit's owner took ~0.9 s per turn in the Steam capture) and
  // how many times per second the stick VALUE may change while reports keep flowing at --rate
  // (a stream changes it ~66 times per second inside ~250 reports). 0 = every report.
  turn_seconds = std::clamp(turn_seconds, 0.5, 10.0);
  if (stick_update_rate != 0) stick_update_rate = std::clamp(stick_update_rate, 5, 250);
  burst = std::clamp(burst, 1, 13);
  // Stick-circle submit rate in reports per second: 0 = the mode's default (20 for modes 0 and 1,
  // 60 for the fast modes), otherwise 20..250 in steps of 20 (the real unit streams at 250).
  if (stick_rate != 0) stick_rate = std::clamp(stick_rate, 20, 250);
  // --scale S: magnification of the clipped shape (modes 1 and 5) before the per-axis clip. 1.2 is the
  // wired unit's shape; the BLE stream measures 1.11..1.16 at the diagonals, so 1.1..1.15 stages it.
  circle_scale = std::clamp(circle_scale, 1.0, 2.0);
  // --clip N: the largest magnitude a stick axis may carry. Moonlight scales the client's
  // stick by 0x7FFE, so a stream never reaches +/-32767; --clip 32766 stages that.
  stick_clip = std::clamp(stick_clip, 16384, 32767);
  // Sleep() granularity is 15.6 ms by default, which makes a 16 ms sleep last up to 31 ms; the
  // hold's cadences depend on 4..50 ms sleeps being honoured, so ask for 1 ms timer resolution.
  timeBeginPeriod(1);
  if (monitor_seconds > 0) return monitor(monitor_seconds);
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

    // Haptic pulse as Steam's UI sends it -> a steam_haptic feedback event carrying the
    // verbatim 0x81 report (drivers before a4a12fa decoded it to generic_rumble instead;
    // both are accepted so the probe still runs against an older package).
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
      if (client.poll_feedback(slot, &event) != ERROR_SUCCESS) {
        Sleep(50);
        continue;
      }
      if (event.type == lvg::feedback_type::steam_haptic) {
        lvg::steam_haptic_feedback haptic {};
        std::memcpy(&haptic, event.payload, sizeof(haptic));
        std::printf("  feedback: steam_haptic length=%u report=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    haptic.length, haptic.report[0], haptic.report[1], haptic.report[2], haptic.report[3],
                    haptic.report[4], haptic.report[5], haptic.report[6], haptic.report[7]);
        got_feedback = event.payload_size == sizeof(haptic) && haptic.length == 8 &&
                       haptic.report[0] == sc::haptic_pulse_id && haptic.report[1] == 1 &&
                       haptic.report[2] == 0x90 && haptic.report[3] == 0x01 && haptic.report[6] == 1;
      } else if (event.type == lvg::feedback_type::generic_rumble) {
        lvg::generic_rumble_rgb_feedback rumble {};
        std::memcpy(&rumble, event.payload, sizeof(rumble));
        std::printf("  feedback: generic_rumble low=%u high=%u (pre-a4a12fa driver)\n", rumble.low_frequency, rumble.high_frequency);
        got_feedback = rumble.low_frequency == 65535;
      }
    }
    check(got_feedback, "pulse came back as the verbatim 0x81 steam_haptic event (left, 400 us, repeat 1)");
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
      std::printf("Steam's controller test runs its steps in order: left trigger (full pull), right trigger, finger across the\n"
                  "whole left pad, whole right pad, left stick in circles (several turns), right stick, every remaining button\n"
                  "(stick and pad clicks included), then A when the left haptic buzzes and A when the right one buzzes.\n"
                  "The hold drives exactly that and repeats; A is pressed automatically when a haptic pulse arrives. The\n"
                  "grip touch flags toggle every 2 s throughout, so the grips should light up blue on Steam's screen.\n"
                  "Feedback events are printed as they arrive. Stick circle mode %d (--circle 0..5), stick submit rate\n"
                  "%s (--rate 20..250 per second overrides the mode's 20 or 60), %.1f s per turn (--turn), stick value\n"
                  "changes %s per second (--update).\n", circle_mode,
                  stick_rate ? std::to_string(stick_rate).c_str() : "mode default", turn_seconds,
                  stick_update_rate ? std::to_string(stick_update_rate).c_str() : "with every report");
      const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(hold_seconds) * 1000;
      HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
      unsigned tick = 0;
      int last_phase = -1;
      unsigned haptic_ack_ticks = 0;   // ticks left of the automatic A press after a haptic pulse
      bool pad_down[2] = {false, false};
      const auto send_touch = [&](std::uint8_t pad, lvg::touch_event type, unsigned x, unsigned y) {
        lvg::touch_state_request touch {};
        touch.header.size = sizeof(touch); touch.header.version = lvg::k_protocol_version;
        touch.controller_id = slot; touch.contact_index = pad; touch.event_type = static_cast<std::uint8_t>(type);
        touch.x = static_cast<std::uint16_t>(x > 65535 ? 65535 : x); touch.y = static_cast<std::uint16_t>(y > 65535 ? 65535 : y);
        touch.pressure = 32767;
        (void) client.submit_touch_state(touch);
        pad_down[pad & 1] = type != lvg::touch_event::up;
      };
      // Whole-surface sweep of a pad over `steps` ticks: a serpentine raster of `rows` rows (one finger pass per
      // row, alternating direction), then a trace round the edges (left, top, right, bottom) and a diagonal.
      const auto pad_sweep = [](unsigned i, unsigned steps, unsigned &x, unsigned &y) {
        constexpr unsigned rows = 10;
        const unsigned raster = steps * 3 / 4, per_row = raster / rows, k = i % steps;
        if (k < per_row * rows) {
          const unsigned row = k / per_row, t = (k % per_row) * 65535u / (per_row > 1 ? per_row - 1 : 1);
          x = (row % 2) ? 65535u - t : t;
          y = row * 65535u / (rows - 1);
          return;
        }
        const unsigned e = k - per_row * rows, edge = steps - per_row * rows, leg = edge / 5 ? edge / 5 : 1;
        const unsigned seg = e / leg, t = (e % leg) * 65535u / (leg > 1 ? leg - 1 : 1);
        switch (seg) {
          case 0: x = 0; y = t; break;
          case 1: x = t; y = 65535; break;
          case 2: x = 65535; y = 65535 - t; break;
          case 3: x = 65535 - t; y = 0; break;
          default: x = t; y = t; break;
        }
      };
      // Step 7: every remaining button, held for 5 ticks (250 ms) with a 5 tick gap. The pad clicks need a
      // finger on the pad (the protocol has one click flag; the driver puts it on the touched pad).
      struct button_step { std::uint32_t mask; int touch_pad; const char *name; };
      static const button_step k_buttons[] = {
        {lvg::button_mask::south, -1, "A"}, {lvg::button_mask::east, -1, "B"},
        {lvg::button_mask::west, -1, "X"}, {lvg::button_mask::north, -1, "Y"},
        {lvg::button_mask::dpad_up, -1, "dpad up"}, {lvg::button_mask::dpad_down, -1, "dpad down"},
        {lvg::button_mask::dpad_left, -1, "dpad left"}, {lvg::button_mask::dpad_right, -1, "dpad right"},
        {lvg::button_mask::left_shoulder, -1, "L1"}, {lvg::button_mask::right_shoulder, -1, "R1"},
        {lvg::button_mask::left_stick, -1, "L3 (left stick click)"}, {lvg::button_mask::right_stick, -1, "R3 (right stick click)"},
        {lvg::button_mask::touchpad, 0, "left pad click"}, {lvg::button_mask::touchpad, 1, "right pad click"},
        {lvg::button_mask::start, -1, "View"}, {lvg::button_mask::back, -1, "Menu"},
        {lvg::button_mask::home, -1, "Steam"}, {lvg::button_mask::misc, -1, "QAM"},
        {lvg::button_mask::paddle_2, -1, "L4"}, {lvg::button_mask::paddle_4, -1, "L5"},
        {lvg::button_mask::paddle_1, -1, "R4"}, {lvg::button_mask::paddle_3, -1, "R5"},
      };
      constexpr unsigned k_button_count = sizeof(k_buttons) / sizeof(k_buttons[0]);
      static const char *const circle_names[] = {" (unit circle, 20/s)", " (BLE shape: axes clipped, 20/s)", " (unit circle, 60/s)",
                                                 " (unit circle, 60/s, motion streaming)", " (unit circle, 60/s, 100 ms gap each second)",
                                                 " (BLE shape, 60/s, motion, gaps)"};
      const bool circle_clipped = circle_mode == 1 || circle_mode == 5;
      const bool circle_fast = circle_mode >= 2;
      const bool circle_motion = circle_mode == 3 || circle_mode == 5;
      const bool circle_gaps = circle_mode == 4 || circle_mode == 5;
      // Reports per 50 ms tick during the stick phases and the sleep between them. The tick stays
      // 50 ms long whatever the rate, so a turn is always 3 s.
      const unsigned stick_sub = stick_rate ? std::max(1u, (static_cast<unsigned>(stick_rate) + 10) / 20) : (circle_fast ? 3 : 1);
      const DWORD sub_sleep_ms = stick_sub > 1 ? 50 / stick_sub : 50;
      const DWORD stick_tail_sleep_ms = stick_sub > 1 ? 50 - sub_sleep_ms * (stick_sub - 1) : 50;
      // Ticks per turn (20 ticks per second) and, in sub-steps, how often the stick value may change.
      const unsigned turn_ticks = std::max(10u, static_cast<unsigned>(std::lround(turn_seconds * 20.0)));
      const unsigned update_every = stick_update_rate ? std::max(1u, (stick_sub * 20 + stick_update_rate / 2) / static_cast<unsigned>(stick_update_rate)) : 1;
      short held_cx = 0, held_cy = 0;
      const auto send_motion = [&](unsigned k) {
        // Gyro: a slow wobble of a few deg/s; accel: 1 g on Z with a little noise. Units are milli
        // (deg/s, m/s^2) as Vibepollo sends them.
        lvg::motion_state_request m {};
        m.header.size = sizeof(m); m.header.version = lvg::k_protocol_version; m.controller_id = slot;
        m.motion_type = static_cast<std::uint8_t>(lvg::motion_kind::gyroscope);
        m.x_milli = static_cast<std::int32_t>(3000.0 * std::sin(k * 0.1)); m.y_milli = static_cast<std::int32_t>(2000.0 * std::cos(k * 0.13)); m.z_milli = 500;
        (void) client.submit_motion_state(m);
        m.motion_type = static_cast<std::uint8_t>(lvg::motion_kind::accelerometer);
        // SDL axes as Vibepollo sends them: Y is up, so a controller held flat has 1 g on Y
        // (the driver maps that to the unit's Z, where the wired capture shows 1 g at rest).
        m.x_milli = static_cast<std::int32_t>(200.0 * std::sin(k * 0.2)); m.y_milli = 9807; m.z_milli = 100;
        (void) client.submit_motion_state(m);
      };
      while (GetTickCount64() < end) {
        if (WaitForSingleObject(stdin_handle, 0) == WAIT_OBJECT_0) {
          INPUT_RECORD rec; DWORD n = 0;
          if (PeekConsoleInputW(stdin_handle, &rec, 1, &n) && n && rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown &&
              rec.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) break;
          ReadConsoleInputW(stdin_handle, &rec, 1, &n);
        }
        // 50 ms ticks. Phase lengths in ticks: triggers 60 each, pads 160 each, sticks 180 each (3 turns),
        // buttons 10 per button, then a 100 tick (5 s) quiet window for the haptic steps.
        constexpr unsigned k_trig = 60, k_pad = 160, k_stick = 180, k_btn = 10 * k_button_count, k_haptic = 100;
        constexpr unsigned k_cycle = 2 * k_trig + 2 * k_pad + 2 * k_stick + k_btn + k_haptic;
        const unsigned c = tick % k_cycle;
        int phase; unsigned i;
        if (c < k_trig) { phase = 0; i = c; }
        else if (c < 2 * k_trig) { phase = 1; i = c - k_trig; }
        else if (c < 2 * k_trig + k_pad) { phase = 2; i = c - 2 * k_trig; }
        else if (c < 2 * k_trig + 2 * k_pad) { phase = 3; i = c - 2 * k_trig - k_pad; }
        else if (c < 2 * k_trig + 2 * k_pad + k_stick) { phase = 4; i = c - 2 * k_trig - 2 * k_pad; }
        else if (c < 2 * k_trig + 2 * k_pad + 2 * k_stick) { phase = 5; i = c - 2 * k_trig - 2 * k_pad - k_stick; }
        else if (c < 2 * k_trig + 2 * k_pad + 2 * k_stick + k_btn) { phase = 6; i = c - 2 * k_trig - 2 * k_pad - 2 * k_stick; }
        else { phase = 7; i = c - 2 * k_trig - 2 * k_pad - 2 * k_stick - k_btn; }
        if (phase != last_phase) {
          static const char *const names[] = {"1. left trigger 0->255->0", "2. right trigger 0->255->0",
                                              "3. left pad whole-surface sweep", "4. right pad whole-surface sweep",
                                              "5. left stick circles", "6. right stick circles",
                                              "7. every remaining button", "8/9. waiting for haptic pulses (A auto-pressed)"};
          if (phase == 4 || phase == 5) std::printf("  phase: %s%s @ %u reports/s, %.1f s/turn, value every %u report(s), bursts of %d\n", names[phase], circle_names[circle_mode], stick_sub * 20, turn_seconds, update_every, burst);
          else std::printf("  phase: %s\n", names[phase]);
          for (int pad = 0; pad < 2; ++pad) {
            if (pad_down[pad]) send_touch(static_cast<std::uint8_t>(pad), lvg::touch_event::up, 0, 0);
          }
          last_phase = phase;
        }
        std::uint32_t buttons = 0;
        short lx = 0, ly = 0, rx = 0, ry = 0;
        unsigned char lt = 0, rt = 0;
        bool skip_send = false;
        // Steps 8/9: Steam buzzes one side and waits for A. Every haptic pulse becomes a feedback event here,
        // so answer each one with a 300 ms A press (also harmless during the earlier steps).
        if (haptic_ack_ticks > 0) {
          buttons |= lvg::button_mask::south;
          --haptic_ack_ticks;
        }
        // Grip sense: left 2 s, right 2 s, both 2 s, none 2 s.
        switch ((tick / 40) % 4) {
          case 0: buttons |= lvg::button_mask::left_grip_touch; break;
          case 1: buttons |= lvg::button_mask::right_grip_touch; break;
          case 2: buttons |= lvg::button_mask::left_grip_touch | lvg::button_mask::right_grip_touch; break;
          default: break;
        }
        // Trigger: 0..255..0 twice per phase. A hand holds the full pull for a good fraction of a
        // second, so the wave is a trapezoid: a third of the period ramping up, a third held at 255,
        // a third ramping down. A single 50 ms peak was lost at high report rates.
        const auto tri = [](unsigned k, unsigned period) {
          const unsigned third = period / 3 ? period / 3 : 1, m = k % period;
          if (m < third) return static_cast<unsigned char>(m * 255u / third);
          if (m < 2 * third) return static_cast<unsigned char>(255);
          const unsigned down = period - m;
          return static_cast<unsigned char>(down >= third ? 255 : down * 255u / third);
        };
        switch (phase) {
          case 0: lt = tri(i, k_trig / 2); break;
          case 1: rt = tri(i, k_trig / 2); break;
          case 2: case 3: {
            unsigned x = 0, y = 0;
            pad_sweep(i, k_pad, x, y);
            send_touch(static_cast<std::uint8_t>(phase - 2), i == 0 ? lvg::touch_event::down : lvg::touch_event::move, x, y);
            break;
          }
          case 4: case 5: {
            // One full turn every 60 ticks (3 s). Mode 0: the magnitude stays at 32767 all the way round.
            // Fast modes send three reports per tick (~60/s); the gap modes send nothing for two ticks
            // (100 ms) out of every twenty, so the position jumps when reports resume.
            if (circle_gaps && (i % 20) < 2) { skip_send = true; break; }
            const unsigned sub = stick_sub;
            for (unsigned k = 0; k < sub; ++k) {
              const double t = static_cast<double>(i % turn_ticks) + static_cast<double>(k) / sub;
              const double angle = t * (2.0 * 3.14159265358979 / turn_ticks);
              double fx = std::cos(angle), fy = std::sin(angle);
              if (circle_clipped) { fx = std::clamp(circle_scale * fx, -1.0, 1.0); fy = std::clamp(circle_scale * fy, -1.0, 1.0); }
              short cx = static_cast<short>(std::clamp<long>(std::lround(32767.0 * fx), -stick_clip, stick_clip));
              short cy = static_cast<short>(std::clamp<long>(std::lround(32767.0 * fy), -stick_clip, stick_clip));
              // --update: keep repeating the last value between value changes, as a stream does
              // when the device reports faster than the client samples the stick.
              // --burst: the value changes only with the first report of each group.
              const bool value_repeats = (update_every > 1 && ((i % turn_ticks) * sub + k) % update_every != 0)
                                         || (burst > 1 && k % static_cast<unsigned>(burst) != 0);
              if (value_repeats) { cx = held_cx; cy = held_cy; }
              else { held_cx = cx; held_cy = cy; }
              if (phase == 4) { lx = cx; ly = cy; } else { rx = cx; ry = cy; }
              if (circle_motion) send_motion(i * sub + k);
              if (k + 1 < sub) {
                // Intermediate report of a fast mode; the last one goes out with the common send below.
                (void) client.submit_input_state(make_input(slot, buttons, lx, ly, rx, ry, lt, rt));
                // --burst: no pause inside a group; the group's whole share of the tick after it.
                if (burst <= 1) Sleep(sub_sleep_ms);
                else if ((k + 1) % static_cast<unsigned>(burst) == 0) Sleep(sub_sleep_ms * static_cast<DWORD>(burst));
              }
            }
            break;
          }
          case 6: {
            const button_step &step = k_buttons[(i / 10) % k_button_count];
            const unsigned k = i % 10;
            if (k == 0) std::printf("    button: %s\n", step.name);
            if (step.touch_pad >= 0) {
              // Finger down one tick before the click and up one tick after it.
              if (k == 0) send_touch(static_cast<std::uint8_t>(step.touch_pad), lvg::touch_event::down, 32768, 32768);
              if (k == 6) send_touch(static_cast<std::uint8_t>(step.touch_pad), lvg::touch_event::up, 0, 0);
            }
            if (k >= 1 && k < 6) buttons = step.mask;
            break;
          }
          default: break;
        }
        if (!skip_send) (void) client.submit_input_state(make_input(slot, buttons, lx, ly, rx, ry, lt, rt));
        while (client.poll_feedback(slot, &event) == ERROR_SUCCESS) {
          const unsigned long long at_s = (GetTickCount64() - (end - static_cast<ULONGLONG>(hold_seconds) * 1000)) / 1000;
          bool active = false;
          if (event.type == lvg::feedback_type::steam_haptic) {
            // Verbatim haptic report: 0x80 with a non-zero speed, or 0x81 with a non-zero
            // on-time and repeat, means Steam is driving the motor; the rest are stops/commands.
            lvg::steam_haptic_feedback haptic {};
            std::memcpy(&haptic, event.payload, sizeof(haptic));
            const unsigned on_us = haptic.report[2] | (haptic.report[3] << 8);
            const unsigned repeat = haptic.report[6] | (haptic.report[7] << 8);
            const unsigned left_speed = haptic.report[4] | (haptic.report[5] << 8);
            const unsigned right_speed = haptic.report[7] | (haptic.report[8] << 8);
            active = (haptic.report[0] == sc::haptic_pulse_id && on_us != 0 && repeat != 0) ||
                     (haptic.report[0] == sc::haptic_rumble_id && (left_speed | right_speed) != 0);
            std::printf("  [%llus] feedback steam_haptic 0x%02x side=%u len=%u%s\n", at_s,
                        haptic.report[0], haptic.report[1], haptic.length, active ? "  -> pressing A" : "");
          } else {
            lvg::generic_rumble_rgb_feedback rumble {};
            std::memcpy(&rumble, event.payload, sizeof(rumble));
            active = (rumble.low_frequency | rumble.high_frequency) != 0;
            std::printf("  [%llus] feedback type %u low=%u high=%u%s\n", at_s,
                        static_cast<unsigned>(event.type), rumble.low_frequency, rumble.high_frequency,
                        active ? "  -> pressing A" : "");
          }
          if (active) haptic_ack_ticks = 6;
        }
        ++tick;
        Sleep(((phase == 4 || phase == 5) && !skip_send) ? stick_tail_sleep_ms : 50);
      }
      for (int pad = 0; pad < 2; ++pad) {
        if (pad_down[pad]) send_touch(static_cast<std::uint8_t>(pad), lvg::touch_event::up, 0, 0);
      }
      (void) client.submit_input_state(make_input(slot, 0));
      std::printf("hold finished\n");
    }
    CloseHandle(handle);
  }
  check(client.destroy_controller(slot) == ERROR_SUCCESS, "release test controller");
  {
    // A released slot is "not found", not "someone else's" (as destroy_controller
    // already answers), so a client holding a stale id is not sent after a
    // permission problem.
    lvg::feedback_event stale {};
    const DWORD polled = client.poll_feedback(slot, &stale);
    if (polled != ERROR_NOT_FOUND) {
      std::printf("poll_feedback on the released slot returned %lu
", static_cast<unsigned long>(polled));
    }
    check(polled == ERROR_NOT_FOUND, "poll_feedback on a released slot is ERROR_NOT_FOUND");
  }
  std::printf("%s\n", failures ? "PROBE FAILED" : "PROBE PASSED");
  timeEndPeriod(1);
  return failures ? 1 : 0;
}
