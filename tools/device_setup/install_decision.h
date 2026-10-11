// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// The install verdict of the device-setup tool as a pure function, so a CTest can
// pin it without Windows: the tool only reports success when the package it was
// asked to install is the one every owned root node runs.
#pragma once

namespace lvg::device_setup {

struct install_outcome {
  bool ready {};                        // the source-device interface answered
  bool updated {};                      // UpdateDriverForPlugAndPlayDevices selected the staged package
  bool reboot_required {};              // Windows asked for a reboot at any step
  bool device_cycle_succeeded {};       // the owned node reloaded in this session
  bool device_cycle_reboot_required {}; // ... but Windows deferred that reload to a reboot
  bool version_matches {};              // every owned node reports the INF's DriverVer
};

// A changed selection (or a requested reboot) means an open interface may still
// belong to the previous UMDF host; the owned node has to reload in this session.
[[nodiscard]] constexpr bool live_reload_required(const install_outcome &o) noexcept {
  return o.reboot_required || o.updated;
}

// When the attestation has to be taken: with a reload after a successful cycle,
// without one as soon as the interface answers. Windows declining the staged
// package (ERROR_NO_MORE_ITEMS) is indistinguishable from an idempotent re-run
// of the same package until the running DriverVer is compared.
[[nodiscard]] constexpr bool attestation_due(const install_outcome &o) noexcept {
  if (!o.ready) {
    return false;
  }
  return !live_reload_required(o) || (o.device_cycle_succeeded && !o.device_cycle_reboot_required);
}

[[nodiscard]] constexpr bool reload_verified(const install_outcome &o) noexcept {
  return attestation_due(o) && o.version_matches;
}

// What the control-interface probe found. The tool opens the interface
// exclusively (like the client), so a driver that is in use by Vibepollo
// answers CreateFile with ERROR_SHARING_VIOLATION: the interface exists and a
// client passed the handshake to hold it, which is "ready" for install/status
// purposes (the DriverVer attestation still decides success). Access denied
// (a non-interactive account) is reported as such instead of "not ready".
enum class probe_verdict {
  absent,
  denied,
  in_use,
  answered,
};
constexpr unsigned long k_win32_error_access_denied = 5;      // ERROR_ACCESS_DENIED
constexpr unsigned long k_win32_error_sharing_violation = 32;  // ERROR_SHARING_VIOLATION
[[nodiscard]] constexpr probe_verdict classify_open(const unsigned long open_error) noexcept {
  switch (open_error) {
    case k_win32_error_sharing_violation:
      return probe_verdict::in_use;
    case k_win32_error_access_denied:
      return probe_verdict::denied;
    default:
      return probe_verdict::absent;
  }
}
[[nodiscard]] constexpr bool interface_ready(const probe_verdict verdict) noexcept {
  return verdict == probe_verdict::answered || verdict == probe_verdict::in_use;
}
[[nodiscard]] constexpr const wchar_t *probe_verdict_name(const probe_verdict verdict) noexcept {
  switch (verdict) {
    case probe_verdict::answered:
      return L"answered";
    case probe_verdict::in_use:
      return L"in_use";
    case probe_verdict::denied:
      return L"denied";
    default:
      return L"absent";
  }
}

constexpr int k_exit_ok = 0;
constexpr int k_exit_not_ready = 3;
constexpr int k_exit_reboot = 3010;

[[nodiscard]] constexpr int install_exit_code(const install_outcome &o) noexcept {
  if (reload_verified(o)) {
    return k_exit_ok;  // a verified live reload supersedes a reboot request
  }
  if (o.reboot_required) {
    return k_exit_reboot;
  }
  return k_exit_not_ready;
}

}  // namespace lvg::device_setup
