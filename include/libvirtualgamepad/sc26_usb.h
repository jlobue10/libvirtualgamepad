// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// Steam Controller (2026, Valve "Ibex") wired USB contract. Portable: no
// Windows headers, so the encoders and the feature-report responder can be unit
// tested on any platform. The driver adapts Vibeshine's protocol structs onto
// the plain integer API here.
//
// Sources (all public, none from Valve firmware or Steam binaries):
//   - Linux drivers/hid/hid-steam.c (Ibex report ids, sizes, byte offsets, the
//     feature-report command set and reply framing)
//   - SDL src/joystick/hidapi/SDL_hidapi_steam_triton.c and Valve's
//     controller_structs.h / controller_constants.h (button bits, state layout,
//     haptic output reports, settings/attribute ids, sensor and axis scaling)
//   - Linux drivers/hid/hid-ids.h and SDL usb_ids.h (vendor and product ids)
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lvg::sc26_usb {

inline constexpr std::uint16_t vendor_id = 0x28DE;
inline constexpr std::uint16_t product_id = 0x1302;   // Wired 2026 controller.
inline constexpr std::uint16_t product_id_ble = 0x1303;
// bcdDevice of the real unit. Provisional until the owner's descriptor dump.
inline constexpr std::uint16_t version = 0x0100;

// Report ids.
inline constexpr std::uint8_t features_report_id = 0x01;  // Control channel.
inline constexpr std::uint8_t input_report_id = 0x42;     // Wired state (with quaternion).
inline constexpr std::uint8_t battery_report_id = 0x43;
inline constexpr std::uint8_t input_report_id_ble = 0x45; // BLE state, no quaternion.
inline constexpr std::uint8_t wireless_report_id = 0x79;
inline constexpr std::uint8_t haptic_rumble_id = 0x80;
inline constexpr std::uint8_t haptic_pulse_id = 0x81;
inline constexpr std::uint8_t haptic_command_id = 0x82;
inline constexpr std::uint8_t haptic_lfo_id = 0x83;
inline constexpr std::uint8_t haptic_sweep_id = 0x84;
inline constexpr std::uint8_t haptic_script_id = 0x85;

inline constexpr std::size_t input_report_size = 54;    // hid-steam: REPORT_ID_INPUT size 54
inline constexpr std::size_t battery_report_size = 15;  // hid-steam: REPORT_ID_BATTERY size 15
inline constexpr std::size_t feature_report_size = 64;  // id + 63 bytes (HID_FEATURE_REPORT_BYTES)
inline constexpr std::size_t haptic_report_size = 10;   // id + 9 (HID_RUMBLE_OUTPUT_REPORT_BYTES)

// Button bits of the u32 at offset 2 (SDL TritonButtons).
enum button : std::uint32_t {
  btn_a = 0x00000001u,
  btn_b = 0x00000002u,
  btn_x = 0x00000004u,
  btn_y = 0x00000008u,
  btn_qam = 0x00000010u,
  btn_r3 = 0x00000020u,
  btn_view = 0x00000040u,
  btn_r4 = 0x00000080u,
  btn_r5 = 0x00000100u,
  btn_r = 0x00000200u,
  btn_dpad_down = 0x00000400u,
  btn_dpad_right = 0x00000800u,
  btn_dpad_left = 0x00001000u,
  btn_dpad_up = 0x00002000u,
  btn_menu = 0x00004000u,
  btn_l3 = 0x00008000u,
  btn_steam = 0x00010000u,
  btn_l4 = 0x00020000u,
  btn_l5 = 0x00040000u,
  btn_l = 0x00080000u,
  btn_right_stick_touch = 0x00100000u,
  btn_right_pad_touch = 0x00200000u,
  btn_right_pad_click = 0x00400000u,
  btn_right_trigger_click = 0x00800000u,
  btn_left_stick_touch = 0x01000000u,
  btn_left_pad_touch = 0x02000000u,
  btn_left_pad_click = 0x04000000u,
  btn_left_trigger_click = 0x08000000u,
  btn_right_grip_touch = 0x10000000u,
  btn_left_grip_touch = 0x20000000u,
};

// Feature-report commands (controller_constants.h / hid-steam.c).
enum command : std::uint8_t {
  cmd_clear_digital_mappings = 0x81,
  cmd_get_attributes_values = 0x83,
  cmd_set_default_digital_mappings = 0x85,
  cmd_set_settings_values = 0x87,
  cmd_get_settings_values = 0x89,
  cmd_get_settings_maxs = 0x8B,
  cmd_get_settings_defaults = 0x8C,
  cmd_load_default_settings = 0x8E,
  cmd_trigger_haptic_pulse = 0x8F,
  cmd_get_string_attribute = 0xAE,
};

enum attribute : std::uint8_t {
  attr_unique_id = 0,
  attr_product_id = 1,
  attr_capabilities = 2,
  attr_firmware_version = 3,
  attr_firmware_build_time = 4,
  attr_radio_firmware_build_time = 5,
  attr_radio_device_id0 = 6,
  attr_radio_device_id1 = 7,
  attr_dongle_firmware_build_time = 8,
  attr_board_revision = 9,
  attr_bootloader_build_time = 10,
  attr_connection_interval_us = 11,
};
inline constexpr std::uint8_t string_attr_board_serial = 0x14;
inline constexpr std::uint8_t string_attr_unit_serial = 0x15;

inline constexpr std::uint8_t setting_lizard_mode = 9;
inline constexpr std::uint8_t setting_imu_mode = 48;
inline constexpr std::uint8_t setting_count = 99;  // SETTING_COUNT in controller_constants.h

// Charge states of the battery report (EChargeState).
inline constexpr std::uint8_t charge_reset = 0;
inline constexpr std::uint8_t charge_discharging = 1;
inline constexpr std::uint8_t charge_charging = 2;
inline constexpr std::uint8_t charge_src_validate = 3;
inline constexpr std::uint8_t charge_done = 4;

// Sensor scaling (SDL): gyro full scale 2000 deg/s over +/-32768, accel 2 g.
inline constexpr std::int32_t gyro_counts_per_1000_dps = 16384;  // counts = dps*1000 * 16384 / 1000000
inline constexpr std::int32_t accel_counts_per_g = 16384;
inline constexpr std::int32_t milli_g = 9807;  // Earth gravity in mm/s^2.

#pragma pack(push, 1)

// TritonMTUFull_t behind the report id (controller_structs.h); offsets below
// match hid-steam's steam_ibex_*_mappings tables.
struct input_report {
  std::uint8_t report_id;
  std::uint8_t sequence;
  std::uint32_t buttons;
  std::int16_t left_trigger;   // 0..32767
  std::int16_t right_trigger;
  std::int16_t left_x;         // +/-32767, positive up/right
  std::int16_t left_y;
  std::int16_t right_x;
  std::int16_t right_y;
  std::int16_t left_pad_x;     // +/-32767, zero when untouched
  std::int16_t left_pad_y;
  std::uint16_t left_pressure; // 0..32767
  std::int16_t right_pad_x;
  std::int16_t right_pad_y;
  std::uint16_t right_pressure;
  std::uint32_t imu_timestamp;
  std::int16_t accel_x;
  std::int16_t accel_y;
  std::int16_t accel_z;
  std::int16_t gyro_x;
  std::int16_t gyro_y;
  std::int16_t gyro_z;
  std::int16_t quat_w;
  std::int16_t quat_x;
  std::int16_t quat_y;
  std::int16_t quat_z;
};

// TritonBatteryStatus_t behind the report id.
struct battery_report {
  std::uint8_t report_id;
  std::uint8_t charge_state;
  std::uint8_t battery_level;  // 0..100
  std::uint16_t battery_voltage_mv;
  std::uint16_t system_voltage_mv;
  std::uint16_t input_voltage_mv;
  std::uint16_t current_ma;
  std::uint16_t input_current_ma;
  std::uint16_t temperature;
};

// Control channel: [report id][type][length][payload].
struct feature_report {
  std::uint8_t report_id;
  std::uint8_t type;
  std::uint8_t length;
  std::uint8_t payload[feature_report_size - 3];
};

struct haptic_rumble_report {  // 0x80
  std::uint8_t report_id;
  std::uint8_t type;
  std::uint16_t intensity;
  std::uint16_t left_speed;
  std::int8_t left_gain;
  std::uint16_t right_speed;
  std::int8_t right_gain;
};

struct haptic_pulse_report {  // 0x81
  std::uint8_t report_id;
  std::uint8_t side;  // firmware: 1 = left, 0 = right
  std::uint16_t on_us;
  std::uint16_t off_us;
  std::uint16_t repeat;
};

#pragma pack(pop)

static_assert(sizeof(input_report) == input_report_size);
static_assert(sizeof(battery_report) == battery_report_size);
static_assert(sizeof(feature_report) == feature_report_size);
static_assert(sizeof(haptic_rumble_report) == haptic_report_size);
static_assert(sizeof(haptic_pulse_report) == 8);
// Pinned to hid-steam's Ibex tables (byte index into the report incl. id).
static_assert(offsetof(input_report, buttons) == 2);
static_assert(offsetof(input_report, left_trigger) == 6);
static_assert(offsetof(input_report, right_trigger) == 8);
static_assert(offsetof(input_report, left_x) == 10);
static_assert(offsetof(input_report, left_y) == 12);
static_assert(offsetof(input_report, right_x) == 14);
static_assert(offsetof(input_report, right_y) == 16);
static_assert(offsetof(input_report, left_pad_x) == 18);
static_assert(offsetof(input_report, left_pad_y) == 20);
static_assert(offsetof(input_report, right_pad_x) == 24);
static_assert(offsetof(input_report, right_pad_y) == 26);
static_assert(offsetof(input_report, imu_timestamp) == 30);
static_assert(offsetof(input_report, accel_x) == 34);
static_assert(offsetof(input_report, accel_z) == 38);
static_assert(offsetof(input_report, gyro_x) == 40);
static_assert(offsetof(input_report, gyro_z) == 44);
static_assert(offsetof(input_report, quat_w) == 46);

// PROVISIONAL report descriptor. Shape only: one vendor-defined collection with
// the input, battery, feature and haptic reports above at their wire sizes.
// It is replaced by the owner's dump of a real wired controller before the
// profile is enabled (PROFILE_CONTRACT: descriptors from permitted sources),
// and the tests pin the real one's report sizes to the structs above.
inline constexpr std::uint8_t report_descriptor[] = {
  0x06, 0xff, 0xff,        // Usage Page (Vendor 0xFFFF)
  0x09, 0x01,              // Usage (1)
  0xa1, 0x01,              // Collection (Application)
  0x15, 0x00,              //   Logical Minimum (0)
  0x26, 0xff, 0x00,        //   Logical Maximum (255)
  0x75, 0x08,              //   Report Size (8)
  0x85, 0x42,              //   Report ID (0x42)
  0x09, 0x02,              //   Usage (2)
  0x95, 0x35,              //   Report Count (53)
  0x81, 0x02,              //   Input (Data,Var,Abs)
  0x85, 0x43,              //   Report ID (0x43)
  0x09, 0x03,              //   Usage (3)
  0x95, 0x0e,              //   Report Count (14)
  0x81, 0x02,              //   Input
  0x85, 0x01,              //   Report ID (1)
  0x09, 0x04,              //   Usage (4)
  0x95, 0x3f,              //   Report Count (63)
  0xb1, 0x02,              //   Feature (Data,Var,Abs)
  0x85, 0x80, 0x09, 0x05, 0x95, 0x09, 0x91, 0x02,  // Output 0x80, 9 bytes
  0x85, 0x81, 0x09, 0x06, 0x95, 0x09, 0x91, 0x02,  // Output 0x81
  0x85, 0x82, 0x09, 0x07, 0x95, 0x09, 0x91, 0x02,  // Output 0x82
  0x85, 0x83, 0x09, 0x08, 0x95, 0x09, 0x91, 0x02,  // Output 0x83
  0x85, 0x84, 0x09, 0x09, 0x95, 0x09, 0x91, 0x02,  // Output 0x84
  0x85, 0x85, 0x09, 0x0a, 0x95, 0x09, 0x91, 0x02,  // Output 0x85
  0xc0,                    // End Collection
};
inline constexpr std::size_t report_descriptor_size = sizeof(report_descriptor);
inline constexpr bool report_descriptor_is_provisional = true;

// ---- Input encoding ------------------------------------------------------

// Normalized state in Vibeshine's conventions: buttons as a bitmask the caller
// has already translated to `button` bits; sticks +/-32767 positive up/right;
// triggers 0..255; pads 0..65535 with (0,0) top-left as the protocol's touch
// requests carry them; pressure 0..65535; motion in milli m/s^2 and milli dps
// in SDL axis order (x right, y up, z toward the player).
struct state {
  std::uint8_t sequence {};
  std::uint32_t imu_timestamp {};
  bool pad_touched[2] {};
  std::uint16_t pad_x[2] {};
  std::uint16_t pad_y[2] {};
  std::uint16_t pad_pressure[2] {};
  std::int16_t accel[3] {0, 0, static_cast<std::int16_t>(accel_counts_per_g)};  // device x,y,z
  std::int16_t gyro[3] {};
  std::uint8_t battery_percent {100};
  std::uint8_t charge_state {charge_discharging};
  bool stick_touch_from_deflection {true};

  void reset() noexcept { *this = state {}; }
};

[[nodiscard]] inline std::int16_t clamp_i16(const std::int32_t value) noexcept {
  return static_cast<std::int16_t>(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
}

[[nodiscard]] inline std::int16_t pad_axis_x(const std::uint16_t x) noexcept {
  // SDL: x_norm = raw / 65536 + 0.5  =>  raw = (x_norm - 0.5) * 65536
  return clamp_i16(static_cast<std::int32_t>(x) - 32768);
}

[[nodiscard]] inline std::int16_t pad_axis_y(const std::uint16_t y) noexcept {
  // SDL: y_norm = -raw / 65536 + 0.5  =>  raw = (0.5 - y_norm) * 65536
  return clamp_i16(32768 - static_cast<std::int32_t>(y));
}

[[nodiscard]] inline std::uint16_t pad_pressure(const std::uint16_t pressure) noexcept {
  return static_cast<std::uint16_t>(pressure >> 1);  // 0..65535 -> 0..32767
}

[[nodiscard]] inline std::int16_t trigger_axis(const std::uint8_t trigger) noexcept {
  return static_cast<std::int16_t>((static_cast<std::int32_t>(trigger) * 32767) / 255);
}

// Threshold at which the firmware reports the trigger's end-stop click.
inline constexpr std::uint8_t trigger_click_threshold = 0xF0;
// Deflection beyond which a stick counts as touched when the client cannot
// report capacitive touch itself.
inline constexpr std::int32_t stick_touch_deflection = 3276;

[[nodiscard]] inline input_report encode_input(
  const std::uint32_t buttons,
  const std::int16_t left_x, const std::int16_t left_y,
  const std::int16_t right_x, const std::int16_t right_y,
  const std::uint8_t left_trigger, const std::uint8_t right_trigger,
  state &st) noexcept {
  input_report r {};
  r.report_id = input_report_id;
  r.sequence = st.sequence++;
  std::uint32_t bits = buttons;
  if (left_trigger >= trigger_click_threshold) bits |= btn_left_trigger_click;
  if (right_trigger >= trigger_click_threshold) bits |= btn_right_trigger_click;
  if (st.pad_touched[0]) bits |= btn_left_pad_touch;
  if (st.pad_touched[1]) bits |= btn_right_pad_touch;
  if (st.stick_touch_from_deflection) {
    const auto deflected = [](const std::int16_t x, const std::int16_t y) {
      const std::int32_t ax = x < 0 ? -static_cast<std::int32_t>(x) : x;
      const std::int32_t ay = y < 0 ? -static_cast<std::int32_t>(y) : y;
      return ax > stick_touch_deflection || ay > stick_touch_deflection;
    };
    if (deflected(left_x, left_y)) bits |= btn_left_stick_touch;
    if (deflected(right_x, right_y)) bits |= btn_right_stick_touch;
  }
  r.buttons = bits;
  r.left_trigger = trigger_axis(left_trigger);
  r.right_trigger = trigger_axis(right_trigger);
  r.left_x = left_x;
  r.left_y = left_y;
  r.right_x = right_x;
  r.right_y = right_y;
  if (st.pad_touched[0]) {
    r.left_pad_x = pad_axis_x(st.pad_x[0]);
    r.left_pad_y = pad_axis_y(st.pad_y[0]);
    r.left_pressure = pad_pressure(st.pad_pressure[0]);
  }
  if (st.pad_touched[1]) {
    r.right_pad_x = pad_axis_x(st.pad_x[1]);
    r.right_pad_y = pad_axis_y(st.pad_y[1]);
    r.right_pressure = pad_pressure(st.pad_pressure[1]);
  }
  r.imu_timestamp = st.imu_timestamp;
  r.accel_x = st.accel[0];
  r.accel_y = st.accel[1];
  r.accel_z = st.accel[2];
  r.gyro_x = st.gyro[0];
  r.gyro_y = st.gyro[1];
  r.gyro_z = st.gyro[2];
  // No orientation filter runs here; identity quaternion.
  r.quat_w = 32767;
  return r;
}

// Motion in SDL axis order -> device axes. SDL reads device X as x, device Z as
// y, and -device Y as z, so the inverse is x->X, y->Z, -z->Y.
inline void apply_accel_milli(state &st, const std::int32_t x, const std::int32_t y,
                              const std::int32_t z) noexcept {
  const auto convert = [](const std::int32_t milli) {
    return clamp_i16(static_cast<std::int32_t>((static_cast<std::int64_t>(milli) * accel_counts_per_g) / milli_g));
  };
  st.accel[0] = convert(x);
  st.accel[2] = convert(y);
  st.accel[1] = convert(-z);
}

inline void apply_gyro_milli(state &st, const std::int32_t x, const std::int32_t y,
                             const std::int32_t z) noexcept {
  const auto convert = [](const std::int32_t milli) {
    return clamp_i16(static_cast<std::int32_t>((static_cast<std::int64_t>(milli) * gyro_counts_per_1000_dps) / 1000000));
  };
  st.gyro[0] = convert(x);
  st.gyro[2] = convert(y);
  st.gyro[1] = convert(-z);
}

[[nodiscard]] inline battery_report encode_battery(const state &st) noexcept {
  battery_report r {};
  r.report_id = battery_report_id;
  r.charge_state = st.charge_state;
  r.battery_level = st.battery_percent > 100 ? 100 : st.battery_percent;
  // Plausible pack figures; nothing on the host side acts on them.
  r.battery_voltage_mv = static_cast<std::uint16_t>(3500 + (r.battery_level * 7));
  r.system_voltage_mv = 3300;
  return r;
}

// ---- Feature-report control channel ---------------------------------------

struct attributes {
  std::uint32_t unique_id {};
  // Qualified: the member name would otherwise shadow the namespace constant.
  std::uint32_t product_id {lvg::sc26_usb::product_id};
  std::uint32_t capabilities {};
  std::uint32_t firmware_build_time {};   // Copy from a real unit (owner capture).
  std::uint32_t board_revision {};
  std::uint32_t bootloader_build_time {};
  std::uint32_t connection_interval_us {4000};
};

struct feature_state {
  attributes attrs {};
  std::array<char, 16> unit_serial {"LVGSC260000"};
  std::uint16_t settings[setting_count] {};
  bool lizard_mode {true};
  std::uint8_t imu_mode {};
  // Reply to the most recent command, read back with GetFeature.
  std::uint8_t reply[feature_report_size] {};
  bool have_reply {};
  std::uint8_t last_unknown_command {};
  std::uint32_t unknown_commands {};

  void reset() noexcept {
    const attributes keep = attrs;
    const auto serial = unit_serial;
    *this = feature_state {};
    attrs = keep;
    unit_serial = serial;
  }
};

inline void put_le16(std::uint8_t *p, const std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put_le32(std::uint8_t *p, const std::uint32_t v) noexcept {
  put_le16(p, static_cast<std::uint16_t>(v));
  put_le16(p + 2, static_cast<std::uint16_t>(v >> 16));
}
[[nodiscard]] inline std::uint16_t get_le16(const std::uint8_t *p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// Handles a SetFeature on the control channel. `data` starts at the report id
// (data[0] == features_report_id); `size` is the HID transfer length. Returns
// false for a malformed transfer; unknown commands are acknowledged with an
// empty reply and counted so the host can surface them.
[[nodiscard]] inline bool set_feature(const std::uint8_t *data, const std::size_t size,
                                      feature_state &fs) noexcept {
  if (data == nullptr || size < 3 || data[0] != features_report_id) return false;
  const std::uint8_t type = data[1];
  const std::uint8_t length = data[2];
  const std::uint8_t *payload = data + 3;
  const std::size_t available = size - 3;
  if (length > available) return false;

  std::memset(fs.reply, 0, sizeof(fs.reply));
  fs.reply[0] = features_report_id;
  fs.reply[1] = type;
  fs.reply[2] = 0;
  fs.have_reply = true;
  std::uint8_t *out = fs.reply + 3;
  constexpr std::size_t out_capacity = feature_report_size - 3;

  switch (type) {
    case cmd_clear_digital_mappings:
    case cmd_set_default_digital_mappings:
      return true;
    case cmd_load_default_settings:
      std::memset(fs.settings, 0, sizeof(fs.settings));
      fs.lizard_mode = true;
      fs.imu_mode = 0;
      return true;
    case cmd_set_settings_values: {
      for (std::size_t i = 0; i + 3 <= length; i += 3) {
        const std::uint8_t id = payload[i];
        const std::uint16_t value = get_le16(payload + i + 1);
        if (id < setting_count) fs.settings[id] = value;
        if (id == setting_lizard_mode) fs.lizard_mode = value != 0;
        if (id == setting_imu_mode) fs.imu_mode = static_cast<std::uint8_t>(value);
      }
      return true;
    }
    case cmd_get_settings_values:
    case cmd_get_settings_defaults:
    case cmd_get_settings_maxs: {
      // Request payload lists the setting ids wanted; an empty request returns
      // the first ids that fit. Replies are (id, value) triples.
      std::size_t n = 0;
      const std::size_t max_triples = out_capacity / 3;
      if (length == 0) {
        for (std::uint8_t id = 0; id < setting_count && n < max_triples; ++id, ++n) {
          out[n * 3] = id;
          put_le16(out + n * 3 + 1, type == cmd_get_settings_values ? fs.settings[id]
                                     : type == cmd_get_settings_maxs ? 0xFFFFu : 0u);
        }
      } else {
        for (std::size_t i = 0; i < length && n < max_triples; ++i, ++n) {
          const std::uint8_t id = payload[i];
          out[n * 3] = id;
          const std::uint16_t v = id < setting_count && type == cmd_get_settings_values ? fs.settings[id]
                                  : type == cmd_get_settings_maxs ? 0xFFFFu : 0u;
          put_le16(out + n * 3 + 1, v);
        }
      }
      fs.reply[2] = static_cast<std::uint8_t>(n * 3);
      return true;
    }
    case cmd_get_attributes_values: {
      const struct { std::uint8_t tag; std::uint32_t value; } list[] = {
        {attr_unique_id, fs.attrs.unique_id},
        {attr_product_id, fs.attrs.product_id},
        {attr_capabilities, fs.attrs.capabilities},
        {attr_firmware_build_time, fs.attrs.firmware_build_time},
        {attr_board_revision, fs.attrs.board_revision},
        {attr_bootloader_build_time, fs.attrs.bootloader_build_time},
        {attr_connection_interval_us, fs.attrs.connection_interval_us},
      };
      std::size_t n = 0;
      for (const auto &a : list) {
        if ((n + 1) * 5 > out_capacity) break;
        out[n * 5] = a.tag;
        put_le32(out + n * 5 + 1, a.value);
        ++n;
      }
      fs.reply[2] = static_cast<std::uint8_t>(n * 5);
      return true;
    }
    case cmd_get_string_attribute: {
      // Request: [len][tag]; reply: [tag][string]. hid-steam expects the tag at
      // payload[0] and the serial right after it.
      const std::uint8_t tag = length >= 1 ? payload[0] : string_attr_unit_serial;
      out[0] = tag;
      const char *text = fs.unit_serial.data();
      std::size_t n = 0;
      while (text[n] != '\0' && n + 1 < out_capacity - 1 && n < fs.unit_serial.size()) {
        out[1 + n] = static_cast<std::uint8_t>(text[n]);
        ++n;
      }
      fs.reply[2] = static_cast<std::uint8_t>(1 + n);
      return true;
    }
    case cmd_trigger_haptic_pulse:
      // Legacy pulse command on the control channel; the output report is the
      // 2026 path. Accept it so a host that still uses it is not refused.
      return true;
    default:
      fs.last_unknown_command = type;
      ++fs.unknown_commands;
      return true;
  }
}

// Answers a GetFeature on the control channel with the reply to the last
// command. Returns the number of bytes written (always the full report), or 0
// when the buffer cannot hold it.
[[nodiscard]] inline std::size_t get_feature(std::uint8_t *buffer, const std::size_t capacity,
                                             const feature_state &fs) noexcept {
  if (buffer == nullptr || capacity < feature_report_size) return 0;
  std::memset(buffer, 0, capacity);
  if (fs.have_reply) {
    std::memcpy(buffer, fs.reply, feature_report_size);
  } else {
    buffer[0] = features_report_id;
  }
  return feature_report_size;
}

// ---- Haptic output ---------------------------------------------------------

struct rumble {
  std::uint16_t left {};   // 0..65535
  std::uint16_t right {};
};

// Decodes the two haptic output reports a host uses for rumble. Pulses become a
// magnitude from their duty cycle so a client without pad actuators can still
// render them. Returns false for other report ids or short transfers.
[[nodiscard]] inline bool decode_haptic_output(const std::uint8_t *data, const std::size_t size,
                                               rumble &current) noexcept {
  if (data == nullptr || size < 2) return false;
  if (data[0] == haptic_rumble_id) {
    if (size < sizeof(haptic_rumble_report)) return false;
    current.left = get_le16(data + 4);
    current.right = get_le16(data + 7);
    return true;
  }
  if (data[0] == haptic_pulse_id) {
    if (size < sizeof(haptic_pulse_report)) return false;
    const std::uint16_t on_us = get_le16(data + 2);
    const std::uint16_t off_us = get_le16(data + 4);
    const std::uint16_t repeat = get_le16(data + 6);
    std::uint16_t magnitude = 0;
    if (repeat != 0 && on_us != 0) {
      const std::uint32_t period = static_cast<std::uint32_t>(on_us) + off_us;
      magnitude = static_cast<std::uint16_t>((static_cast<std::uint32_t>(on_us) * 65535u) / (period == 0 ? 1u : period));
    }
    if (data[1] == 1) current.left = magnitude; else current.right = magnitude;
    return true;
  }
  return false;
}

}  // namespace lvg::sc26_usb
