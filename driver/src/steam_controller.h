// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// Steam Controller (2026) profile. Steam talks to this controller as a HID
// device of its own: a vendor collection that streams the Triton state report,
// a 64-byte feature-report control channel for settings, attributes and the
// serial number, and output reports for the pad haptics. The portable contract
// and the pure encoders live in include/libvirtualgamepad/sc26_usb.h; this
// header adapts Vibeshine's protocol structs onto them.

#pragma once

#include <cstddef>
#include <cstdint>

#include "libvirtualgamepad/protocol.h"
#include "libvirtualgamepad/sc26_usb.h"

namespace lvg::driver {

inline constexpr std::uint8_t k_sc26_input_report_id = lvg::sc26_usb::input_report_id;
inline constexpr std::uint8_t k_sc26_battery_report_id = lvg::sc26_usb::battery_report_id;
inline constexpr std::uint8_t k_sc26_features_report_id = lvg::sc26_usb::features_report_id;
inline constexpr std::uint16_t k_sc26_vendor_id = lvg::sc26_usb::vendor_id;
inline constexpr std::uint16_t k_sc26_product_id = lvg::sc26_usb::product_id;
inline constexpr std::uint16_t k_sc26_version = lvg::sc26_usb::version;

using sc26_input_report = lvg::sc26_usb::input_report;
using sc26_battery_report = lvg::sc26_usb::battery_report;

struct sc26_state {
  lvg::sc26_usb::state device;
  lvg::sc26_usb::feature_state features;
  // Driver clock (microseconds) of the last motion sample; 0 = never.
  std::uint64_t last_motion_us {};
  // Driver clock of the last battery report (0x43) queued for VHF, by the
  // client's update or the keep-alive's periodic one; 0 = never.
  std::uint64_t last_battery_us {};
  // Driver clock of the last state report queued for VHF (written by the
  // driver's pump, not by sc26_tick); the keep-alive resends the state when
  // this goes stale. A motion sample or a host's input-report read must not
  // touch it: neither puts a report on the wire.
  std::uint64_t last_report_us {};
  // Set once the client has sent a grip-touch bit (button_mask::left/right_grip_touch):
  // from then on the grips follow the client. Until then they read released;
  // there is no motion-based guess (it held both grips for the whole session).
  bool grip_explicit {};
  // Same for the stick-touch bits (button_mask::left/right_stick_touch): the first one
  // seen turns the deflection heuristic off and the sticks' touch follows the client.
  bool stick_touch_explicit {};
  // Same for the physical trigger clicks (button_mask::sc26_buttons_explicit): once the host
  // has marked this slot's client authoritative, the analog end-stop heuristic stays off.
  bool trigger_click_explicit {};

  void reset() noexcept;
};

// Stamps the report's IMU clock (microseconds, truncated to 32 bits like the
// unit's) and clears the grips while no client has reported them. Call before
// encoding a report; it does not mark anything as sent.
void sc26_tick(sc26_state *state, std::uint64_t now_us) noexcept;

[[nodiscard]] const std::uint8_t *sc26_descriptor(std::size_t *size) noexcept;

// Translates Vibeshine's button mask to the controller's bits. Exposed so the
// tests can pin the mapping.
[[nodiscard]] std::uint32_t sc26_buttons(std::uint32_t buttons, const sc26_state &state) noexcept;

[[nodiscard]] sc26_input_report encode_sc26_input(
  const input_state_request &input,
  sc26_state *state) noexcept;

[[nodiscard]] sc26_battery_report encode_sc26_battery(const sc26_state &state) noexcept;

// contact_index 0 is the left pad, 1 the right pad; each pad is single-touch.
[[nodiscard]] bool apply_sc26_touch(const touch_state_request &touch, sc26_state *state) noexcept;
[[nodiscard]] bool apply_sc26_motion(const motion_state_request &motion, sc26_state *state,
                                     std::uint64_t now_us) noexcept;
[[nodiscard]] bool apply_sc26_battery(const battery_state_request &battery, sc26_state *state) noexcept;

// The real controller's firmware attributes (ioctl_set_steam_controller_attributes)
// replace the flagged fields of the feature state, so Steam's next
// GET_ATTRIBUTES_VALUES / GET_DEVICE_INFO / GET_STRING_ATTRIBUTE read them.
// sc26_usb::apply_attributes holds the rules: product id and connection
// interval never change, a serial or build id that is not non-empty printable
// ASCII keeps its current value. Returns the flags applied. Call under the
// slot's state lock.
std::uint32_t apply_sc26_attributes(
  const steam_controller_attributes_request &attributes,
  sc26_state *state) noexcept;

// Haptic output reports 0x80..0x85 become a steam_haptic feedback event that
// carries the report verbatim (the host renders or replays it). Returns false
// for anything else, including the 0x86..0x89 reports of unknown purpose.
[[nodiscard]] bool apply_sc26_output(
  const std::uint8_t *data,
  std::size_t size,
  std::uint32_t controller_id,
  sc26_state *state,
  feedback_event *event) noexcept;

// A control-channel SetFeature carrying SET_SETTINGS_VALUES (0x87) or
// LOAD_DEFAULT_SETTINGS (0x8E) becomes steam_settings feedback events of at
// most k_steam_settings_triples_max triples each, in the order written.
// `data` starts at the report id, as set_sc26_feature's buffer does. Returns
// the number of events written to `events`: 0 for any other command, an
// empty write, a malformed transfer, or a write that would not fit
// `capacity` (a settings write is forwarded whole or not at all).
[[nodiscard]] std::size_t apply_sc26_settings(
  const std::uint8_t *data,
  std::size_t size,
  std::uint32_t controller_id,
  feedback_event *events,
  std::size_t capacity) noexcept;

// Control channel. set returns false only for a malformed transfer; get
// returns the bytes written or 0.
[[nodiscard]] bool set_sc26_feature(
  std::uint8_t report_id,
  const std::uint8_t *buffer,
  std::size_t size,
  sc26_state *state) noexcept;

[[nodiscard]] std::size_t fill_sc26_feature(
  std::uint8_t report_id,
  std::uint8_t *buffer,
  std::size_t capacity,
  const sc26_state &state) noexcept;

}  // namespace lvg::driver
