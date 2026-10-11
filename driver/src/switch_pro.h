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

// SDL and Steam encode a 0..65535 amplitude by picking the first row of a
// 101-entry table whose threshold is >= the request; the row number is the
// index carried in the report. The table is far from linear below row ~60
// (row 68 = 32861 = 50 %), so decoding the index linearly reported a 50 %
// request as 68 %. These are SDL's thresholds (SDL_hidapi_switch.c,
// EncodeRumbleHighAmplitude / EncodeRumbleLowAmplitude share the column).
inline constexpr std::uint16_t k_switch_amplitude_by_index[k_switch_amplitude_max + 1] = {
  0, 514, 775, 921, 1096, 1303, 1550, 1843, 2192, 2606,
  3100, 3686, 4383, 5213, 6199, 7372, 7698, 8039, 8395, 8767,
  9155, 9560, 9984, 10426, 10887, 11369, 11873, 12398, 12947, 13520,
  14119, 14744, 15067, 15397, 15734, 16079, 16431, 16790, 17158, 17534,
  17918, 18310, 18711, 19121, 19540, 19967, 20405, 20851, 21308, 21775,
  22251, 22739, 23236, 23745, 24265, 24797, 25340, 25894, 26462, 27041,
  27633, 28238, 28856, 29488, 30134, 30794, 31468, 32157, 32861, 33581,
  34316, 35068, 35836, 36620, 37422, 38242, 39079, 39935, 40809, 41703,
  42616, 43549, 44503, 45477, 46473, 47491, 48531, 49593, 50679, 51789,
  52923, 54082, 55266, 56476, 57713, 58977, 60268, 61588, 62936, 64315,
  65535
};

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
// Offsets as hid-nintendo reads the 0x30 report and the 0x21 subcommand reply.
static_assert(offsetof(switch_input_report, buttons_right) == 3);
static_assert(offsetof(switch_input_report, left_stick) == 6);
static_assert(offsetof(switch_input_report, right_stick) == 9);
static_assert(offsetof(switch_input_report, imu) == 13);
static_assert(offsetof(switch_subcommand_reply, ack) == 13);
static_assert(offsetof(switch_subcommand_reply, subcommand) == 14);
static_assert(offsetof(switch_subcommand_reply, data) == 15);
static_assert(sizeof(switch_usb_reply) == 64);

struct switch_state {
  std::uint8_t timer;
  std::uint8_t battery_level;   // 0..8, even values, high nibble of the status byte.
  bool cable_connected;
  bool charging;                // bit 4 of the status byte
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

// Decodes the four-byte-per-side rumble payload carried by 0x01 and 0x10.
[[nodiscard]] bool decode_switch_rumble(
  const std::uint8_t *report,
  std::size_t size,
  playstation_output_feedback *feedback) noexcept;

}  // namespace lvg::driver
