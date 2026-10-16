// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Exercise actual DS4/DS5 reports across multiple tracking-id wraparounds.
#include "dualshock4.h"
#include "dualsense.h"

#include <array>
#include <cstdio>
#include <string>

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string &message) {
  ++checks;
  failures += !ok;
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", message.c_str());
}

template<class State, class Apply, class Report>
void exercise(const char *profile, Apply apply, Report report) {
  using namespace lvg;
  for (std::uint8_t held = 0; held < 2; ++held) {
    State state {};
    state.reset();
    touch_state_request event {};
    event.header.size = sizeof(event);
    event.header.version = k_protocol_version;
    event.contact_index = held;
    event.event_type = static_cast<std::uint8_t>(touch_event::down);
    const std::string label = std::string(profile) + " held slot " + std::to_string(held) + ": ";
    check(apply(event, &state), label + "initial contact accepted");
    const auto held_id = report(state)[held];
    bool accepted = true, stable = true, active = true, balanced = true;
    int collisions = 0;
    event.contact_index = 1 - held;
    for (int tap = 0; tap < 400; ++tap) {
      event.event_type = static_cast<std::uint8_t>(touch_event::down);
      accepted &= apply(event, &state);
      const auto ids = report(state);
      stable &= ids[held] == held_id;
      active &= (ids[0] & 0x80) == 0 && (ids[1] & 0x80) == 0;
      collisions += ids[0] == ids[1];
      // Repeated DOWN and MOVE must retain the current contact's identity.
      accepted &= apply(event, &state);
      stable &= report(state) == ids;
      event.event_type = static_cast<std::uint8_t>(touch_event::move);
      event.x = static_cast<std::uint16_t>(tap * 100);
      accepted &= apply(event, &state);
      stable &= report(state) == ids;
      event.event_type = static_cast<std::uint8_t>(touch_event::up);
      accepted &= apply(event, &state);
      const auto released = report(state);
      balanced &= released[held] == held_id && (released[1 - held] & 0x80) != 0;
    }
    check(accepted && active, label + "all 400 new contacts and movements are accepted");
    check(collisions == 0, label + "active contacts remain distinct across wraps (collisions=" + std::to_string(collisions) + ")");
    check(stable, label + "held/repeated/moved contact identities remain stable");
    check(balanced, label + "each UP releases only its own contact");
    event.event_type = static_cast<std::uint8_t>(touch_event::cancel_all);
    check(apply(event, &state) && (report(state)[0] & 0x80) && (report(state)[1] & 0x80),
          label + "cancel-all releases both contacts");
  }
}
}  // namespace

int main() {
  using namespace lvg::driver;
  exercise<ds4_state>("DS4", apply_ds4_touch, [](ds4_state &state) {
    const auto report = encode_ds4_input({}, &state);
    return std::array<std::uint8_t, 2> {report.touch[0].points[0].tracking_id, report.touch[0].points[1].tracking_id};
  });
  exercise<ds5_state>("DS5", apply_ds5_touch, [](ds5_state &state) {
    const auto report = encode_ds5_input({}, &state);
    return std::array<std::uint8_t, 2> {report.touch[0].contact, report.touch[1].contact};
  });
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
