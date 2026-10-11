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
