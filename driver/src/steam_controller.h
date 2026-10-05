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
  lvg::sc26_usb::rumble rumble;

  void reset() noexcept;
};

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
[[nodiscard]] bool apply_sc26_motion(const motion_state_request &motion, sc26_state *state) noexcept;
[[nodiscard]] bool apply_sc26_battery(const battery_state_request &battery, sc26_state *state) noexcept;

// Output reports 0x80 (rumble) and 0x81 (pulse) become a generic_rumble
// feedback event. Returns false for other reports.
[[nodiscard]] bool apply_sc26_output(
  const std::uint8_t *data,
  std::size_t size,
  std::uint32_t controller_id,
  sc26_state *state,
  feedback_event *event) noexcept;

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
