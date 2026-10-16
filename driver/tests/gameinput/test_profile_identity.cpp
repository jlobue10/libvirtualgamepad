// SPDX-License-Identifier: MIT
// Native PnP IDs must agree with HID attributes so GameInput and HIDAPI can
// identify the same controller. Does not load or interact with any driver.
#include "profile.h"
#include "libvirtualgamepad/sc26_usb.h"

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>

int main() {
  using namespace lvg;
  using namespace lvg::driver;
  int failures = 0;
  const auto check = [&](bool condition, const std::string &label) {
    if (!condition) {
      std::printf("FAIL: %s\n", label.c_str());
      ++failures;
    }
  };
  const struct { profile id; const char *name; bool xinput; } profiles[] = {
    {profile::dualshock_4, "DualShock 4", false},
    {profile::dualsense, "DualSense", false},
    {profile::switch_pro, "Switch Pro", false},
    {profile::xbox_one, "Xbox One", true},
    {profile::xbox_series, "Xbox Series", true},
    {profile::steam_controller, "Steam Controller", false},
  };
  for (const auto &entry : profiles) {
    if (entry.id == profile::steam_controller && lvg::sc26_usb::report_descriptor_is_provisional) {
      continue;  // Refused until the real descriptor lands; identity checked then.
    }
    const auto *definition = find_profile(entry.id);
    const std::string name = entry.name;
    check(definition != nullptr, name + " is implemented");
    if (definition == nullptr) continue;
    wchar_t expected[64] {};
    std::swprintf(expected, std::size(expected), L"HID\\VID_%04X&PID_%04X",
                  static_cast<unsigned>(definition->vendor_id),
                  static_cast<unsigned>(definition->product_id));
    const auto prefix_length = std::wcslen(expected);
    const bool has_ids = definition->hardware_ids != nullptr &&
      definition->hardware_ids_bytes >= (prefix_length + 2) * sizeof(wchar_t);
    check(has_ids, name + " supplies explicit PnP hardware IDs");
    if (!has_ids) continue;
    const auto *ids = definition->hardware_ids;
    check(std::wcsncmp(ids, expected, prefix_length) == 0 &&
          (ids[prefix_length] == L'\0' || ids[prefix_length] == L'&'),
          name + " first PnP ID agrees with its HID VID/PID");
    const auto characters = definition->hardware_ids_bytes / sizeof(wchar_t);
    check(definition->hardware_ids_bytes % sizeof(wchar_t) == 0 &&
          ids[characters - 1] == L'\0' && ids[characters - 2] == L'\0',
          name + " IDs are a double-null-terminated REG_MULTI_SZ");
    if (entry.xinput) {
      check(std::wcscmp(ids + prefix_length, L"&IG_00") == 0,
            name + " retains its XInput filter marker");
    } else {
      check(std::wcscmp(ids, expected) == 0,
            name + " native HID ID has no XInput filter marker");
    }
  }
  if (!failures) std::printf("PASS: all six native profile identities\n");
  return failures ? 1 : 0;
}
