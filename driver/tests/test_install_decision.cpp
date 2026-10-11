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
  if (failures == 0) {
    std::printf("install decision: all checks passed\n");
  }
  return failures == 0 ? 0 : 1;
}
