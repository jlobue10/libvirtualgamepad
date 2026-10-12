// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// Nintendo Switch Pro Controller profile.
//
// This profile needs more than a descriptor. A host that recognizes the
// identity drives the controller through the console's USB protocol: it sends
// 0x80 handshake commands and 0x01 subcommands and waits for 0x81 and 0x21
// replies, and it reads stick and motion calibration out of emulated SPI flash
// before it will use the device. A device that only streams 0x30 input reports
// never finishes initialization, so the handshake is part of the profile rather
// than an extra.
//
// Two things differ from the other pads and are easy to get backwards:
//   * the vertical stick axes are already positive-up, the same as the
//     protocol's, so they are the one profile that must NOT invert them;
//   * the face buttons are positional, so Vibeshine's south/east/west/north map
//     to Nintendo's B/A/Y/X rather than to the same letters.

#pragma once

#include <cstddef>
#include <cstdint>

#include "libvirtualgamepad/protocol.h"

namespace lvg::driver {

class report_pump;

inline constexpr std::uint8_t k_switch_input_report_id = 0x30;      // Full input report.
inline constexpr std::uint8_t k_switch_subcommand_reply_id = 0x21;  // Subcommand acknowledgement.
inline constexpr std::uint8_t k_switch_usb_reply_id = 0x81;         // USB handshake reply.
inline constexpr std::uint8_t k_switch_rumble_subcommand_id = 0x01; // Rumble + subcommand.
inline constexpr std::uint8_t k_switch_rumble_only_id = 0x10;       // Rumble only.
inline constexpr std::uint8_t k_switch_usb_command_id = 0x80;       // USB handshake command.

inline constexpr std::uint16_t k_switch_vendor_id = 0x057E;
inline constexpr std::uint16_t k_switch_product_id = 0x2009;  // Pro Controller.
inline constexpr std::uint16_t k_switch_version = 0x0200;

// Sticks are 12-bit, centred at midscale.
inline constexpr std::uint16_t k_switch_stick_center = 2048;
inline constexpr std::uint16_t k_switch_stick_range = 1400;
inline constexpr std::uint16_t k_switch_stick_max = 4095;

// Rumble amplitude indices saturate around 0x64 in the console's encoding.
inline constexpr std::uint8_t k_switch_amplitude_max = 0x64;

// USB handshake subtypes.
inline constexpr std::uint8_t k_switch_usb_request_status = 0x01;
inline constexpr std::uint8_t k_switch_usb_handshake = 0x02;
inline constexpr std::uint8_t k_switch_usb_baudrate = 0x03;
inline constexpr std::uint8_t k_switch_usb_hid_only = 0x04;
inline constexpr std::uint8_t k_switch_usb_disable_timeout = 0x05;

// Subcommands a host issues during initialization.
inline constexpr std::uint8_t k_switch_sub_request_device_info = 0x02;
inline constexpr std::uint8_t k_switch_sub_set_input_mode = 0x03;
inline constexpr std::uint8_t k_switch_sub_shipment_state = 0x08;
inline constexpr std::uint8_t k_switch_sub_spi_read = 0x10;
inline constexpr std::uint8_t k_switch_sub_set_player_lights = 0x30;
inline constexpr std::uint8_t k_switch_sub_get_player_lights = 0x31;
inline constexpr std::uint8_t k_switch_sub_set_home_light = 0x38;
inline constexpr std::uint8_t k_switch_sub_enable_imu = 0x40;
inline constexpr std::uint8_t k_switch_sub_enable_vibration = 0x48;

// Button bits, as the console's three status bytes define them.
enum switch_button_right : std::uint8_t {
  switch_y = 0x01,
  switch_x = 0x02,
  switch_b = 0x04,
  switch_a = 0x08,
  switch_r = 0x40,
  switch_zr = 0x80,
};

enum switch_button_shared : std::uint8_t {
  switch_minus = 0x01,
  switch_plus = 0x02,
  switch_right_stick = 0x04,
  switch_left_stick = 0x08,
  switch_home = 0x10,
  switch_capture = 0x20,
};

enum switch_button_left : std::uint8_t {
  switch_down = 0x01,
  switch_up = 0x02,
  switch_right = 0x04,
  switch_left = 0x08,
  switch_l = 0x40,
  switch_zl = 0x80,
};

#pragma pack(push, 1)

struct switch_input_report {
  std::uint8_t report_id;
  std::uint8_t timer;
  // High nibble battery level, low nibble connection info.
  std::uint8_t connection_battery;
  std::uint8_t buttons_right;
  std::uint8_t buttons_shared;
  std::uint8_t buttons_left;
  // Two 12-bit axes packed across three bytes each.
  std::uint8_t left_stick[3];
  std::uint8_t right_stick[3];
  std::uint8_t vibrator_report;
  // Three 12-byte motion samples.
  std::uint8_t imu[36];
  std::uint8_t reserved[15];
};

// A subcommand reply is an input report with the controller state, then an
// acknowledgement byte, the subcommand it answers, and its payload.
struct switch_subcommand_reply {
  std::uint8_t report_id;
  std::uint8_t timer;
  std::uint8_t connection_battery;
  std::uint8_t buttons_right;
  std::uint8_t buttons_shared;
  std::uint8_t buttons_left;
  std::uint8_t left_stick[3];
  std::uint8_t right_stick[3];
  std::uint8_t vibrator_report;
  std::uint8_t ack;
  std::uint8_t subcommand;
  std::uint8_t data[49];
};

struct switch_usb_reply {
  std::uint8_t report_id;
  std::uint8_t subtype;
  std::uint8_t data[62];
};

#pragma pack(pop)

static_assert(sizeof(switch_input_report) == 64);
static_assert(sizeof(switch_subcommand_reply) == 64);
static_assert(sizeof(switch_usb_reply) == 64);

struct switch_state {
  std::uint8_t timer;
  std::uint8_t battery_level;   // 0..8, even values, high nibble of the status byte.
  bool cable_connected;
  std::int16_t gyro[3];
  std::int16_t accel[3];
  std::uint8_t player_lights;
  bool imu_enabled;
  bool vibration_enabled;
  // Set once the host has completed the USB handshake; a host that has not
  // finished it is still expecting replies rather than input.
  bool handshake_complete;

  void reset() noexcept;
};

[[nodiscard]] const std::uint8_t *switch_descriptor(std::size_t *size) noexcept;

[[nodiscard]] switch_input_report encode_switch_input(
  const input_state_request &input,
  switch_state *state) noexcept;

[[nodiscard]] bool apply_switch_motion(const motion_state_request &motion, switch_state *state) noexcept;
[[nodiscard]] bool apply_switch_battery(const battery_state_request &battery, switch_state *state) noexcept;

// Builds the reply to a 0x80 USB command. Returns 0 when the command needs no
// reply, otherwise the number of bytes written.
[[nodiscard]] std::size_t handle_switch_usb_command(
  const std::uint8_t *report,
  std::size_t size,
  switch_state *state,
  switch_usb_reply *reply) noexcept;

// Builds the reply to a 0x01 rumble+subcommand report, and decodes any rumble
// it carried. Returns 0 when no reply is owed.
[[nodiscard]] std::size_t handle_switch_subcommand(
  const std::uint8_t *report,
  std::size_t size,
  const input_state_request &last_input,
  switch_state *state,
  switch_subcommand_reply *reply) noexcept;

// Queues the acknowledgement and, when full input mode is requested, a fresh
// 0x30 report after it. Initialization must finish even if client input is idle.
// The caller serializes access to the state and pump.
[[nodiscard]] bool queue_switch_subcommand_reply(
  const std::uint8_t *report,
  std::size_t size,
  const input_state_request &last_input,
  switch_state *state,
  report_pump &pump) noexcept;

// Decodes the four-byte-per-side rumble payload carried by 0x01 and 0x10.
[[nodiscard]] bool decode_switch_rumble(
  const std::uint8_t *report,
  std::size_t size,
  playstation_output_feedback *feedback) noexcept;

}  // namespace lvg::driver
