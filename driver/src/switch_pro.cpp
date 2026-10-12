// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "switch_pro.h"
#include "report_pump.h"

#include <cstring>

namespace lvg::driver {
namespace {

// The Pro Controller's USB report descriptor: a vendor collection carrying the
// console's own protocol. Everything interesting is vendor-defined, because the
// host drives this device by report id and byte offset rather than by HID
// usages.
constexpr std::uint8_t k_switch_descriptor[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Game Pad)
  0xA1, 0x01,        // Collection (Application)
  0x06, 0x01, 0xFF,  //   Usage Page (Vendor-defined 0xFF01)
  0x85, 0x21,        //   Report ID (33) - subcommand reply
  0x09, 0x21,        //   Usage (0x21)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x3F,        //   Report Count (63)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x85, 0x30,        //   Report ID (48) - full controller state
  0x09, 0x30,        //   Usage (0x30)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x3F,        //   Report Count (63)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x85, 0x81,        //   Report ID (129) - USB handshake reply
  0x09, 0x81,        //   Usage (0x81)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x3F,        //   Report Count (63)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x85, 0x01,        //   Report ID (1) - rumble and subcommand
  0x09, 0x01,        //   Usage (0x01)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x3F,        //   Report Count (63)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0x85, 0x10,        //   Report ID (16) - rumble only
  0x09, 0x10,        //   Usage (0x10)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x3F,        //   Report Count (63)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0x85, 0x80,        //   Report ID (128) - USB handshake command
  0x09, 0x80,        //   Usage (0x80)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x3F,        //   Report Count (63)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0xC0,              // End Collection
};

// A locally administered address, so it cannot collide with a real Nintendo
// device. Reported most-significant byte first, as the console does.
constexpr std::uint8_t k_switch_mac[6] = {0x02, 0x56, 0x47, 0x53, 0x57, 0x50};

[[nodiscard]] std::uint16_t clamp_stick(const std::int32_t value) noexcept {
  if (value < 0) {
    return 0;
  }
  return value > k_switch_stick_max ? k_switch_stick_max : static_cast<std::uint16_t>(value);
}

// Packs two 12-bit axes into the console's three-byte layout.
void pack_stick(std::uint8_t *const out, const std::uint16_t horizontal, const std::uint16_t vertical) noexcept {
  out[0] = static_cast<std::uint8_t>(horizontal & 0xFFu);
  out[1] = static_cast<std::uint8_t>(((horizontal >> 8) & 0x0Fu) | ((vertical & 0x0Fu) << 4));
  out[2] = static_cast<std::uint8_t>((vertical >> 4) & 0xFFu);
}

// Signed normalized input onto the 12-bit range around centre. The vertical
// axes are deliberately not inverted: this controller already reports
// positive-up, which is the protocol's own convention.
[[nodiscard]] std::uint16_t to_stick(const std::int16_t value) noexcept {
  const std::int32_t scaled =
    k_switch_stick_center + (static_cast<std::int32_t>(value) * k_switch_stick_range) / 32767;
  return clamp_stick(scaled);
}

[[nodiscard]] std::int16_t clamp_i16(const std::int32_t value) noexcept {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return static_cast<std::int16_t>(value);
}

// Writes a 12-bit pair into a factory calibration block.
void pack_calibration_pair(std::uint8_t *const out, const std::uint16_t first, const std::uint16_t second) noexcept {
  out[0] = static_cast<std::uint8_t>(first & 0xFFu);
  out[1] = static_cast<std::uint8_t>(((first >> 8) & 0x0Fu) | ((second & 0x0Fu) << 4));
  out[2] = static_cast<std::uint8_t>((second >> 4) & 0xFFu);
}

// Serves the emulated SPI flash a host reads calibration from. Anything not
// modelled reads as erased flash, which is what a host treats as "unset" and
// falls back from, rather than as fabricated data it would trust.
std::size_t read_spi(const std::uint32_t address, const std::uint8_t length, std::uint8_t *const out) noexcept {
  std::memset(out, 0xFF, length);

  switch (address) {
    case 0x603D: {
      // Factory stick calibration. Left is max-above, centre, min-below; right
      // is centre, min-below, max-above. Getting that order wrong is a stick
      // that reads offset rather than one that fails outright.
      if (length < 18) {
        return length;
      }
      pack_calibration_pair(out + 0, k_switch_stick_range, k_switch_stick_range);
      pack_calibration_pair(out + 3, k_switch_stick_center, k_switch_stick_center);
      pack_calibration_pair(out + 6, k_switch_stick_range, k_switch_stick_range);
      pack_calibration_pair(out + 9, k_switch_stick_center, k_switch_stick_center);
      pack_calibration_pair(out + 12, k_switch_stick_range, k_switch_stick_range);
      pack_calibration_pair(out + 15, k_switch_stick_range, k_switch_stick_range);
      return length;
    }
    case 0x6020: {
      // Factory motion calibration: zero offsets with the console's nominal
      // sensitivity coefficients.
      if (length < 24) {
        return length;
      }
      std::memset(out, 0, length);
      const std::uint16_t accel_scale = 16384;
      const std::uint16_t gyro_scale = 13371;
      for (int axis = 0; axis < 3; ++axis) {
        out[6 + axis * 2] = static_cast<std::uint8_t>(accel_scale & 0xFFu);
        out[7 + axis * 2] = static_cast<std::uint8_t>(accel_scale >> 8);
        out[18 + axis * 2] = static_cast<std::uint8_t>(gyro_scale & 0xFFu);
        out[19 + axis * 2] = static_cast<std::uint8_t>(gyro_scale >> 8);
      }
      return length;
    }
    case 0x6050: {
      // Body and button colours.
      if (length < 6) {
        return length;
      }
      const std::uint8_t colours[6] = {0x32, 0x32, 0x32, 0xE6, 0xE6, 0xE6};
      std::memcpy(out, colours, sizeof(colours));
      return length;
    }
    default:
      // 0x8010 user stick calibration and 0x8026 user motion calibration land
      // here: erased flash means "no user calibration", and the host uses the
      // factory blocks above.
      return length;
  }
}

}  // namespace

void switch_state::reset() noexcept {
  timer = 0;
  battery_level = 8;  // Full; the console reports this in even steps 0..8.
  cable_connected = true;
  for (int axis = 0; axis < 3; ++axis) {
    gyro[axis] = 0;
    accel[axis] = 0;
  }
  accel[2] = 4096;  // Roughly one gravity on Z at rest.
  player_lights = 0x01;
  imu_enabled = false;
  vibration_enabled = false;
  handshake_complete = false;
}

const std::uint8_t *switch_descriptor(std::size_t *const size) noexcept {
  if (size != nullptr) {
    *size = sizeof(k_switch_descriptor);
  }
  return k_switch_descriptor;
}

switch_input_report encode_switch_input(
  const input_state_request &input,
  switch_state *const state) noexcept {
  switch_input_report report {};
  report.report_id = k_switch_input_report_id;

  if (state != nullptr) {
    state->timer = static_cast<std::uint8_t>(state->timer + 1);
    report.timer = state->timer;
    // High nibble battery, low nibble connection: 0 means a wired Pro
    // Controller rather than a Joy-Con on a rail.
    report.connection_battery =
      static_cast<std::uint8_t>((state->battery_level & 0x0Eu) << 4 | (state->cable_connected ? 0x01u : 0x00u));
    report.vibrator_report = 0x0C;
  } else {
    report.connection_battery = 0x81;
    report.vibrator_report = 0x0C;
  }

  // Face buttons are positional. Vibeshine's south is physically where
  // Nintendo prints B, east is A, west is Y, north is X.
  std::uint8_t right = 0;
  if (input.buttons & button_mask::south) {
    right |= switch_b;
  }
  if (input.buttons & button_mask::east) {
    right |= switch_a;
  }
  if (input.buttons & button_mask::west) {
    right |= switch_y;
  }
  if (input.buttons & button_mask::north) {
    right |= switch_x;
  }
  if (input.buttons & button_mask::right_shoulder) {
    right |= switch_r;
  }
  // This controller has no analog triggers; the shoulder buttons are digital.
  if (input.right_trigger > 0) {
    right |= switch_zr;
  }
  report.buttons_right = right;

  std::uint8_t shared = 0;
  if (input.buttons & button_mask::back) {
    shared |= switch_minus;
  }
  if (input.buttons & button_mask::start) {
    shared |= switch_plus;
  }
  if (input.buttons & button_mask::left_stick) {
    shared |= switch_left_stick;
  }
  if (input.buttons & button_mask::right_stick) {
    shared |= switch_right_stick;
  }
  if (input.buttons & button_mask::home) {
    shared |= switch_home;
  }
  if (input.buttons & (button_mask::misc | button_mask::touchpad)) {
    shared |= switch_capture;
  }
  report.buttons_shared = shared;

  std::uint8_t left = 0;
  if (input.buttons & button_mask::dpad_down) {
    left |= switch_down;
  }
  if (input.buttons & button_mask::dpad_up) {
    left |= switch_up;
  }
  if (input.buttons & button_mask::dpad_right) {
    left |= switch_right;
  }
  if (input.buttons & button_mask::dpad_left) {
    left |= switch_left;
  }
  if (input.buttons & button_mask::left_shoulder) {
    left |= switch_l;
  }
  if (input.left_trigger > 0) {
    left |= switch_zl;
  }
  report.buttons_left = left;

  pack_stick(report.left_stick, to_stick(input.left_x), to_stick(input.left_y));
  pack_stick(report.right_stick, to_stick(input.right_x), to_stick(input.right_y));

  if (state != nullptr && state->imu_enabled) {
    // The console sends three samples per report; repeating the newest is
    // honest here because the protocol delivers one sample at a time.
    for (int sample = 0; sample < 3; ++sample) {
      std::uint8_t *const slot = report.imu + sample * 12;
      for (int axis = 0; axis < 3; ++axis) {
        const std::int16_t accel = state->accel[axis];
        slot[axis * 2] = static_cast<std::uint8_t>(accel & 0xFF);
        slot[axis * 2 + 1] = static_cast<std::uint8_t>((accel >> 8) & 0xFF);
        const std::int16_t gyro = state->gyro[axis];
        slot[6 + axis * 2] = static_cast<std::uint8_t>(gyro & 0xFF);
        slot[7 + axis * 2] = static_cast<std::uint8_t>((gyro >> 8) & 0xFF);
      }
    }
  }

  return report;
}

bool apply_switch_motion(const motion_state_request &motion, switch_state *const state) noexcept {
  if (state == nullptr) {
    return false;
  }

  switch (static_cast<motion_kind>(motion.motion_type)) {
    case motion_kind::accelerometer: {
      // The console's accelerometer reads 4096 counts per gravity.
      const auto convert = [](const std::int32_t milli) {
        return clamp_i16((milli * 4096) / 9807);
      };
      state->accel[0] = convert(motion.x_milli);
      state->accel[1] = convert(motion.y_milli);
      state->accel[2] = convert(motion.z_milli);
      return true;
    }
    case motion_kind::gyroscope: {
      // And roughly 79 counts per degree per second.
      const auto convert = [](const std::int32_t milli) {
        return clamp_i16((milli * 79) / 1000);
      };
      state->gyro[0] = convert(motion.x_milli);
      state->gyro[1] = convert(motion.y_milli);
      state->gyro[2] = convert(motion.z_milli);
      return true;
    }
    default:
      return false;
  }
}

bool apply_switch_battery(const battery_state_request &battery, switch_state *const state) noexcept {
  if (state == nullptr) {
    return false;
  }

  const auto reported = static_cast<lvg::battery_state>(battery.flags);
  state->cable_connected =
    reported == lvg::battery_state::charging || reported == lvg::battery_state::full ||
    reported == lvg::battery_state::not_charging;

  if (battery.percent <= 100) {
    // Reported in even steps from 0 (empty) to 8 (full).
    const std::uint32_t level = (static_cast<std::uint32_t>(battery.percent) * 8u + 50u) / 100u;
    state->battery_level = static_cast<std::uint8_t>((level > 8u ? 8u : level) & 0x0Eu);
    if (battery.percent >= 88) {
      state->battery_level = 8;
    }
  }
  return true;
}

std::size_t handle_switch_usb_command(
  const std::uint8_t *const report,
  const std::size_t size,
  switch_state *const state,
  switch_usb_reply *const reply) noexcept {
  if (report == nullptr || reply == nullptr || size < 2 || report[0] != k_switch_usb_command_id) {
    return 0;
  }

  *reply = {};
  reply->report_id = k_switch_usb_reply_id;
  reply->subtype = report[1];

  switch (report[1]) {
    case k_switch_usb_request_status: {
      // Device type then the address, most-significant byte first.
      reply->data[0] = 0x00;
      reply->data[1] = 0x03;  // Pro Controller.
      std::memcpy(reply->data + 2, k_switch_mac, sizeof(k_switch_mac));
      return 2 + 2 + sizeof(k_switch_mac);
    }
    case k_switch_usb_handshake:
      if (state != nullptr) {
        state->handshake_complete = true;
      }
      return 2;
    case k_switch_usb_baudrate:
    case k_switch_usb_hid_only:
      return 2;
    case k_switch_usb_disable_timeout:
      // Acknowledged by silence on real hardware.
      return 0;
    default:
      return 0;
  }
}

std::size_t handle_switch_subcommand(
  const std::uint8_t *const report,
  const std::size_t size,
  const input_state_request &last_input,
  switch_state *const state,
  switch_subcommand_reply *const reply) noexcept {
  // A rumble+subcommand report is [id][packet counter][8 rumble bytes][subcommand][args...].
  if (report == nullptr || reply == nullptr || size < 11 ||
      report[0] != k_switch_rumble_subcommand_id) {
    return 0;
  }

  const std::uint8_t subcommand = report[10];
  const std::uint8_t *const args = report + 11;
  const std::size_t args_size = size - 11;

  // The reply carries a full controller state, so build one and overlay the
  // acknowledgement. A host parses this as an input report too.
  const switch_input_report input = encode_switch_input(last_input, state);
  *reply = {};
  reply->report_id = k_switch_subcommand_reply_id;
  reply->timer = input.timer;
  reply->connection_battery = input.connection_battery;
  reply->buttons_right = input.buttons_right;
  reply->buttons_shared = input.buttons_shared;
  reply->buttons_left = input.buttons_left;
  std::memcpy(reply->left_stick, input.left_stick, sizeof(reply->left_stick));
  std::memcpy(reply->right_stick, input.right_stick, sizeof(reply->right_stick));
  reply->vibrator_report = input.vibrator_report;
  reply->subcommand = subcommand;

  switch (subcommand) {
    case k_switch_sub_request_device_info: {
      reply->ack = 0x82;
      reply->data[0] = 0x04;  // Firmware major.
      reply->data[1] = 0x21;  // Firmware minor.
      reply->data[2] = 0x03;  // Pro Controller.
      reply->data[3] = 0x02;
      std::memcpy(reply->data + 4, k_switch_mac, sizeof(k_switch_mac));
      reply->data[10] = 0x01;
      reply->data[11] = 0x01;  // Colours live in SPI.
      return sizeof(switch_subcommand_reply);
    }
    case k_switch_sub_spi_read: {
      if (args_size < 5) {
        reply->ack = 0x80;
        return sizeof(switch_subcommand_reply);
      }
      const std::uint32_t address = static_cast<std::uint32_t>(args[0]) |
                                    (static_cast<std::uint32_t>(args[1]) << 8) |
                                    (static_cast<std::uint32_t>(args[2]) << 16) |
                                    (static_cast<std::uint32_t>(args[3]) << 24);
      std::uint8_t length = args[4];
      if (length > sizeof(reply->data) - 5) {
        length = static_cast<std::uint8_t>(sizeof(reply->data) - 5);
      }
      reply->ack = 0x90;
      std::memcpy(reply->data, args, 5);
      read_spi(address, length, reply->data + 5);
      return sizeof(switch_subcommand_reply);
    }
    case k_switch_sub_enable_imu:
      if (state != nullptr && args_size >= 1) {
        state->imu_enabled = args[0] != 0;
      }
      reply->ack = 0x80;
      return sizeof(switch_subcommand_reply);
    case k_switch_sub_enable_vibration:
      if (state != nullptr && args_size >= 1) {
        state->vibration_enabled = args[0] != 0;
      }
      reply->ack = 0x80;
      return sizeof(switch_subcommand_reply);
    case k_switch_sub_set_player_lights:
      if (state != nullptr && args_size >= 1) {
        state->player_lights = args[0];
      }
      reply->ack = 0x80;
      return sizeof(switch_subcommand_reply);
    case k_switch_sub_get_player_lights:
      reply->ack = 0xB0;
      reply->data[0] = state != nullptr ? state->player_lights : 0x01;
      return sizeof(switch_subcommand_reply);
    case k_switch_sub_set_input_mode:
    case k_switch_sub_shipment_state:
    case k_switch_sub_set_home_light:
      reply->ack = 0x80;
      return sizeof(switch_subcommand_reply);
    default:
      // Acknowledge unknown subcommands rather than stalling initialization on
      // one this profile does not model.
      reply->ack = 0x80;
      return sizeof(switch_subcommand_reply);
  }
}

bool queue_switch_subcommand_reply(
  const std::uint8_t *const report,
  const std::size_t size,
  const input_state_request &last_input,
  switch_state *const state,
  report_pump &pump) noexcept {
  switch_subcommand_reply reply {};
  if (handle_switch_subcommand(report, size, last_input, state, &reply) == 0 ||
      !pump.enqueue(&reply, sizeof(reply), reply.report_id, report_kind::priority)) {
    return false;
  }

  if (reply.subcommand == k_switch_sub_set_input_mode && size >= 12 &&
      report[11] == k_switch_input_report_id) {
    // Chromium waits for controller data after the mode acknowledgement.
    // Sending only the 0x21 reply leaves a quiet controller uninitialized.
    const switch_input_report input = encode_switch_input(last_input, state);
    return pump.enqueue(&input, sizeof(input), input.report_id, report_kind::continuous);
  }
  return true;
}

bool decode_switch_rumble(
  const std::uint8_t *const report,
  const std::size_t size,
  playstation_output_feedback *const feedback) noexcept {
  if (report == nullptr || feedback == nullptr || size < 10) {
    return false;
  }
  if (report[0] != k_switch_rumble_subcommand_id && report[0] != k_switch_rumble_only_id) {
    return false;
  }

  // Four bytes per side follow the packet counter. The console encodes
  // frequency and amplitude together; only the amplitude has meaning for a
  // client that just wants to know how hard to vibrate.
  const std::uint8_t *const left = report + 2;
  const std::uint8_t *const right = report + 6;

  const auto amplitude = [](const std::uint8_t *const side) -> std::uint16_t {
    const std::uint8_t high_index = static_cast<std::uint8_t>((side[1] & 0xFE) >> 1);
    const std::int32_t low_index =
      ((static_cast<std::int32_t>(side[3] & 0x7F) - 0x40) * 2) + ((side[2] & 0x80) >> 7);
    const std::int32_t index = high_index > low_index ? high_index : low_index;
    if (index <= 0) {
      return 0;
    }
    const std::int32_t clamped = index > k_switch_amplitude_max ? k_switch_amplitude_max : index;
    return static_cast<std::uint16_t>((clamped * 65535) / k_switch_amplitude_max);
  };

  *feedback = {};
  feedback->low_frequency = amplitude(left);
  feedback->high_frequency = amplitude(right);
  return true;
}

}  // namespace lvg::driver
