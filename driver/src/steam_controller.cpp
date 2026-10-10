// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "steam_controller.h"

#include <cstring>

namespace lvg::driver {
namespace sc = lvg::sc26_usb;

// Every haptic report the driver forwards has to fit the feedback payload.
static_assert(sc::report_size(sc::haptic_rumble_id) <= sizeof(steam_haptic_feedback::report));
static_assert(sc::report_size(sc::haptic_pulse_id) <= sizeof(steam_haptic_feedback::report));
static_assert(sc::report_size(sc::haptic_command_id) <= sizeof(steam_haptic_feedback::report));
static_assert(sc::report_size(sc::haptic_lfo_id) <= sizeof(steam_haptic_feedback::report));
static_assert(sc::report_size(sc::haptic_sweep_id) <= sizeof(steam_haptic_feedback::report));
static_assert(sc::report_size(sc::haptic_script_id) <= sizeof(steam_haptic_feedback::report));
static_assert(sizeof(steam_haptic_feedback) <= sizeof(feedback_event::payload));

void sc26_state::reset() noexcept {
  device.reset();
  features.reset();
  last_motion_us = 0;
  last_battery_us = 0;
  last_report_us = 0;
  grip_explicit = false;
  stick_touch_explicit = false;
}

const std::uint8_t *sc26_descriptor(std::size_t *const size) noexcept {
  if (size != nullptr) {
    *size = sc::report_descriptor_size;
  }
  return sc::report_descriptor;
}

std::uint32_t sc26_buttons(const std::uint32_t buttons, const sc26_state &state) noexcept {
  struct pair { std::uint32_t from; std::uint32_t to; };
  // Paddle order follows the Moonlight Android Steam Controller driver, which
  // sends R4 as paddle 1, L4 as paddle 2, R5 as paddle 3 and L5 as paddle 4.
  constexpr pair k_map[] = {
    {button_mask::south, sc::btn_a},
    {button_mask::east, sc::btn_b},
    {button_mask::west, sc::btn_x},
    {button_mask::north, sc::btn_y},
    {button_mask::dpad_up, sc::btn_dpad_up},
    {button_mask::dpad_down, sc::btn_dpad_down},
    {button_mask::dpad_left, sc::btn_dpad_left},
    {button_mask::dpad_right, sc::btn_dpad_right},
    // Valve's constant names and the buttons' positions disagree: SDL's Triton
    // driver maps TRITON_LBUTTON_VIEW (0x40) to SDL_GAMEPAD_BUTTON_START and
    // TRITON_LBUTTON_MENU (0x4000) to SDL_GAMEPAD_BUTTON_BACK, and a Moonlight
    // client that reads the real controller sends 0x40 as its start button. The
    // virtual device has to put each press back on the bit the client read it
    // from, or Steam shows the two buttons swapped (observed 2026-10-07).
    {button_mask::start, sc::btn_view},
    {button_mask::back, sc::btn_menu},
    {button_mask::left_stick, sc::btn_l3},
    {button_mask::right_stick, sc::btn_r3},
    {button_mask::left_shoulder, sc::btn_l},
    {button_mask::right_shoulder, sc::btn_r},
    {button_mask::home, sc::btn_steam},
    {button_mask::misc, sc::btn_qam},
    {button_mask::paddle_1, sc::btn_r4},
    {button_mask::paddle_2, sc::btn_l4},
    {button_mask::paddle_3, sc::btn_r5},
    {button_mask::paddle_4, sc::btn_l5},
  };
  std::uint32_t out = 0;
  for (const pair &p : k_map) {
    if ((buttons & p.from) != 0) {
      out |= p.to;
    }
  }
  if ((buttons & button_mask::touchpad) != 0) {
    // The protocol has one pad-click flag; the controller has one per pad. A
    // click lands on the pad under a finger: the left pad when only it is
    // touched, the right pad when only it is, both when both are (the
    // protocol cannot tell which was pressed), and the right pad when neither
    // is, so a click without touch data still registers.
    const bool left = state.device.pad_touched[0];
    const bool right = state.device.pad_touched[1];
    if (left) {
      out |= sc::btn_left_pad_click;
    }
    if (right || !left) {
      out |= sc::btn_right_pad_click;
    }
  }
  return out;
}

// Stick touch from the client (LI_CCAP_STICK_TOUCH) is carried as two button bits.
// The first one seen turns the deflection heuristic off for good: from then on a
// stick reads touched exactly when the client says so, including a thumb resting
// on a centred stick, which the real unit reports and the heuristic cannot.
static void apply_client_stick_touch(const std::uint32_t buttons, sc26_state *const state) noexcept {
  constexpr std::uint32_t stick_touch_bits =
    button_mask::left_stick_touch | button_mask::right_stick_touch;
  if ((buttons & stick_touch_bits) != 0) {
    state->stick_touch_explicit = true;
    state->device.stick_touch_from_deflection = false;
  }
  if (state->stick_touch_explicit) {
    state->device.stick_touch[0] = (buttons & button_mask::left_stick_touch) != 0;
    state->device.stick_touch[1] = (buttons & button_mask::right_stick_touch) != 0;
  }
}

sc26_input_report encode_sc26_input(
  const input_state_request &input,
  sc26_state *const state) noexcept {
  if (state == nullptr) {
    sc26_state scratch {};
    scratch.reset();
    apply_client_stick_touch(input.buttons, &scratch);
    return sc::encode_input(sc26_buttons(input.buttons, scratch), input.left_x, input.left_y,
                            input.right_x, input.right_y, input.left_trigger,
                            input.right_trigger, scratch.device);
  }
  // Grip sense from the client (Moonlight extension LI_CCAP_GRIP_SENSE) is carried as
  // two button bits. The first one seen switches this controller to explicit grips for
  // good; until then, and for clients without the extension, the motion heuristic in
  // sc26_tick() stands in.
  constexpr std::uint32_t grip_bits = button_mask::left_grip_touch | button_mask::right_grip_touch;
  if ((input.buttons & grip_bits) != 0) {
    state->grip_explicit = true;
  }
  if (state->grip_explicit) {
    state->device.grip_touch[0] = (input.buttons & button_mask::left_grip_touch) != 0;
    state->device.grip_touch[1] = (input.buttons & button_mask::right_grip_touch) != 0;
  }
  apply_client_stick_touch(input.buttons, state);
  return sc::encode_input(sc26_buttons(input.buttons, *state), input.left_x, input.left_y,
                          input.right_x, input.right_y, input.left_trigger, input.right_trigger,
                          state->device);
}

sc26_battery_report encode_sc26_battery(const sc26_state &state) noexcept {
  return sc::encode_battery(state.device);
}

bool apply_sc26_touch(const touch_state_request &touch, sc26_state *const state) noexcept {
  if (state == nullptr) {
    return false;
  }
  auto &st = state->device;
  const auto event = static_cast<touch_event>(touch.event_type);
  if (event == touch_event::cancel_all) {
    st.pad_touched[0] = st.pad_touched[1] = false;
    return true;
  }
  if (touch.contact_index >= 2) {
    return false;
  }
  const std::uint8_t pad = touch.contact_index;
  switch (event) {
    case touch_event::down:
    case touch_event::move:
    case touch_event::hover:
      st.pad_touched[pad] = true;
      st.pad_x[pad] = touch.x;
      st.pad_y[pad] = touch.y;
      st.pad_pressure[pad] = touch.pressure;
      return true;
    case touch_event::up:
    case touch_event::cancel:
      st.pad_touched[pad] = false;
      st.pad_pressure[pad] = 0;
      return true;
    default:
      return false;
  }
}

void sc26_tick(sc26_state *const state, const std::uint64_t now_us) noexcept {
  if (state == nullptr) {
    return;
  }
  state->device.imu_timestamp = static_cast<std::uint32_t>(now_us);
  // Grips are reported only when the client reports them. The earlier motion
  // heuristic ("held while motion samples flow") turned both grips on for the
  // whole session, since a streaming client forwards IMU samples continuously,
  // and Steam shows a grip that never changes as not touched at all.
  if (!state->grip_explicit) {
    state->device.grip_touch[0] = false;
    state->device.grip_touch[1] = false;
  }
}

bool apply_sc26_motion(const motion_state_request &motion, sc26_state *const state,
                       const std::uint64_t now_us) noexcept {
  if (state == nullptr) {
    return false;
  }
  switch (static_cast<motion_kind>(motion.motion_type)) {
    case motion_kind::accelerometer:
      sc::apply_accel_milli(state->device, motion.x_milli, motion.y_milli, motion.z_milli);
      break;
    case motion_kind::gyroscope:
      sc::apply_gyro_milli(state->device, motion.x_milli, motion.y_milli, motion.z_milli);
      break;
    default:
      return false;
  }
  // The sample waits for the next report (input state or keep-alive); stamping
  // the clock here would make the keep-alive think a report just went out.
  state->last_motion_us = now_us != 0 ? now_us : 1;
  return true;
}

bool apply_sc26_battery(const battery_state_request &battery, sc26_state *const state) noexcept {
  if (state == nullptr) {
    return false;
  }
  auto &st = state->device;
  // 0xFF is LI_BATTERY_PERCENTAGE_UNKNOWN on the wire: keep the last known level
  // rather than reporting a full battery (the DualShock profile does the same).
  if (battery.percent <= 100) {
    st.battery_percent = battery.percent;
  }
  switch (static_cast<lvg::battery_state>(battery.flags)) {
    case lvg::battery_state::charging:
      st.charge_state = sc::charge_charging;
      break;
    case lvg::battery_state::full:
      st.charge_state = sc::charge_done;
      st.battery_percent = 100;
      break;
    case lvg::battery_state::discharging:
    case lvg::battery_state::not_charging:
    case lvg::battery_state::unknown:
    case lvg::battery_state::not_present:
    default:
      st.charge_state = sc::charge_discharging;
      break;
  }
  return true;
}

bool apply_sc26_output(
  const std::uint8_t *const data,
  const std::size_t size,
  const std::uint32_t controller_id,
  sc26_state *const state,
  feedback_event *const event) noexcept {
  if (state == nullptr || event == nullptr || data == nullptr || size < 2 ||
      !sc::is_output_report(data[0]) || data[0] > sc::haptic_script_id) {
    return false;
  }
  const std::size_t declared = sc::report_size(data[0]);
  const std::size_t length = declared;
  steam_haptic_feedback payload {};
  if (size < declared || length > sizeof(payload.report)) {
    return false;
  }
  // HID reports have the descriptor's fixed length. Ignore trailing transport
  // padding, but never turn a truncated write into a successful feedback event:
  // it would occupy a queue slot while the client cannot replay it.
  payload.length = static_cast<std::uint8_t>(length);
  std::memcpy(payload.report, data, length);
  *event = {};
  event->header.size = sizeof(*event);
  event->header.version = k_protocol_version;
  event->controller_id = controller_id;
  event->type = feedback_type::steam_haptic;
  event->payload_size = sizeof(payload);
  std::memcpy(event->payload, &payload, sizeof(payload));
  return true;
}

bool set_sc26_feature(
  const std::uint8_t report_id,
  const std::uint8_t *const buffer,
  const std::size_t size,
  sc26_state *const state) noexcept {
  if (state == nullptr || report_id != k_sc26_features_report_id) {
    return false;
  }
  return sc::set_feature(buffer, size, state->features);
}

std::size_t fill_sc26_feature(
  const std::uint8_t report_id,
  std::uint8_t *const buffer,
  const std::size_t capacity,
  const sc26_state &state) noexcept {
  if (report_id != k_sc26_features_report_id) {
    return 0;
  }
  return sc::get_feature(buffer, capacity, state.features);
}

}  // namespace lvg::driver
