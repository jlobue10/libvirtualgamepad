// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// The device-setup tool's install verdict (tools/device_setup/install_decision.h):
// Windows declining the staged package is only a success when the owned node
// already runs that package's DriverVer.
#include "../../tools/device_setup/install_decision.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
void check(const bool condition, const std::string &what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
using lvg::device_setup::install_outcome;
using lvg::device_setup::install_exit_code;
using lvg::device_setup::attestation_due;
}  // namespace

int main() {
  // Windows kept an already-selected package (updated=false), interface answers
  check(attestation_due(install_outcome {.ready = true}), "a declined update is attested before success is claimed");
  check(install_exit_code(install_outcome {.ready = true, .version_matches = true}) == 0,
        "an idempotent re-run of the same package exits 0");
  check(install_exit_code(install_outcome {.ready = true, .version_matches = false}) == 3,
        "a declined different package exits 3 (old DLL still live)");
  // A selected package needs a live reload of the owned node in this session
  check(!attestation_due(install_outcome {.ready = true, .updated = true}),
        "a selected package is not attested before the owned node reloaded");
  check(install_exit_code(install_outcome {.ready = true, .updated = true, .device_cycle_succeeded = true, .version_matches = true}) == 0,
        "selected + reloaded + matching version exits 0");
  check(install_exit_code(install_outcome {.ready = true, .updated = true, .device_cycle_succeeded = true, .version_matches = false}) == 3,
        "selected + reloaded + other version exits 3");
  check(install_exit_code(install_outcome {.ready = true, .updated = true, .device_cycle_succeeded = true, .device_cycle_reboot_required = true, .version_matches = true}) == 3,
        "a reload Windows deferred to a reboot is not a verified reload");
  // Reboot requests
  check(install_exit_code(install_outcome {.ready = true, .reboot_required = true}) == 3010,
        "a reboot request without a verified reload exits 3010");
  check(install_exit_code(install_outcome {.ready = true, .reboot_required = true, .device_cycle_succeeded = true, .version_matches = true}) == 0,
        "a verified live reload supersedes the reboot request");
  check(install_exit_code(install_outcome {.ready = false, .reboot_required = true}) == 3010,
        "not ready with a reboot request exits 3010");
  check(install_exit_code(install_outcome {.ready = false}) == 3, "not ready exits 3");

  // D20-03: the probe verdict. A client holding the interface is in use = ready;
  // the attestation still decides success rather than exit 0 on sight.
  using lvg::device_setup::classify_open;
  using lvg::device_setup::interface_ready;
  using lvg::device_setup::probe_verdict;
  using lvg::device_setup::k_win32_error_sharing_violation;
  using lvg::device_setup::k_win32_error_access_denied;
  check(classify_open(k_win32_error_sharing_violation) == probe_verdict::in_use, "a sharing violation means a client holds the interface");
  check(classify_open(k_win32_error_access_denied) == probe_verdict::denied, "access denied is reported as denied");
  check(classify_open(2) == probe_verdict::absent, "file not found is absent");

  // L44-02: the staged package is installed non-interactively; an untrusted publisher
  // is reported as such instead of blocking on a prompt nobody can see.
  using lvg::device_setup::classify_driver_update;
  using lvg::device_setup::driver_update_verdict;
  using lvg::device_setup::k_win32_error_authenticode_publisher_not_trusted;
  using lvg::device_setup::k_win32_error_authenticode_trust_not_established;
  using lvg::device_setup::k_win32_error_no_more_items;
  using lvg::device_setup::k_win32_error_signature_osattribute_mismatch;
  check(classify_driver_update(true, 0) == driver_update_verdict::installed, "a successful update selected the package");
  check(classify_driver_update(false, k_win32_error_no_more_items) == driver_update_verdict::declined,
        "Windows declining an equal/newer package is not a failure");
  check(classify_driver_update(false, k_win32_error_authenticode_trust_not_established) == driver_update_verdict::publisher_not_trusted &&
            classify_driver_update(false, k_win32_error_authenticode_publisher_not_trusted) == driver_update_verdict::publisher_not_trusted &&
            classify_driver_update(false, k_win32_error_signature_osattribute_mismatch) == driver_update_verdict::publisher_not_trusted,
        "an untrusted publisher is reported as such");
  check(classify_driver_update(false, k_win32_error_access_denied) == driver_update_verdict::failed, "other errors stay failures");
  check(interface_ready(probe_verdict::in_use) && interface_ready(probe_verdict::answered), "in-use and answered interfaces are ready");
  check(!interface_ready(probe_verdict::denied) && !interface_ready(probe_verdict::absent), "denied and absent interfaces are not ready");
  check(attestation_due(install_outcome {.ready = interface_ready(probe_verdict::in_use)}), "an in-use interface still requires the DriverVer attestation");
  check(install_exit_code(install_outcome {.ready = interface_ready(probe_verdict::in_use), .version_matches = false}) == 3,
        "an in-use interface running another DriverVer is not a success");
  if (failures == 0) {
    std::printf("install decision: all checks passed\n");
  }
  return failures == 0 ? 0 : 1;
}
