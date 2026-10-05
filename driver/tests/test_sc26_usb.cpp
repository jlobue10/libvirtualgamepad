// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Standalone cross-platform regression test for the Steam Controller (2026)
// wired contract: the captured descriptor's report sizes, literal-offset input
// encoding, the feature-report control channel (including a replay of the
// commands Steam sent a real unit), and haptic output decoding.
#include "libvirtualgamepad/sc26_usb.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char *message) {
  if (!value) { std::printf("FAIL: %s\n", message); ++failures; }
}
int le16(const std::uint8_t *p) { return static_cast<std::int16_t>(p[0] | (p[1] << 8)); }
unsigned ule16(const std::uint8_t *p) { return p[0] | (p[1] << 8); }
unsigned ule32(const std::uint8_t *p) { return ule16(p) | (ule16(p + 2) << 16); }

std::vector<std::uint8_t> from_hex(const char *hex) {
  std::vector<std::uint8_t> out;
  for (; hex[0] != '\0' && hex[1] != '\0'; hex += 2) {
    const auto nibble = [](const char c) -> unsigned {
      return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0';
    };
    out.push_back(static_cast<std::uint8_t>((nibble(hex[0]) << 4) | nibble(hex[1])));
  }
  out.resize(64);  // SetFeature transfers are the full 64-byte report.
  return out;
}
}

int main() {
  using namespace lvg::sc26_usb;

  check(version == 0x0307, "bcdDevice is the captured unit's 0x0307");
  check(!report_descriptor_is_provisional, "the real descriptor is compiled in");

  // Descriptor walk: every report the real descriptor declares has to match the
  // size table, and every report in the size table has to be declared.
  std::map<unsigned, unsigned> input_bits, output_bits, feature_bits;
  std::map<unsigned, unsigned> application_pages;  // top-level collection -> usage page
  unsigned id = 0, size = 0, count = 0, page = 0, usage = 0, depth = 0, applications = 0;
  for (std::size_t i = 0; i < report_descriptor_size;) {
    unsigned prefix = report_descriptor[i++];
    unsigned length = (prefix & 3) == 3 ? 4 : (prefix & 3);
    if (prefix == 0xfe || i + length > report_descriptor_size) return 2;
    unsigned value = 0;
    for (unsigned j = 0; j < length; ++j) value |= report_descriptor[i++] << (8 * j);
    switch (prefix & 0xfc) {
      case 0x04: page = value; break;
      case 0x08: usage = value; break;
      case 0x84: id = value; break;
      case 0x74: size = value; break;
      case 0x94: count = value; break;
      case 0x80: input_bits[id] += size * count; break;
      case 0x90: output_bits[id] += size * count; break;
      case 0xb0: feature_bits[id] += size * count; break;
      case 0xa0:
        if (depth++ == 0) application_pages[applications++] = (page << 16) | usage;
        break;
      case 0xc0: --depth; break;
    }
  }
  check(depth == 0, "collections balance");
  check(applications == 3, "three top-level collections (mouse, keyboard, vendor)");
  check(application_pages[0] == 0x00010002u, "first collection is Generic Desktop / Mouse (lizard mode)");
  check(application_pages[1] == 0x00010006u, "second collection is Generic Desktop / Keyboard (lizard mode)");
  check(application_pages[2] == 0xff000001u, "third collection is vendor page 0xFF00 usage 1 (what Steam opens)");
  check(input_bits[input_report_id] / 8 + 1 == input_report_size, "state report 0x42 is 54 bytes");
  check(input_bits[battery_report_id] / 8 + 1 == battery_report_size, "battery report 0x43 is 15 bytes");
  check(feature_bits[features_report_id] / 8 + 1 == feature_report_size, "feature report 1 is 64 bytes");
  check(feature_bits[features_report_id_2] / 8 + 1 == feature_report_size, "feature report 2 is 64 bytes");
  check(output_bits[haptic_rumble_id] / 8 + 1 == haptic_report_size, "rumble report 0x80 is 10 bytes");
  check(output_bits[haptic_pulse_id] / 8 + 1 == haptic_pulse_report_size, "pulse report 0x81 is 8 bytes");
  check(output_bits[haptic_command_id] / 8 + 1 == haptic_command_report_size, "haptic command 0x82 is 4 bytes");
  unsigned declared = 0;
  for (const auto &[report, bits] : input_bits) {
    check(report_size(static_cast<std::uint8_t>(report)) == bits / 8 + 1, "input report size table matches the descriptor");
    ++declared;
  }
  for (const auto &[report, bits] : output_bits) {
    check(report_size(static_cast<std::uint8_t>(report)) == bits / 8 + 1, "output report size table matches the descriptor");
    check(is_output_report(static_cast<std::uint8_t>(report)), "every declared output report is in the accepted range");
    ++declared;
  }
  for (const auto &[report, bits] : feature_bits) {
    check(report_size(static_cast<std::uint8_t>(report)) == bits / 8 + 1, "feature report size table matches the descriptor");
    ++declared;
  }
  unsigned tabulated = 0;
  for (unsigned r = 0; r < 256; ++r) if (report_size(static_cast<std::uint8_t>(r)) != 0) ++tabulated;
  check(tabulated == declared, "the size table lists exactly the descriptor's reports");
  check(output_bits.size() == 10 && is_output_report(output_report_last_id) && !is_output_report(0x8a) &&
        !is_output_report(0x7f), "accepted output ids are exactly 0x80..0x89");

  // Input encoding at literal offsets (hid-steam steam_ibex_* tables; confirmed
  // against the capture: timestamp at 30 steps ~3.8 ms, 1 g on accel Z at 38,
  // constant identity quaternion at 46).
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
    check(ule16(b + 7) == 4980 && ule16(b + 13) == 0x76ED, "charging report carries input voltage and temperature");
    check(sizeof(r) == 15, "battery report is 15 bytes");
    st.charge_state = charge_discharging;
    const battery_report d = encode_battery(st);
    check(d.input_voltage_mv == 0 && d.current_ma == 0, "discharging report has no input figures");
  }

  // The driver keeps the state in zero-initialised WDF memory: reset() has to
  // produce the defaults from all-zero contents, not preserve them. (The first
  // test rig returned all-zero attributes and an empty serial because it did.)
  {
    alignas(feature_state) unsigned char raw[sizeof(feature_state)] {};
    auto *fs = reinterpret_cast<feature_state *>(raw);
    fs->reset();
    check(fs->attrs.product_id == product_id && fs->attrs.firmware_build_time == captured_firmware_build_time &&
          fs->attrs.connection_interval_us == 4000, "reset from zeroed memory restores the attribute defaults");
    check(std::strlen(fs->unit_serial.data()) > 0 && std::strlen(fs->board_serial.data()) > 0,
          "reset from zeroed memory restores the serials");
    std::uint8_t cmd[64] {1, cmd_get_attributes_values, 0};
    std::uint8_t reply[64] {};
    check(set_feature(cmd, sizeof(cmd), *fs) && get_feature(reply, sizeof(reply), *fs) == 64 &&
          ule32(reply + 4) == product_id, "attributes after a zeroed-memory reset carry the product id");
  }

  // Control channel: set then get, like the firmware.
  {
    feature_state fs {};
    check(fs.attrs.firmware_build_time == captured_firmware_build_time &&
          fs.attrs.bootloader_build_time == captured_bootloader_build_time &&
          fs.attrs.board_revision == captured_board_revision, "attribute defaults are the captured unit's");
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

    // Get attributes: the real unit's six tags in its order.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_attributes_values; cmd[2] = 0;
    check(set_feature(cmd, sizeof(cmd), fs), "get attributes accepted");
    check(get_feature(reply, sizeof(reply), fs) == 64, "attributes reply");
    check(reply[1] == cmd_get_attributes_values && reply[2] == 30, "attribute reply is 30 bytes like the real unit's");
    const std::uint8_t expected_tags[] = {attr_product_id, attr_capabilities, attr_bootloader_build_time,
                                          attr_firmware_build_time, attr_board_revision, attr_connection_interval_us};
    bool order_ok = true;
    for (unsigned i = 0; i < 6; ++i) order_ok = order_ok && reply[3 + i * 5] == expected_tags[i];
    check(order_ok, "attribute tags come in the captured order");
    check(ule32(reply + 4) == product_id, "product id attribute is 0x1302");
    check(ule32(reply + 4 + 10) == captured_bootloader_build_time, "bootloader build time attribute");
    check(ule32(reply + 4 + 15) == 0x6789ABCD, "firmware build time attribute comes from the state");
    check(ule32(reply + 4 + 25) == 4000, "connection interval attribute is 4000 us");

    // Serial, framed as the unit answers and hid-steam reads: [0xAE][20][tag][chars].
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_string_attribute; cmd[2] = 0x15; cmd[3] = string_attr_unit_serial;
    check(set_feature(cmd, sizeof(cmd), fs), "get serial accepted");
    check(get_feature(reply, sizeof(reply), fs) == 64, "serial reply");
    check(reply[1] == cmd_get_string_attribute && reply[3] == string_attr_unit_serial, "serial tag echoed");
    check(reply[2] == string_attr_reply_length, "serial reply is the fixed 20 bytes");
    check(std::memcmp(reply + 4, fs.unit_serial.data(), std::strlen(fs.unit_serial.data())) == 0, "serial text");
    check(reply[4 + std::strlen(fs.unit_serial.data())] == 0, "serial is NUL padded");
    cmd[3] = string_attr_board_serial;
    check(set_feature(cmd, sizeof(cmd), fs) && get_feature(reply, sizeof(reply), fs) == 64, "get board serial");
    check(reply[3] == string_attr_board_serial &&
          std::memcmp(reply + 4, fs.board_serial.data(), std::strlen(fs.board_serial.data())) == 0, "board serial text");

    // Get settings for explicit ids.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_settings_values; cmd[2] = 2; cmd[3] = setting_imu_mode; cmd[4] = setting_lizard_mode;
    check(set_feature(cmd, sizeof(cmd), fs), "get settings accepted");
    check(get_feature(reply, sizeof(reply), fs) == 64, "settings reply");
    check(reply[2] == 6 && reply[3] == setting_imu_mode && ule16(reply + 4) == 0x18 &&
          reply[6] == setting_lizard_mode && ule16(reply + 7) == 0, "settings triples");

    // Device info, in the shapes the real unit used.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_device_info; cmd[2] = 1; cmd[3] = 0;
    check(set_feature(cmd, sizeof(cmd), fs) && get_feature(reply, sizeof(reply), fs) == 64, "device info 0");
    check(reply[1] == cmd_get_device_info && reply[2] == 41 && reply[3] == 0, "device info 0 is 41 bytes");
    check(ule32(reply + 4) == 0x6789ABCD && ule32(reply + 8) == 0x0C, "device info 0 carries build time and board revision");
    check(std::memcmp(reply + 28, fs.unit_serial.data(), std::strlen(fs.unit_serial.data())) == 0, "device info 0 ends with the serial");
    cmd[3] = 1;
    check(set_feature(cmd, sizeof(cmd), fs) && get_feature(reply, sizeof(reply), fs) == 64 && reply[2] == 34 && reply[3] == 1,
          "device info 1 is 34 bytes");
    cmd[3] = 2;
    check(set_feature(cmd, sizeof(cmd), fs) && get_feature(reply, sizeof(reply), fs) == 64 && reply[2] == 9 && reply[3] == 2 &&
          reply[6] == 1, "device info 2 is 9 bytes");

    // Keyed values: no bond, nothing else.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_get_keyed_value; cmd[2] = 9; std::memcpy(cmd + 3, "esb/bond", 9);
    check(set_feature(cmd, sizeof(cmd), fs) && get_feature(reply, sizeof(reply), fs) == 64 && reply[2] == 1 && reply[3] == 0,
          "esb/bond answers one zero byte");
    cmd[2] = 24; std::memcpy(cmd + 3, "user/wireless_transport", 24);
    check(set_feature(cmd, sizeof(cmd), fs) && get_feature(reply, sizeof(reply), fs) == 64 && reply[2] == 0,
          "other keys answer empty");

    // The three write-only commands are known, not counted as unknown.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = cmd_write_c1; cmd[2] = 16;
    check(set_feature(cmd, sizeof(cmd), fs), "0xC1 accepted");
    cmd[1] = cmd_write_dc; cmd[2] = 2; check(set_feature(cmd, sizeof(cmd), fs), "0xDC accepted");
    cmd[1] = cmd_write_e2; cmd[2] = 2; check(set_feature(cmd, sizeof(cmd), fs), "0xE2 accepted");
    check(fs.unknown_commands == 0, "captured commands are not unknown");

    // Unknown command is acknowledged, counted, not refused.
    std::memset(cmd, 0, sizeof(cmd));
    cmd[0] = 1; cmd[1] = 0xC7; cmd[2] = 0;
    check(set_feature(cmd, sizeof(cmd), fs), "unknown command acknowledged");
    check(fs.unknown_commands == 1 && fs.last_unknown_command == 0xC7, "unknown command recorded");
    check(get_feature(reply, sizeof(reply), fs) == 64 && reply[1] == 0xC7 && reply[2] == 0, "unknown reply is empty");

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

  // Replay of the commands Steam sent the author's unit on connect
  // (captures/sc26-steam-handshake.txt, transfers 0..18 and the later 0xC1/0xDC/0xE2/0xED).
  // Every one must be accepted and none may be unknown; the replies Steam read
  // must have the real unit's lengths.
  {
    feature_state fs {};
    std::uint8_t reply[64] {};
    const struct { const char *hex; unsigned reply_len; } steam[] = {
      {"018300", 30},                                               // GET_ATTRIBUTES_VALUES
      {"01ae15010213000002000000000a2ef9d26804e3854d6a09", 20},     // GET_STRING_ATTRIBUTE unit serial (stale buffer tail)
      {"018703328403", 0},                                          // SET_SETTINGS 50 = 900
      {"018703090000", 0},                                          // lizard mode off
      {"01ae150000000000009fbeeea6fa7f0000f001bd04e70100", 20},     // GET_STRING_ATTRIBUTE board serial
      {"018100", 0},                                                // CLEAR_DIGITAL_MAPPINGS
      {"01870f301800070700080700310200520300", 0},                  // IMU mode 0x18, 7=7, 8=7, 49=2, 82=3
      {"01870f1800002e000034ffff35ffff2e0000", 0},
      {"01870634ffff35ffff", 0},
      {"01f20100", 41},                                             // GET_DEVICE_INFO 0
      {"01f20101", 34},
      {"01f20102", 9},
      {"0187032d6400", 0},                                          // 45 = 100
      {"01c110ffffffff030905ffffffffffffffffff", 0},
      {"01dc020102", 0},
      {"01e2020120", 0},
      {"01ed096573622f626f6e6400", 1},                              // "esb/bond"
      {"01ed0b6573622f626f6e645f3200", 0},                          // "esb/bond_2"
      {"01ed18757365722f776972656c6573735f7472616e73706f727400", 0}, // "user/wireless_transport"
      {"018500", 0},                                                // SET_DEFAULT_DIGITAL_MAPPINGS
      {"018e00", 0},                                                // LOAD_DEFAULT_SETTINGS
    };
    for (const auto &step : steam) {
      const std::vector<std::uint8_t> cmd = from_hex(step.hex);
      check(set_feature(cmd.data(), cmd.size(), fs), "captured Steam command accepted");
      check(get_feature(reply, sizeof(reply), fs) == 64 && reply[1] == cmd[1], "reply echoes the command type");
      check(reply[2] == step.reply_len, "reply length matches the real unit's");
      if (std::strcmp(step.hex, "0187032d6400") == 0) {
        check(!fs.lizard_mode && fs.imu_mode == 0x18 && fs.settings[50] == 900 && fs.settings[45] == 100,
              "Steam's settings landed");
      }
    }
    check(fs.unknown_commands == 0, "nothing Steam sent is unknown");
    check(fs.lizard_mode && fs.imu_mode == 0, "the closing LOAD_DEFAULT_SETTINGS reset them");
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
    std::uint8_t click[8] {haptic_pulse_id, 1, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00};  // Steam's UI click as captured
    check(decode_haptic_output(click, sizeof(click), r) && r.left == 65535, "captured 400 us single pulse is full magnitude");
    std::uint8_t stop[8] {haptic_pulse_id, 1, 0, 0, 0, 0, 0, 0};
    check(decode_haptic_output(stop, sizeof(stop), r) && r.left == 0, "zero-repeat pulse stops the left pad");
    std::uint8_t command[4] {haptic_command_id, 0, 0x02, 0xf2};  // as captured after each click
    check(!decode_haptic_output(command, sizeof(command), r) && is_output_report(command[0]),
          "haptic command is accepted by the driver but renders nothing");
    std::uint8_t other[10] {haptic_lfo_id};
    check(!decode_haptic_output(other, sizeof(other), r), "LFO report is not rumble");
    check(!decode_haptic_output(out, 5, r), "short rumble report refused");
  }

  if (failures == 0) std::printf("PASS: sc26 usb contract\n");
  return failures == 0 ? 0 : 1;
}
