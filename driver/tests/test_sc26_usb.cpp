// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Standalone cross-platform regression test for the Steam Controller (2026)
// wired contract: descriptor report sizes, literal-offset input encoding, the
// feature-report control channel, and haptic output decoding.
#include "libvirtualgamepad/sc26_usb.h"
#include <cstdio>
#include <cstring>
#include <map>

namespace {
int failures = 0;
void check(bool value, const char *message) {
  if (!value) { std::printf("FAIL: %s\n", message); ++failures; }
}
int le16(const std::uint8_t *p) { return static_cast<std::int16_t>(p[0] | (p[1] << 8)); }
unsigned ule16(const std::uint8_t *p) { return p[0] | (p[1] << 8); }
unsigned ule32(const std::uint8_t *p) { return ule16(p) | (ule16(p + 2) << 16); }
}

int main() {
  using namespace lvg::sc26_usb;

  // Descriptor: every report the structs describe has to be declared at the
  // wire size, whichever descriptor (provisional or dumped) is compiled in.
  std::map<unsigned, unsigned> input_bits, output_bits, feature_bits;
  unsigned id = 0, size = 0, count = 0, page = 0, collections = 0;
  for (std::size_t i = 0; i < report_descriptor_size;) {
    unsigned prefix = report_descriptor[i++];
    unsigned length = (prefix & 3) == 3 ? 4 : (prefix & 3);
    if (prefix == 0xfe || i + length > report_descriptor_size) return 2;
    unsigned value = 0;
    for (unsigned j = 0; j < length; ++j) value |= report_descriptor[i++] << (8 * j);
    switch (prefix & 0xfc) {
      case 0x04: page = value; break;
      case 0x84: id = value; break;
      case 0x74: size = value; break;
      case 0x94: count = value; break;
      case 0x80: input_bits[id] += size * count; break;
      case 0x90: output_bits[id] += size * count; break;
      case 0xb0: feature_bits[id] += size * count; break;
      case 0xa0: ++collections; break;
    }
  }
  check(page >= 0xff00u, "vendor-defined usage page");
  check(collections >= 1, "at least one application collection");
  check(input_bits[input_report_id] / 8 + 1 == input_report_size, "state report 0x42 is 54 bytes");
  check(input_bits[battery_report_id] / 8 + 1 == battery_report_size, "battery report 0x43 is 15 bytes");
  check(feature_bits[features_report_id] / 8 + 1 == feature_report_size, "feature report 1 is 64 bytes");
  for (unsigned r = haptic_rumble_id; r <= haptic_script_id; ++r) {
    check(output_bits[r] / 8 + 1 == haptic_report_size, "haptic output reports are 10 bytes");
  }

  // Input encoding at literal offsets (hid-steam steam_ibex_* tables).
  {
    state st {};
    st.pad_touched[1] = true;
    st.pad_x[1] = 65535;  // right edge
    st.pad_y[1] = 0;      // top
    st.pad_pressure[1] = 65535;
    apply_accel_milli(st, 0, 9807, 0);   // 1 g "up" in SDL axes
    apply_gyro_milli(st, 1000000, 0, 0); // 1000 dps about x
    const input_report r = encode_input(btn_a | btn_steam | btn_l4, 1000, -2000, 3000, 32767,
                                        255, 10, st);
    const auto *b = reinterpret_cast<const std::uint8_t *>(&r);
    check(b[0] == 0x42, "report id 0x42");
    check(b[1] == 0, "first sequence number is 0");
    check(st.sequence == 1, "sequence advanced");
    const unsigned buttons = ule32(b + 2);
    check((buttons & btn_a) && (buttons & btn_steam) && (buttons & btn_l4), "buttons carried");
    check(buttons & btn_left_trigger_click, "full left trigger sets its click bit");
    check(!(buttons & btn_right_trigger_click), "light right trigger has no click bit");
    check(buttons & btn_right_pad_touch, "touched right pad sets its touch bit");
    check(!(buttons & btn_left_pad_touch), "untouched left pad has no touch bit");
    check(buttons & btn_right_stick_touch, "deflected right stick counts as touched");
    check(!(buttons & btn_left_stick_touch), "centred-ish left stick is not touched");
    check(le16(b + 6) == 32767, "left trigger 255 -> 32767 at offset 6");
    check(le16(b + 8) == (10 * 32767) / 255, "right trigger scaled at offset 8");
    check(le16(b + 10) == 1000 && le16(b + 12) == -2000, "left stick at 10/12, positive up kept");
    check(le16(b + 14) == 3000 && le16(b + 16) == 32767, "right stick at 14/16");
    check(le16(b + 18) == 0 && le16(b + 20) == 0 && ule16(b + 22) == 0, "untouched left pad reads zero");
    check(le16(b + 24) == 32767, "right pad x at offset 24 is the right edge");
    check(le16(b + 26) == 32767, "right pad y at offset 26 is the top edge");
    check(ule16(b + 28) == 32767, "right pad pressure at offset 28 is full");
    check(le16(b + 34) == 0, "accel X at 34");
    check(le16(b + 36) == 0, "accel Y (device) at 36 for SDL-up gravity");
    check(le16(b + 38) == accel_counts_per_g, "accel Z (device) at 38 carries 1 g for SDL y");
    check(le16(b + 40) == 16384, "gyro X at 40: 1000 dps -> 16384 counts");
    check(le16(b + 46) == 32767 && le16(b + 48) == 0, "identity quaternion at 46");
    check(sizeof(r) == 54, "struct is the wire size");
  }

  // Pad axis conventions (SDL: x = raw/65536 + 0.5, y = -raw/65536 + 0.5).
  check(pad_axis_x(32768) == 0 && pad_axis_y(32768) == 0, "pad centre is zero");
  check(pad_axis_x(0) == -32768 && pad_axis_x(65535) == 32767, "pad x spans the range");
  check(pad_axis_y(0) == 32767 && pad_axis_y(65535) == -32767, "pad y is positive up");

  // Battery report.
  {
    state st {};
    st.battery_percent = 42;
    st.charge_state = charge_charging;
    const battery_report r = encode_battery(st);
    const auto *b = reinterpret_cast<const std::uint8_t *>(&r);
    check(b[0] == 0x43 && b[1] == charge_charging && b[2] == 42, "battery report id/state/level");
    check(sizeof(r) == 15, "battery report is 15 bytes");
  }

  // Control channel: set then get, like the firmware.
  {
    feature_state fs {};
    fs.attrs.firmware_build_time = 0x6789ABCD;
    fs.attrs.board_revision = 0x0C;
    std::uint8_t cmd[64] {};
    std::uint8_t reply[64] {};

    // Set settings: lizard off, IMU raw accel|gyro.
    cmd[0] = 1; cmd[1] = cmd_set_settings_values; cmd[2] = 6;
    cmd[3] = setting_lizard_mode; cmd[4] = 0; cmd[5] = 0;
    cmd[6] = setting_imu_mode; cmd[7] = 0x18; cmd[8] = 0;
    check(set_feature(cmd, sizeof(cmd), fs), "set settings accepted");
    check(!fs.lizard_mode && fs.imu_mode == 0x18, "settings applied");
    check(get_feature(reply, sizeof(reply), fs) == 64, "reply is a full report");
    check(reply[0] == 1 && reply[1] == cmd_set_settings_values && reply[2] == 0, "set settings reply echoes type with no payload");

    // Get attributes.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_attributes_values; cmd[2] = 0;
    check(set_feature(cmd, sizeof(cmd), fs), "get attributes accepted");
    check(get_feature(reply, sizeof(reply), fs) == 64, "attributes reply");
    check(reply[1] == cmd_get_attributes_values && reply[2] % 5 == 0 && reply[2] >= 10, "attribute list of (tag,u32)");
    bool saw_pid = false, saw_fw = false;
    for (unsigned i = 0; i < reply[2]; i += 5) {
      if (reply[3 + i] == attr_product_id) saw_pid = ule32(reply + 4 + i) == product_id;
      if (reply[3 + i] == attr_firmware_build_time) saw_fw = ule32(reply + 4 + i) == 0x6789ABCD;
    }
    check(saw_pid && saw_fw, "product id and firmware build time attributes present");

    // Serial, framed as hid-steam reads it: [0xAE][len][0x15][chars].
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_string_attribute; cmd[2] = 1; cmd[3] = string_attr_unit_serial;
    check(set_feature(cmd, sizeof(cmd), fs), "get serial accepted");
    check(get_feature(reply, sizeof(reply), fs) == 64, "serial reply");
    check(reply[1] == cmd_get_string_attribute && reply[3] == string_attr_unit_serial, "serial tag echoed");
    check(reply[2] == 1 + std::strlen(fs.unit_serial.data()), "serial length covers tag and text");
    check(std::memcmp(reply + 4, fs.unit_serial.data(), std::strlen(fs.unit_serial.data())) == 0, "serial text");

    // Get settings for explicit ids.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_settings_values; cmd[2] = 2; cmd[3] = setting_imu_mode; cmd[4] = setting_lizard_mode;
    check(set_feature(cmd, sizeof(cmd), fs), "get settings accepted");
    check(get_feature(reply, sizeof(reply), fs) == 64, "settings reply");
    check(reply[2] == 6 && reply[3] == setting_imu_mode && ule16(reply + 4) == 0x18 &&
          reply[6] == setting_lizard_mode && ule16(reply + 7) == 0, "settings triples");

    // Unknown command is acknowledged, counted, not refused.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = 0xC1; cmd[2] = 0;
    check(set_feature(cmd, sizeof(cmd), fs), "unknown command acknowledged");
    check(fs.unknown_commands == 1 && fs.last_unknown_command == 0xC1, "unknown command recorded");
    check(get_feature(reply, sizeof(reply), fs) == 64 && reply[1] == 0xC1 && reply[2] == 0, "unknown reply is empty");

    // Malformed: wrong report id, short transfer, length past the end.
    cmd[0] = 2;
    check(!set_feature(cmd, sizeof(cmd), fs), "wrong report id refused");
    cmd[0] = 1;
    check(!set_feature(cmd, 2, fs), "short transfer refused");
    cmd[1] = cmd_set_settings_values; cmd[2] = 62;
    check(!set_feature(cmd, 10, fs), "declared length past the transfer refused");
    check(get_feature(reply, 10, fs) == 0, "small get buffer refused");

    // Load defaults resets settings.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_load_default_settings;
    check(set_feature(cmd, sizeof(cmd), fs) && fs.lizard_mode && fs.imu_mode == 0, "load defaults");
  }

  // Haptic output decoding.
  {
    rumble r {};
    std::uint8_t out[10] {haptic_rumble_id, 0, 0, 0, 0x00, 0x80, 0, 0xff, 0x3f, 0};
    check(decode_haptic_output(out, sizeof(out), r), "rumble report decoded");
    check(r.left == 0x8000 && r.right == 0x3fff, "rumble speeds at offsets 4 and 7");
    std::uint8_t pulse[8] {haptic_pulse_id, 1, 0x10, 0x27, 0x10, 0x27, 0xff, 0xff};  // 10 ms on, 10 ms off, left
    check(decode_haptic_output(pulse, sizeof(pulse), r), "pulse report decoded");
    check(r.left == 32767 && r.right == 0x3fff, "50% duty pulse on the left pad, right untouched");
    std::uint8_t stop[8] {haptic_pulse_id, 1, 0, 0, 0, 0, 0, 0};
    check(decode_haptic_output(stop, sizeof(stop), r) && r.left == 0, "zero-repeat pulse stops the left pad");
    std::uint8_t other[10] {haptic_lfo_id};
    check(!decode_haptic_output(other, sizeof(other), r), "LFO report is not rumble");
    check(!decode_haptic_output(out, 5, r), "short rumble report refused");
  }

  if (failures == 0) std::printf("PASS: sc26 usb contract\n");
  return failures == 0 ? 0 : 1;
}
