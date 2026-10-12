// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// A malformed HID report descriptor does not fail loudly: VhfCreate still
// succeeds and the HID child simply never starts, which looks like a driver
// bug from the host side. This walks the descriptor the way HIDCLASS does and
// proves that every report's bit layout matches the packed struct the driver
// memcpys into, so that class of error is caught at build time instead of on a
// test machine.
//
// Build and run standalone:
//   g++ -std=c++20 -I driver/src -I include
//     driver/tests/test_pid_descriptor.cpp driver/src/pid_ff.cpp -o pid_test

#include "dualsense.h"
#include "dualshock4.h"
#include "pid_ff.h"
#include "profile.h"
#include "report_pump.h"
#include "switch_pro.h"
#include "xbox_one.h"
#include "xbox_series.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(const bool condition, const std::string &what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

// Existing ordering tests inspect a completed copy. Lifetime tests below keep
// the actual submission pointer, as VHF does with custom buffering enabled.
bool take_report(lvg::driver::report_pump &pump, lvg::driver::report_buffer &out) {
  const auto *const report = pump.take();
  if (report == nullptr) {
    return false;
  }
  out = *report;
  return true;
}

struct report_bits_t {
  std::size_t input = 0;
  std::size_t output = 0;
  std::size_t feature = 0;
};

struct walk_result_t {
  bool well_formed = true;
  std::string error;
  int collection_depth = 0;
  int max_collection_depth = 0;
  std::map<std::uint8_t, report_bits_t> reports;
  std::size_t item_count = 0;
  bool saw_pid_usage_page = false;
  int top_level_collections = 0;
};

// Walks short HID items. The descriptor deliberately contains no long items.
walk_result_t walk(const std::uint8_t *data, const std::size_t size) {
  walk_result_t result;

  std::size_t report_size = 0;
  std::size_t report_count = 0;
  std::uint8_t report_id = 0;
  std::uint32_t usage_page = 0;

  std::size_t offset = 0;
  while (offset < size) {
    const std::uint8_t prefix = data[offset];
    if (prefix == 0xFE) {
      result.well_formed = false;
      result.error = "long items are not supported by this descriptor";
      return result;
    }

    const std::uint8_t tag = static_cast<std::uint8_t>((prefix >> 4) & 0x0F);
    const std::uint8_t type = static_cast<std::uint8_t>((prefix >> 2) & 0x03);
    const std::uint8_t raw_size = static_cast<std::uint8_t>(prefix & 0x03);
    const std::size_t data_size = raw_size == 3 ? 4 : raw_size;

    if (offset + 1 + data_size > size) {
      result.well_formed = false;
      result.error = "item at offset " + std::to_string(offset) + " runs past the end";
      return result;
    }

    std::uint32_t value = 0;
    for (std::size_t i = 0; i < data_size; ++i) {
      value |= static_cast<std::uint32_t>(data[offset + 1 + i]) << (8 * i);
    }

    ++result.item_count;

    if (type == 0) {  // Main
      switch (tag) {
        case 0x8:  // Input
        case 0x9:  // Output
        case 0xB: {  // Feature
          if (report_size == 0 || report_count == 0) {
            result.well_formed = false;
            result.error = "main item at offset " + std::to_string(offset) +
                           " has no report size/count";
            return result;
          }
          if (result.collection_depth == 0) {
            result.well_formed = false;
            result.error = "main item at offset " + std::to_string(offset) +
                           " is outside any collection";
            return result;
          }
          const std::size_t bits = report_size * report_count;
          auto &bucket = result.reports[report_id];
          if (tag == 0x8) {
            bucket.input += bits;
          } else if (tag == 0x9) {
            bucket.output += bits;
          } else {
            bucket.feature += bits;
          }
          break;
        }
        case 0xA:  // Collection
          if (result.collection_depth == 0) {
            ++result.top_level_collections;
          }
          ++result.collection_depth;
          if (result.collection_depth > result.max_collection_depth) {
            result.max_collection_depth = result.collection_depth;
          }
          break;
        case 0xC:  // End Collection
          --result.collection_depth;
          if (result.collection_depth < 0) {
            result.well_formed = false;
            result.error = "unbalanced End Collection at offset " + std::to_string(offset);
            return result;
          }
          break;
        default:
          break;
      }
    } else if (type == 1) {  // Global
      switch (tag) {
        case 0x0:
          usage_page = value;
          if (usage_page == 0x0F) {
            result.saw_pid_usage_page = true;
          }
          break;
        case 0x7:
          report_size = value;
          break;
        case 0x8:
          report_id = static_cast<std::uint8_t>(value);
          break;
        case 0x9:
          report_count = value;
          break;
        case 0xA:
        case 0xB:
          result.well_formed = false;
          result.error = "push/pop is not modelled by this walker";
          return result;
        default:
          break;
      }
    }

    offset += 1 + data_size;
  }

  return result;
}

}  // namespace

int main() {
  using namespace lvg;
  using namespace lvg::driver;

  std::size_t descriptor_size = 0;
  const std::uint8_t *const descriptor = pid_gamepad_descriptor(&descriptor_size);
  check(descriptor != nullptr && descriptor_size > 0, "descriptor is present");

  const walk_result_t parsed = walk(descriptor, descriptor_size);
  check(parsed.well_formed, "descriptor parses: " + parsed.error);
  if (!parsed.well_formed) {
    std::printf("descriptor walk aborted\n");
    return 1;
  }

  check(parsed.collection_depth == 0, "collections balance");
  check(parsed.top_level_collections == 2, "two top-level application collections");
  check(parsed.saw_pid_usage_page, "PID usage page is present");
  std::printf("descriptor: %zu bytes, %zu items, %zu report ids, max depth %d\n",
              descriptor_size, parsed.item_count, parsed.reports.size(),
              parsed.max_collection_depth);

  // Every report the descriptor declares must match the expected bit budget.
  std::size_t expected_count = 0;
  const pid_report_bits_t *const expected = pid_report_bits(&expected_count);
  check(parsed.reports.size() == expected_count, "descriptor declares the expected report count");

  for (std::size_t i = 0; i < expected_count; ++i) {
    const pid_report_bits_t &want = expected[i];
    const auto found = parsed.reports.find(want.report_id);
    const std::string id = std::to_string(want.report_id);
    if (found == parsed.reports.end()) {
      check(false, "report " + id + " exists in the descriptor");
      continue;
    }
    check(found->second.input == want.input_bits,
          "report " + id + " input bits: got " + std::to_string(found->second.input) +
            " want " + std::to_string(want.input_bits));
    check(found->second.output == want.output_bits,
          "report " + id + " output bits: got " + std::to_string(found->second.output) +
            " want " + std::to_string(want.output_bits));
    check(found->second.feature == want.feature_bits,
          "report " + id + " feature bits: got " + std::to_string(found->second.feature) +
            " want " + std::to_string(want.feature_bits));

    const std::size_t total = want.input_bits + want.output_bits + want.feature_bits;
    check(total % 8 == 0, "report " + id + " is a whole number of bytes");
  }

  // Each parsed report must also match the packed struct the driver uses.
  const auto expect_struct = [&](const std::uint8_t report_id,
                                 const std::size_t struct_size,
                                 const char *name) {
    const auto found = parsed.reports.find(report_id);
    if (found == parsed.reports.end()) {
      check(false, std::string(name) + ": report id missing");
      return;
    }
    const std::size_t bits =
      found->second.input + found->second.output + found->second.feature;
    const std::size_t bytes = bits / 8 + 1;  // +1 for the report ID byte.
    check(bytes == struct_size,
          std::string(name) + ": descriptor says " + std::to_string(bytes) +
            " bytes, struct is " + std::to_string(struct_size));
  };

  expect_struct(k_pid_state_report_id, sizeof(pid_state_report), "pid_state_report");
  expect_struct(k_pid_set_effect_report_id, sizeof(pid_set_effect_report), "pid_set_effect_report");
  expect_struct(k_pid_set_envelope_report_id, sizeof(pid_set_envelope_report), "pid_set_envelope_report");
  expect_struct(k_pid_set_condition_report_id, sizeof(pid_set_condition_report), "pid_set_condition_report");
  expect_struct(k_pid_set_periodic_report_id, sizeof(pid_set_periodic_report), "pid_set_periodic_report");
  expect_struct(k_pid_set_constant_force_report_id, sizeof(pid_set_constant_force_report), "pid_set_constant_force_report");
  expect_struct(k_pid_set_ramp_force_report_id, sizeof(pid_set_ramp_force_report), "pid_set_ramp_force_report");
  expect_struct(k_pid_effect_operation_report_id, sizeof(pid_effect_operation_report), "pid_effect_operation_report");
  expect_struct(k_pid_block_free_report_id, sizeof(pid_block_free_report), "pid_block_free_report");
  expect_struct(k_pid_device_control_report_id, sizeof(pid_device_control_report), "pid_device_control_report");
  expect_struct(k_pid_device_gain_report_id, sizeof(pid_device_gain_report), "pid_device_gain_report");
  expect_struct(k_pid_create_new_effect_report_id, sizeof(pid_create_new_effect_report), "pid_create_new_effect_report");
  expect_struct(k_pid_block_load_report_id, sizeof(pid_block_load_report), "pid_block_load_report");
  expect_struct(k_pid_pool_report_id, sizeof(pid_pool_report), "pid_pool_report");

  // ---- Effect engine behavior ----
  {
    pid_engine engine;

    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    check(engine.create_new_effect(create), "constant force block allocates");

    const pid_block_load_report load = engine.block_load();
    check(load.effect_block_index == 1, "first block is index 1");
    check(load.block_load_status == static_cast<std::uint8_t>(pid_block_load_status::success),
          "first block loads successfully");
    check(load.ram_pool_available < engine.pool().ram_pool_size, "pool accounts for the block");

    pid_set_effect_report set_effect {};
    set_effect.report_id = k_pid_set_effect_report_id;
    set_effect.effect_block_index = 1;
    set_effect.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    set_effect.duration_ms = 100;
    set_effect.gain = 255;
    check(engine.set_effect(set_effect), "set effect accepted");

    pid_set_constant_force_report constant {};
    constant.report_id = k_pid_set_constant_force_report_id;
    constant.effect_block_index = 1;
    constant.magnitude = k_pid_nominal_max;
    check(engine.set_constant_force(constant), "constant force accepted");

    check(engine.rumble().low_frequency == 0, "idle before start");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "start accepted");

    check(engine.rumble().low_frequency == 65535, "full constant force drives the low motor");
    check(engine.rumble().high_frequency == 0, "constant force leaves the high motor alone");
    check(engine.needs_tick(), "a playing effect needs the clock");

    // A negative magnitude is the same strength in the other direction.
    constant.magnitude = static_cast<std::int16_t>(-k_pid_nominal_max);
    check(engine.set_constant_force(constant), "negative constant force accepted");
    check(engine.rumble().low_frequency == 65535, "direction does not change rumble strength");

    // Finite effects must stop themselves.
    engine.advance(150);
    check(!engine.needs_tick(), "finite effect stops on its own");
    check(engine.rumble().low_frequency == 0, "expired effect leaves the motors off");
  }

  {
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::sine);
    check(engine.create_new_effect(create), "sine block allocates");

    pid_set_periodic_report periodic {};
    periodic.report_id = k_pid_set_periodic_report_id;
    periodic.effect_block_index = 1;
    periodic.magnitude = k_pid_nominal_max / 2;
    check(engine.set_periodic(periodic), "periodic accepted");

    pid_set_effect_report set_effect {};
    set_effect.report_id = k_pid_set_effect_report_id;
    set_effect.effect_block_index = 1;
    set_effect.effect_type = static_cast<std::uint8_t>(pid_effect_type::sine);
    set_effect.duration_ms = 0;  // Infinite.
    set_effect.gain = 255;
    check(engine.set_effect(set_effect), "infinite periodic accepted");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "periodic start accepted");

    const pid_rumble_t rumble = engine.rumble();
    check(rumble.high_frequency > 32000 && rumble.high_frequency < 33500,
          "half-magnitude sine drives the high motor at about half");
    check(rumble.low_frequency == 0, "periodic leaves the low motor alone");

    engine.advance(100000);
    check(engine.needs_tick(), "infinite effect keeps playing");

    pid_device_control_report stop {};
    stop.report_id = k_pid_device_control_report_id;
    stop.control = static_cast<std::uint8_t>(pid_device_control::stop_all_effects);
    check(engine.device_control(stop), "stop all accepted");
    check(engine.rumble().high_frequency == 0, "stop all silences the motors");
  }

  {
    // Condition effects must be accepted without inventing vibration.
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::spring);
    check(engine.create_new_effect(create), "spring block allocates");

    pid_set_condition_report condition {};
    condition.report_id = k_pid_set_condition_report_id;
    condition.effect_block_index = 1;
    condition.positive_coefficient = k_pid_nominal_max;
    condition.negative_coefficient = static_cast<std::int16_t>(-k_pid_nominal_max);
    check(engine.set_condition(condition), "condition accepted");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "spring start accepted");
    check(engine.rumble().low_frequency == 0 && engine.rumble().high_frequency == 0,
          "spring produces no rumble");
  }

  {
    // The pool must refuse gracefully rather than overrun.
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    for (int i = 0; i < k_pid_max_effects; ++i) {
      check(engine.create_new_effect(create), "block " + std::to_string(i + 1) + " allocates");
    }
    check(!engine.create_new_effect(create), "the pool refuses one block too many");
    check(engine.block_load().block_load_status ==
            static_cast<std::uint8_t>(pid_block_load_status::full),
          "a refused allocation reports Block Load Full");

    pid_block_free_report free_block {};
    free_block.report_id = k_pid_block_free_report_id;
    free_block.effect_block_index = 1;
    check(engine.block_free(free_block), "freeing a block succeeds");
    check(engine.create_new_effect(create), "a freed block can be reallocated");
  }

  {
    // Device gain scales everything the host asks for.
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    check(engine.create_new_effect(create), "gain test block allocates");

    pid_set_effect_report set_effect {};
    set_effect.report_id = k_pid_set_effect_report_id;
    set_effect.effect_block_index = 1;
    set_effect.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    set_effect.gain = 255;
    check(engine.set_effect(set_effect), "gain test effect accepted");

    pid_set_constant_force_report constant {};
    constant.report_id = k_pid_set_constant_force_report_id;
    constant.effect_block_index = 1;
    constant.magnitude = k_pid_nominal_max;
    check(engine.set_constant_force(constant), "gain test force accepted");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "gain test start accepted");

    pid_device_gain_report gain {};
    gain.report_id = k_pid_device_gain_report_id;
    gain.device_gain = 128;
    check(engine.device_gain(gain), "device gain accepted");

    const std::uint16_t halved = engine.rumble().low_frequency;
    check(halved > 32000 && halved < 33500, "device gain of 128 halves the rumble");

    pid_device_control_report disable {};
    disable.report_id = k_pid_device_control_report_id;
    disable.control = static_cast<std::uint8_t>(pid_device_control::disable_actuators);
    check(engine.device_control(disable), "disable actuators accepted");
    check(engine.rumble().low_frequency == 0, "disabled actuators produce no rumble");
  }

  // ---- Xbox Series profile ----
  {
    std::size_t xbox_size = 0;
    const std::uint8_t *const xbox = xbox_series_descriptor(&xbox_size);
    const walk_result_t xbox_parsed = walk(xbox, xbox_size);
    check(xbox_parsed.well_formed, "xbox descriptor parses: " + xbox_parsed.error);
    if (xbox_parsed.well_formed) {
      check(xbox_parsed.collection_depth == 0, "xbox collections balance");
      check(xbox_parsed.top_level_collections == 1, "xbox has one application collection");
      std::printf("xbox descriptor: %zu bytes, %zu items, %zu report ids\n",
                  xbox_size, xbox_parsed.item_count, xbox_parsed.reports.size());

      const auto input = xbox_parsed.reports.find(k_xbox_series_input_report_id);
      if (input == xbox_parsed.reports.end()) {
        check(false, "xbox input report exists");
      } else {
        check(input->second.input == 128,
              "xbox input report is 128 bits, got " + std::to_string(input->second.input));
        check(input->second.input / 8 + 1 == sizeof(xbox_series_input_report),
              "xbox input report matches its struct");
      }

      const auto output = xbox_parsed.reports.find(k_xbox_series_output_report_id);
      if (output == xbox_parsed.reports.end()) {
        check(false, "xbox output report exists");
      } else {
        check(output->second.output == 64,
              "xbox output report is 64 bits, got " + std::to_string(output->second.output));
        check(output->second.output / 8 + 1 == sizeof(xbox_series_output_report),
              "xbox output report matches its struct");
      }
    }

    // The hardware ID list is what puts the child on the XInput path, so its
    // REG_MULTI_SZ shape has to be right or PnP sees nothing.
    std::size_t id_bytes = 0;
    const wchar_t *const ids = xbox_series_hardware_ids(&id_bytes);
    check(ids != nullptr && id_bytes > 0, "hardware ids are present");
    const std::size_t id_chars = id_bytes / sizeof(wchar_t);
    check(id_chars >= 2 && ids[id_chars - 1] == L'\0' && ids[id_chars - 2] == L'\0',
          "hardware ids are double-null terminated");
    int id_count = 0;
    for (std::size_t i = 0; i + 1 < id_chars;) {
      if (ids[i] == L'\0') {
        break;
      }
      ++id_count;
      while (i < id_chars && ids[i] != L'\0') {
        ++i;
      }
      ++i;
    }
    check(id_count == 2, "two hardware ids are offered, got " + std::to_string(id_count));
  }

  {
    // Axis, trigger, hat and button translation.
    input_state_request state {};
    state.header.size = sizeof(state);
    state.header.version = k_protocol_version;

    state.left_x = 0;
    state.left_y = 0;
    state.right_x = 0;
    state.right_y = 0;
    xbox_series_input_report report = encode_xbox_series_input(state);
    check(report.report_id == k_xbox_series_input_report_id, "xbox report id");
    check(report.left_x == 32768, "centred X maps to mid scale");
    check(report.left_y == 32767, "centred Y maps to mid scale");
    check(report.hat == 0, "a centred d-pad reports the hat null value");
    check(report.buttons == 0, "no buttons pressed");

    // Vibeshine is positive-up; this device is positive-down.
    state.left_y = 32767;
    report = encode_xbox_series_input(state);
    check(report.left_y == 0, "stick fully up maps to 0");
    state.left_y = -32768;
    report = encode_xbox_series_input(state);
    check(report.left_y == 65535, "stick fully down maps to full scale");

    state.left_y = 0;
    state.left_trigger = 255;
    state.right_trigger = 0;
    report = encode_xbox_series_input(state);
    check(report.left_trigger == k_xbox_trigger_max, "full trigger maps to 10-bit maximum");
    check(report.right_trigger == 0, "released trigger is zero");

    state.left_trigger = 0;
    state.buttons = button_mask::dpad_up;
    check(encode_xbox_series_input(state).hat == 1, "up is hat 1");
    state.buttons = button_mask::dpad_up | button_mask::dpad_right;
    check(encode_xbox_series_input(state).hat == 2, "up-right is hat 2");
    state.buttons = button_mask::dpad_left;
    check(encode_xbox_series_input(state).hat == 7, "left is hat 7");
    state.buttons = button_mask::dpad_up | button_mask::dpad_down;
    check(encode_xbox_series_input(state).hat == 0, "opposing d-pad reports the null value");

    state.buttons = button_mask::south | button_mask::east | button_mask::west |
                    button_mask::north;
    report = encode_xbox_series_input(state);
    check(report.buttons == (xbox_a | xbox_b | xbox_x | xbox_y), "face buttons map in HID order");
    // Buttons 3, 6, 9 and 10 are unpopulated on real hardware.
    check((report.buttons & ((1u << 2) | (1u << 5) | (1u << 8) | (1u << 9))) == 0,
          "the vendor's unpopulated button gaps stay empty");

    state.buttons = button_mask::home;
    check(encode_xbox_series_input(state).buttons == xbox_guide, "home maps to Guide");
    state.buttons = button_mask::misc;
    check(encode_xbox_series_input(state).share == 1, "misc maps to Share");
    state.buttons = 0;
    check(encode_xbox_series_input(state).share == 0, "Share is released");
  }

  {
    // Rumble decode: the enable mask gates each actuator.
    xbox_series_output_report output {};
    output.report_id = k_xbox_series_output_report_id;
    output.enable_mask = k_xbox_enable_left_motor | k_xbox_enable_right_motor |
                         k_xbox_enable_left_trigger | k_xbox_enable_right_trigger;
    output.left_motor = k_xbox_rumble_max;
    output.right_motor = k_xbox_rumble_max / 2;
    output.left_trigger_motor = k_xbox_rumble_max;
    output.right_trigger_motor = 0;

    lvg::xbox_rumble_feedback rumble {};
    check(decode_xbox_series_output(output, &rumble), "rumble write decodes");
    check(rumble.low_frequency == 65535, "full left motor is full scale");
    check(rumble.high_frequency > 32000 && rumble.high_frequency < 33500,
          "half right motor is about half scale");
    check(rumble.left_trigger == 65535, "full left trigger motor is full scale");
    check(rumble.right_trigger == 0, "idle right trigger motor is zero");

    output.enable_mask = k_xbox_enable_left_motor;
    check(decode_xbox_series_output(output, &rumble), "masked rumble write decodes");
    check(rumble.low_frequency == 65535, "the enabled actuator still reports");
    check(rumble.high_frequency == 0 && rumble.left_trigger == 0,
          "actuators the mask disables report silence");

    output.report_id = 0x7F;
    check(!decode_xbox_series_output(output, &rumble), "a foreign report id is rejected");

    const feedback_event event = encode_xbox_series_feedback(5, rumble);
    check(event.type == feedback_type::xbox_rumble, "feedback is tagged xbox_rumble");
    check(event.controller_id == 5, "feedback carries the controller id");
    check(event.payload_size == sizeof(lvg::xbox_rumble_feedback), "feedback payload size");
    check(valid_request(&event, sizeof(event)), "feedback event is a valid protocol message");
  }


  // ---- PlayStation profiles ----
  {
    struct ps_case {
      const char *name;
      const std::uint8_t *descriptor;
      std::size_t size;
      std::uint8_t input_id;
      std::size_t input_bytes;
      std::uint8_t output_id;
      std::size_t output_bytes;
    };

    std::size_t ds4_size = 0;
    std::size_t ds5_size = 0;
    const std::uint8_t *const ds4 = ds4_descriptor(&ds4_size);
    const std::uint8_t *const ds5 = ds5_descriptor(&ds5_size);

    const ps_case cases[] = {
      {"ds4", ds4, ds4_size, k_ds4_input_report_id, sizeof(ds4_input_report),
       k_ds4_output_report_id, sizeof(ds4_output_report)},
      {"ds5", ds5, ds5_size, k_ds5_input_report_id, sizeof(ds5_input_report),
       k_ds5_output_report_id, sizeof(ds5_output_report)},
    };

    for (const ps_case &c : cases) {
      const walk_result_t parsed_ps = walk(c.descriptor, c.size);
      const std::string tag = std::string(c.name) + ": ";
      check(parsed_ps.well_formed, tag + "descriptor parses: " + parsed_ps.error);
      if (!parsed_ps.well_formed) {
        continue;
      }
      check(parsed_ps.collection_depth == 0, tag + "collections balance");
      std::printf("%s descriptor: %zu bytes, %zu items, %zu report ids\n",
                  c.name, c.size, parsed_ps.item_count, parsed_ps.reports.size());

      const auto in = parsed_ps.reports.find(c.input_id);
      if (in == parsed_ps.reports.end()) {
        check(false, tag + "input report present");
      } else {
        check(in->second.input / 8 + 1 == c.input_bytes,
              tag + "input report is " + std::to_string(c.input_bytes) + " bytes, descriptor says " +
                std::to_string(in->second.input / 8 + 1));
      }

      const auto out = parsed_ps.reports.find(c.output_id);
      if (out == parsed_ps.reports.end()) {
        check(false, tag + "output report present");
      } else {
        check(out->second.output / 8 + 1 == c.output_bytes,
              tag + "output report is " + std::to_string(c.output_bytes) + " bytes, descriptor says " +
                std::to_string(out->second.output / 8 + 1));
      }
    }
  }

  {
    // DualShock 4 input translation.
    ds4_state state {};
    state.reset();

    input_state_request input {};
    input.header.size = sizeof(input);
    input.header.version = k_protocol_version;

    ds4_input_report report = encode_ds4_input(input, &state);
    check(report.report_id == k_ds4_input_report_id, "ds4 report id");
    check(report.left_x == 128, "ds4 centred X");
    check(report.left_y == 128, "ds4 centred Y");
    check((report.buttons0 & 0x0F) == 8, "ds4 released d-pad is hat 8");

    input.left_y = 32767;
    check(encode_ds4_input(input, &state).left_y == 0, "ds4 stick up maps to 0");
    input.left_y = -32768;
    check(encode_ds4_input(input, &state).left_y == 255, "ds4 stick down maps to 255");
    input.left_y = 0;

    input.buttons = button_mask::south;
    check((encode_ds4_input(input, &state).buttons0 & 0x20) != 0, "ds4 south is Cross");
    input.buttons = button_mask::west;
    check((encode_ds4_input(input, &state).buttons0 & 0x10) != 0, "ds4 west is Square");
    input.buttons = button_mask::north;
    check((encode_ds4_input(input, &state).buttons0 & 0x80) != 0, "ds4 north is Triangle");
    input.buttons = button_mask::east;
    check((encode_ds4_input(input, &state).buttons0 & 0x40) != 0, "ds4 east is Circle");
    input.buttons = button_mask::dpad_up;
    check((encode_ds4_input(input, &state).buttons0 & 0x0F) == 0, "ds4 up is hat 0");
    input.buttons = button_mask::home;
    check((encode_ds4_input(input, &state).buttons2 & 0x01) != 0, "ds4 home is PS");
    input.buttons = button_mask::touchpad;
    check((encode_ds4_input(input, &state).buttons2 & 0x02) != 0, "ds4 touchpad click");
    input.buttons = 0;

    // A pressed trigger also sets the device's digital bit.
    input.left_trigger = 200;
    report = encode_ds4_input(input, &state);
    check(report.left_trigger == 200, "ds4 analog trigger");
    check((report.buttons1 & 0x04) != 0, "ds4 analog trigger sets its digital bit");
    input.left_trigger = 0;

    // The report counter has to advance or consumers treat the device as stalled.
    const std::uint8_t first = static_cast<std::uint8_t>(encode_ds4_input(input, &state).buttons2 >> 2);
    const std::uint8_t second = static_cast<std::uint8_t>(encode_ds4_input(input, &state).buttons2 >> 2);
    check(first != second, "ds4 report counter advances");
  }

  {
    // DualShock 4 touch, motion, and battery folding.
    ds4_state state {};
    state.reset();

    input_state_request input {};
    input.header.size = sizeof(input);
    input.header.version = k_protocol_version;

    touch_state_request touch {};
    touch.header.size = sizeof(touch);
    touch.header.version = k_protocol_version;
    touch.contact_index = 0;
    touch.event_type = static_cast<std::uint8_t>(touch_event::down);
    touch.x = 65535;
    touch.y = 0;
    check(apply_ds4_touch(touch, &state), "ds4 touch down accepted");

    ds4_input_report report = encode_ds4_input(input, &state);
    check((report.touch[0].points[0].tracking_id & 0x80) == 0, "ds4 contact reads as active");
    const std::uint16_t x = static_cast<std::uint16_t>(
      report.touch[0].points[0].coordinates[0] |
      ((report.touch[0].points[0].coordinates[1] & 0x0F) << 8));
    check(x == k_ds4_touch_width - 1, "ds4 touch x maps to the pad width, got " + std::to_string(x));

    touch.event_type = static_cast<std::uint8_t>(touch_event::up);
    check(apply_ds4_touch(touch, &state), "ds4 touch up accepted");
    report = encode_ds4_input(input, &state);
    check((report.touch[0].points[0].tracking_id & 0x80) != 0, "ds4 released contact reads inactive");

    // A third contact has nowhere to go on a two-contact pad.
    touch.contact_index = 2;
    touch.event_type = static_cast<std::uint8_t>(touch_event::down);
    check(!apply_ds4_touch(touch, &state), "ds4 rejects a third contact");

    motion_state_request motion {};
    motion.header.size = sizeof(motion);
    motion.header.version = k_protocol_version;
    motion.motion_type = static_cast<std::uint8_t>(motion_kind::accelerometer);
    motion.y_milli = k_milli_g;
    check(apply_ds4_motion(motion, &state), "ds4 accelerometer accepted");
    report = encode_ds4_input(input, &state);
    check(report.accel_y == k_ds4_accel_counts_per_g,
          "ds4 one gravity is one G of counts, got " + std::to_string(report.accel_y));

    motion.motion_type = static_cast<std::uint8_t>(motion_kind::gyroscope);
    motion.x_milli = 1000;  // 1 degree/second
    motion.y_milli = 0;
    check(apply_ds4_motion(motion, &state), "ds4 gyroscope accepted");
    report = encode_ds4_input(input, &state);
    check(report.gyro_x == k_ds4_gyro_counts_per_dps, "ds4 gyro scaling");

    battery_state_request battery {};
    battery.header.size = sizeof(battery);
    battery.header.version = k_protocol_version;
    battery.percent = 100;
    battery.flags = static_cast<std::uint8_t>(lvg::battery_state::discharging);
    check(apply_ds4_battery(battery, &state), "ds4 battery accepted");
    report = encode_ds4_input(input, &state);
    check(report.battery == 10, "ds4 full battery on battery power is 10");
    check((report.battery_status & 0x10) == 0, "ds4 discharging is not cable powered");

    battery.flags = static_cast<std::uint8_t>(lvg::battery_state::charging);
    check(apply_ds4_battery(battery, &state), "ds4 charging accepted");
    report = encode_ds4_input(input, &state);
    check((report.battery_status & 0x10) != 0, "ds4 charging sets the cable bit");
  }

  {
    // DualShock 4 output: rumble and lightbar.
    ds4_output_report output {};
    output.report_id = k_ds4_output_report_id;
    output.flags = 0x03;
    output.left_rumble = 0xFF;
    output.right_rumble = 0x80;
    output.red = 0x11;
    output.green = 0x22;
    output.blue = 0x33;

    playstation_output_feedback feedback {};
    check(decode_ds4_output(output, &feedback), "ds4 output decodes");
    check(feedback.low_frequency == 0xFF00, "ds4 left motor widens to 16 bits");
    check(feedback.high_frequency == 0x8000, "ds4 right motor widens to 16 bits");
    check(feedback.red == 0x11 && feedback.green == 0x22 && feedback.blue == 0x33, "ds4 lightbar");
    check((feedback.valid & ps_output_lightbar_valid) != 0, "ds4 lightbar marked valid");

    // A report that does not claim the lightbar must not recolour it.
    output.flags = 0x01;
    check(decode_ds4_output(output, &feedback), "ds4 rumble-only output decodes");
    check((feedback.valid & ps_output_lightbar_valid) == 0, "ds4 lightbar not claimed");

    output.report_id = 0x7E;
    check(!decode_ds4_output(output, &feedback), "ds4 rejects a foreign report id");

    std::uint8_t buffer[64] {};
    check(fill_ds4_feature(k_ds4_feature_calibration_id, buffer, sizeof(buffer)) == 37,
          "ds4 calibration feature is 37 bytes");
    check(buffer[0] == k_ds4_feature_calibration_id, "ds4 calibration carries its report id");
    check(fill_ds4_feature(k_ds4_feature_firmware_id, buffer, sizeof(buffer)) == 49,
          "ds4 firmware feature is 49 bytes");
    check(fill_ds4_feature(k_ds4_feature_pairing_id, buffer, sizeof(buffer)) == 16,
          "ds4 pairing feature is 16 bytes");
    // Locally administered, so it cannot collide with a real Sony address.
    check((buffer[6] & 0x02) != 0, "ds4 MAC is locally administered");
    check(buffer[7] == 0x08 && buffer[8] == 0x25 && buffer[9] == 0x00,
          "ds4 pairing carries libScePad MAC-report magic");
    check(fill_ds4_feature(0x7C, buffer, sizeof(buffer)) == 0, "ds4 ignores unknown features");
    check(fill_ds4_feature(k_ds4_feature_calibration_id, buffer, 4) == 0,
          "ds4 refuses a short feature buffer");
  }

  {
    // DualSense: the pieces that differ from the DualShock 4.
    ds5_state state {};
    state.reset();

    input_state_request input {};
    input.header.size = sizeof(input);
    input.header.version = k_protocol_version;
    input.buttons = button_mask::misc;
    const ds5_input_report report = encode_ds5_input(input, &state);
    check(report.report_id == k_ds5_input_report_id, "ds5 report id");
    check((report.buttons[2] & 0x04) != 0, "ds5 misc is the microphone mute button");

    input.buttons = button_mask::touchpad;
    check((encode_ds5_input(input, &state).buttons[2] & 0x02) != 0, "ds5 touchpad click");

    ds5_output_report output {};
    output.report_id = k_ds5_output_report_id;
    output.valid_flag0 = k_ds5_flag0_compatible_vibration | k_ds5_flag0_left_trigger_effect;
    output.valid_flag1 = k_ds5_flag1_lightbar | k_ds5_flag1_player_indicator;
    output.motor_left = 0x40;
    output.motor_right = 0x20;
    output.lightbar_red = 0xAA;
    output.player_leds = 0x05;
    output.left_trigger.mode = static_cast<std::uint8_t>(trigger_effect_mode::weapon);
    output.left_trigger.parameters[0] = 0x12;
    output.right_trigger.mode = static_cast<std::uint8_t>(trigger_effect_mode::vibration);

    playstation_output_feedback feedback {};
    check(decode_ds5_output(output, &feedback), "ds5 output decodes");
    check(feedback.low_frequency == 0x4000 && feedback.high_frequency == 0x2000, "ds5 rumble");
    check(feedback.red == 0xAA, "ds5 lightbar");
    check(feedback.player_leds == 0x05, "ds5 player LEDs");
    check((feedback.valid & ps_output_player_leds_valid) != 0, "ds5 player LEDs marked valid");
    check(feedback.left_trigger.mode == static_cast<std::uint8_t>(trigger_effect_mode::weapon),
          "ds5 left trigger effect");
    check(feedback.left_trigger.parameters[0] == 0x12, "ds5 left trigger parameters");
    // The right trigger was not enabled, so its program must not leak through.
    check(feedback.right_trigger.mode == 0, "ds5 unenabled trigger stays off");
    check((feedback.valid & ps_output_triggers_valid) != 0, "ds5 triggers marked valid");

    std::uint8_t buffer[80] {};
    check(fill_ds5_feature(k_ds5_feature_calibration_id, buffer, sizeof(buffer)) == 41,
          "ds5 calibration feature is 41 bytes");
    check(fill_ds5_feature(k_ds5_feature_firmware_id, buffer, sizeof(buffer)) == 64,
          "ds5 firmware feature is 64 bytes");
    check(fill_ds5_feature(k_ds5_feature_pairing_id, buffer, sizeof(buffer)) == 20,
          "ds5 pairing feature is 20 bytes");
    check(buffer[7] == 0x08 && buffer[8] == 0x25 && buffer[9] == 0x00,
          "ds5 pairing carries libScePad MAC-report magic");
    check(fill_ds5_feature(0x7C, buffer, sizeof(buffer)) == 0, "ds5 ignores unknown features");

    ds5_state calibrated_state {};
    calibrated_state.reset();
    motion_state_request motion {};
    motion.motion_type = static_cast<std::uint8_t>(motion_kind::gyroscope);
    motion.x_milli = 1000;
    check(apply_ds5_motion(motion, &calibrated_state), "ds5 calibration accepts a one-degree/s sample");
    const auto sample = encode_ds5_input(input, &calibrated_state);
    check(fill_ds5_feature(k_ds5_feature_calibration_id, buffer, sizeof(buffer)) == 41,
          "ds5 motion calibration reply is available");
    const auto signed_le16 = [](const std::uint8_t *p) {
      return static_cast<std::int16_t>(p[0] | (p[1] << 8));
    };
    const int speed = signed_le16(buffer + 19) + signed_le16(buffer + 21);
    const int range = signed_le16(buffer + 7) - signed_le16(buffer + 9);
    check(range > 0 && sample.gyro[0] * speed == range,
          "ds5 emitted gyro sample calibrates back to one degree/s");

  }

  {
    // The feedback event has to survive the protocol's own validation.
    playstation_output_feedback feedback {};
    feedback.low_frequency = 0x1234;
    const feedback_event event = encode_playstation_feedback(7, feedback);
    check(event.type == feedback_type::playstation_output, "ps feedback tagged");
    check(event.controller_id == 7, "ps feedback controller id");
    check(event.payload_size == sizeof(feedback), "ps feedback payload size");
    check(valid_request(&event, sizeof(event)), "ps feedback is a valid protocol message");
  }

  {
    // A profile the driver does not implement has to be refused, not answered
    // with a neighbouring one. This is the guard against a switch fallthrough
    // handing a caller a different vendor's controller under the wrong name.
    struct expectation { profile id; bool implemented; const char *name; };
    const expectation expectations[] = {
      {profile::generic_hid, false, "generic_hid"},
      {profile::generic_pid, false, "generic_pid"},
      {profile::xbox_series, true, "xbox_series"},
      {profile::dualshock_4, true, "dualshock_4"},
      {profile::dualsense, true, "dualsense"},
      {profile::xbox_360, false, "xbox_360"},
      {profile::xbox_one, true, "xbox_one"},
      {profile::switch_pro, true, "switch_pro"},
    };

    profile_mask_t expected_mask = 0;
    for (const expectation &e : expectations) {
      const profile_definition *const found = find_profile(e.id);
      check((found != nullptr) == e.implemented,
            std::string(e.name) + (e.implemented ? " is implemented" : " is refused"));
      if (found != nullptr) {
        check(found->id == e.id,
              std::string(e.name) + " returns its own definition, not another profile's");
        check(found->vendor_id != 0x1209 || found->product_id != 0x0001,
              std::string(e.name) + " does not advertise the pid.codes private-test identity");
        expected_mask |= profile_bit(e.id);
      }
    }
    constexpr profile_mask_t k_public_profile_mask = 0x7Cu;
    check(expected_mask == k_public_profile_mask,
          "the implemented profiles produce the exact public mask 0x7C");
    check(available_profiles() == k_public_profile_mask,
          "the advertised profile mask is exactly 0x7C");
    check((available_profiles() & profile_bit(profile::generic_hid)) == 0,
          "generic_hid is absent from the advertised profile mask");
    check((available_profiles() & profile_bit(profile::generic_pid)) == 0,
          "generic_pid is absent from the advertised profile mask");
  }

  {
    // Every profile owns its device's axis convention, so the protocol always
    // carries positive-up and a client must never pre-convert.
    input_state_request input {};
    input.header.size = sizeof(input);
    input.header.version = k_protocol_version;

    input.left_y = 32767;   // Fully up.
    input.right_y = 32767;
    const generic_input_report up = encode_generic_input(input);
    check(up.left_y < 0, "generic profile sends stick-up as negative HID Y");
    check(up.right_y < 0, "generic profile sends stick-up as negative HID Ry");

    input.left_y = -32768;  // Fully down, and the value that cannot be negated.
    const generic_input_report down = encode_generic_input(input);
    check(down.left_y == 32767, "generic profile saturates INT16_MIN instead of wrapping");

    input.left_y = 0;
    check(encode_generic_input(input).left_y == 0, "generic profile leaves a centred stick alone");

    // Horizontal axes are already the same in both conventions.
    input.left_x = 12345;
    check(encode_generic_input(input).left_x == 12345, "generic profile passes X through");
  }

  {
    // ---- Switch Pro ----
    std::size_t sw_size = 0;
    const std::uint8_t *const sw = switch_descriptor(&sw_size);
    const walk_result_t sw_parsed = walk(sw, sw_size);
    check(sw_parsed.well_formed, "switch descriptor parses: " + sw_parsed.error);
    if (sw_parsed.well_formed) {
      check(sw_parsed.collection_depth == 0, "switch collections balance");
      std::printf("switch descriptor: %zu bytes, %zu items, %zu report ids\n",
                  sw_size, sw_parsed.item_count, sw_parsed.reports.size());
      for (const std::uint8_t id : {k_switch_input_report_id, k_switch_subcommand_reply_id,
                                    k_switch_usb_reply_id}) {
        const auto found = sw_parsed.reports.find(id);
        if (found == sw_parsed.reports.end()) {
          check(false, "switch input report " + std::to_string(id) + " declared");
        } else {
          check(found->second.input / 8 + 1 == 64,
                "switch report " + std::to_string(id) + " is 64 bytes");
        }
      }
      for (const std::uint8_t id : {k_switch_rumble_subcommand_id, k_switch_rumble_only_id,
                                    k_switch_usb_command_id}) {
        const auto found = sw_parsed.reports.find(id);
        if (found == sw_parsed.reports.end()) {
          check(false, "switch output report " + std::to_string(id) + " declared");
        } else {
          check(found->second.output / 8 + 1 == 64,
                "switch output " + std::to_string(id) + " is 64 bytes");
        }
      }
    }
  }

  {
    switch_state state {};
    state.reset();

    input_state_request input {};
    input.header.size = sizeof(input);
    input.header.version = k_protocol_version;

    switch_input_report report = encode_switch_input(input, &state);
    check(report.report_id == k_switch_input_report_id, "switch report id");

    const auto axis = [](const std::uint8_t *st) {
      return static_cast<std::uint16_t>(st[0] | ((st[1] & 0x0F) << 8));
    };
    const auto vertical = [](const std::uint8_t *st) {
      return static_cast<std::uint16_t>((st[1] >> 4) | (st[2] << 4));
    };

    check(axis(report.left_stick) == k_switch_stick_center, "switch centred X");
    check(vertical(report.left_stick) == k_switch_stick_center, "switch centred Y");

    // This controller already reports positive-up, so unlike every other
    // profile the vertical axes must NOT be inverted.
    input.left_y = 32767;
    report = encode_switch_input(input, &state);
    check(vertical(report.left_stick) > k_switch_stick_center,
          "switch stick up increases the vertical axis");
    input.left_y = -32768;
    report = encode_switch_input(input, &state);
    check(vertical(report.left_stick) < k_switch_stick_center,
          "switch stick down decreases the vertical axis");
    input.left_y = 0;

    // Face buttons are positional: south is where Nintendo prints B.
    input.buttons = button_mask::south;
    check((encode_switch_input(input, &state).buttons_right & switch_b) != 0, "south is B");
    input.buttons = button_mask::east;
    check((encode_switch_input(input, &state).buttons_right & switch_a) != 0, "east is A");
    input.buttons = button_mask::west;
    check((encode_switch_input(input, &state).buttons_right & switch_y) != 0, "west is Y");
    input.buttons = button_mask::north;
    check((encode_switch_input(input, &state).buttons_right & switch_x) != 0, "north is X");
    input.buttons = button_mask::dpad_up;
    check((encode_switch_input(input, &state).buttons_left & switch_up) != 0, "d-pad up");
    input.buttons = button_mask::home;
    check((encode_switch_input(input, &state).buttons_shared & switch_home) != 0, "home");
    input.buttons = button_mask::misc;
    check((encode_switch_input(input, &state).buttons_shared & switch_capture) != 0,
          "misc is Capture");
    input.buttons = 0;

    // No analog triggers on this pad; travel becomes the digital shoulder.
    input.left_trigger = 200;
    check((encode_switch_input(input, &state).buttons_left & switch_zl) != 0, "left trigger is ZL");
    input.left_trigger = 0;
    check((encode_switch_input(input, &state).buttons_left & switch_zl) == 0, "ZL releases");
  }

  {
    // USB handshake: a host will not proceed without these replies.
    switch_state state {};
    state.reset();
    switch_usb_reply reply {};

    const std::uint8_t status_cmd[2] = {k_switch_usb_command_id, k_switch_usb_request_status};
    check(handle_switch_usb_command(status_cmd, sizeof(status_cmd), &state, &reply) != 0,
          "status command answered");
    check(reply.report_id == k_switch_usb_reply_id, "usb reply id");
    check(reply.subtype == k_switch_usb_request_status, "usb reply subtype");
    check(reply.data[1] == 0x03, "reports itself as a Pro Controller");
    check((reply.data[2] & 0x02) != 0, "advertised address is locally administered");

    const std::uint8_t handshake[2] = {k_switch_usb_command_id, k_switch_usb_handshake};
    check(handle_switch_usb_command(handshake, sizeof(handshake), &state, &reply) != 0,
          "handshake answered");
    check(state.handshake_complete, "handshake recorded");

    const std::uint8_t timeout[2] = {k_switch_usb_command_id, k_switch_usb_disable_timeout};
    check(handle_switch_usb_command(timeout, sizeof(timeout), &state, &reply) == 0,
          "disable-timeout is answered by silence");

    const std::uint8_t foreign[2] = {0x42, 0x01};
    check(handle_switch_usb_command(foreign, sizeof(foreign), &state, &reply) == 0,
          "a foreign report id is ignored");
  }

  {
    // Subcommands, including the SPI reads a host uses for calibration.
    switch_state state {};
    state.reset();
    input_state_request last {};
    last.header.size = sizeof(last);
    last.header.version = k_protocol_version;
    switch_subcommand_reply reply {};

    std::uint8_t request[64] {};
    request[0] = k_switch_rumble_subcommand_id;
    request[10] = k_switch_sub_request_device_info;
    check(handle_switch_subcommand(request, sizeof(request), last, &state, &reply) != 0,
          "device info answered");
    check(reply.report_id == k_switch_subcommand_reply_id, "subcommand reply id");
    check(reply.ack == 0x82, "device info ack");
    check(reply.subcommand == k_switch_sub_request_device_info, "echoes the subcommand");
    check(reply.data[2] == 0x03, "device info reports a Pro Controller");

    // Factory stick calibration must come back as real data, not erased flash,
    // or the host calibrates against 0xFF and the sticks read hard over.
    request[10] = k_switch_sub_spi_read;
    request[11] = 0x3D;
    request[12] = 0x60;
    request[13] = 0x00;
    request[14] = 0x00;
    request[15] = 18;
    check(handle_switch_subcommand(request, sizeof(request), last, &state, &reply) != 0,
          "spi read answered");
    check(reply.ack == 0x90, "spi read ack");
    check(reply.data[0] == 0x3D && reply.data[1] == 0x60, "spi reply echoes the address");
    check(reply.data[4] == 18, "spi reply echoes the length");
    bool all_erased = true;
    for (int i = 0; i < 18; ++i) {
      if (reply.data[5 + i] != 0xFF) {
        all_erased = false;
        break;
      }
    }
    check(!all_erased, "factory stick calibration is populated");

    // User calibration is deliberately erased so the host falls back.
    request[11] = 0x10;
    request[12] = 0x80;
    request[15] = 24;
    check(handle_switch_subcommand(request, sizeof(request), last, &state, &reply) != 0,
          "user calibration read answered");
    bool user_erased = true;
    for (int i = 0; i < 24; ++i) {
      if (reply.data[5 + i] != 0xFF) {
        user_erased = false;
        break;
      }
    }
    check(user_erased, "user calibration reads as unset");

    request[10] = k_switch_sub_enable_imu;
    request[11] = 1;
    check(handle_switch_subcommand(request, sizeof(request), last, &state, &reply) != 0,
          "imu enable answered");
    check(state.imu_enabled, "imu enable recorded");

    request[10] = k_switch_sub_set_player_lights;
    request[11] = 0x03;
    check(handle_switch_subcommand(request, sizeof(request), last, &state, &reply) != 0,
          "player lights answered");
    check(state.player_lights == 0x03, "player lights recorded");

    // An unmodelled subcommand is acknowledged rather than stalling the host.
    request[10] = 0x77;
    check(handle_switch_subcommand(request, sizeof(request), last, &state, &reply) != 0,
          "unknown subcommand acknowledged");
    check(reply.ack == 0x80, "unknown subcommand ack");
  }

  {
    // Rumble: neutral must read as silence, not as a small constant buzz.
    std::uint8_t neutral[10] {};
    neutral[0] = k_switch_rumble_only_id;
    const std::uint8_t idle[4] = {0x00, 0x01, 0x40, 0x40};
    std::memcpy(neutral + 2, idle, 4);
    std::memcpy(neutral + 6, idle, 4);

    playstation_output_feedback feedback {};
    check(decode_switch_rumble(neutral, sizeof(neutral), &feedback), "neutral rumble decodes");
    check(feedback.low_frequency == 0 && feedback.high_frequency == 0,
          "the neutral rumble pattern is silent");

    std::uint8_t strong[10] {};
    strong[0] = k_switch_rumble_only_id;
    strong[2] = 0x00;
    strong[3] = static_cast<std::uint8_t>(k_switch_amplitude_max << 1);
    strong[4] = 0x40;
    strong[5] = 0x40;
    std::memcpy(strong + 6, idle, 4);
    check(decode_switch_rumble(strong, sizeof(strong), &feedback), "strong rumble decodes");
    check(feedback.low_frequency == 65535, "full amplitude is full scale");
    check(feedback.high_frequency == 0, "the idle side stays silent");

    std::uint8_t foreign_rumble[10] {};
    foreign_rumble[0] = 0x42;
    check(!decode_switch_rumble(foreign_rumble, sizeof(foreign_rumble), &feedback),
          "a foreign report id is rejected");
  }

  for (const bool have_client_input : {false, true}) {
    // Reproduce Chromium's final initialization step with no later client
    // packets. The mode ACK must be followed by a fresh full input report.
    switch_state state {};
    state.reset();
    input_state_request last {};
    last.buttons = have_client_input ? button_mask::south : 0;
    report_pump pump;
    pump.reset();
    pump.set_ready();
    if (have_client_input) {
      const auto initial = encode_switch_input(last, &state);
      pump.enqueue(&initial, sizeof(initial), initial.report_id, report_kind::continuous);
      check(pump.take() != nullptr, "initial client input consumed before initialization");
      pump.set_ready();
    }

    std::uint8_t request[12] {};
    request[0] = k_switch_rumble_subcommand_id;
    request[10] = k_switch_sub_set_input_mode;
    request[11] = k_switch_input_report_id;
    for (int attempt = 0; attempt < 2; ++attempt) {
      check(queue_switch_subcommand_reply(request, sizeof(request), last, &state, pump),
            "full input mode queues its replies even on repeated initialization");
      const auto *ack = pump.take();
      check(ack != nullptr && ack->report_id == k_switch_subcommand_reply_id &&
              ack->data[14] == k_switch_sub_set_input_mode,
            "mode acknowledgement precedes full state");
      check(pump.take() == nullptr, "full state waits until VHF releases the ACK");
      pump.set_ready();
      const auto *full = pump.take();
      check(full != nullptr && full->report_id == k_switch_input_report_id &&
              full->length == sizeof(switch_input_report),
            "quiet controller supplies full state without another client packet");
      if (full != nullptr) {
        check(full->data[3] == (have_client_input ? switch_b : 0),
              "initialization preserves the latest buttons or neutral defaults");
      }
      pump.set_ready();
      check(pump.take() == nullptr, "initialization does not start an unbounded report loop");
    }

    // Other modes and a command missing its mode byte owe only an ACK.
    request[11] = 0x3F;
    for (const std::size_t size : {sizeof(request), sizeof(request) - 1}) {
      check(queue_switch_subcommand_reply(request, size, last, &state, pump),
            "other or missing input mode is acknowledged");
      check(pump.take() != nullptr, "mode ACK available");
      pump.set_ready();
      check(pump.take() == nullptr, "no unsolicited full report for another input mode");
    }
  }

  {
    // A deferred consumer retains the actual submission buffer after the
    // submitting function returns, while other producers change every queue.
    report_pump pump;
    pump.reset();
    const auto submit = [&pump]() {
      const std::uint8_t local[] = {0x30, 0x12, 0x34, 0x56};
      pump.enqueue(local, sizeof(local), 0x30, report_kind::continuous);
      pump.set_ready();
      return pump.take();
    };
    const auto *retained = submit();
    check(retained != nullptr, "deferred consumer receives an owned report");
    const std::uint8_t next[] = {0x21, 0xFF, 0xFF, 0xFF};
    pump.enqueue(next, sizeof(next), 0x21, report_kind::priority);
    for (int i = 0; i < k_transition_capacity + 2; ++i) {
      pump.enqueue(next, sizeof(next), 0x21, report_kind::transition);
      pump.enqueue(next, sizeof(next), 0x21, report_kind::continuous);
    }
    check(pump.take() == nullptr, "queued writes cannot reuse an outstanding buffer");
    const std::uint8_t expected[] = {0x30, 0x12, 0x34, 0x56};
    check(retained != nullptr && retained->length == sizeof(expected) &&
            retained->report_id == 0x30 &&
            std::memcmp(retained->data, expected, sizeof(expected)) == 0,
          "deferred read survives function return and pending queue mutation");
    pump.set_ready();
    const auto *after_release = pump.take();
    check(after_release != nullptr && after_release->report_id == 0x21,
          "next report becomes available only after the readiness callback");
  }

  {
    // ---- report pacing ----
    report_pump pump;
    pump.reset();
    check(!pump.ready(), "a fresh pump waits for the first VHF callback");
    check(pump.empty(), "a fresh pump has nothing waiting");

    const std::uint8_t a[4] = {1, 0, 0, 0};
    const std::uint8_t b[4] = {2, 0, 0, 0};
    const std::uint8_t c[4] = {3, 0, 0, 0};
    report_buffer out {};

    check(pump.enqueue(a, sizeof(a), 1, report_kind::continuous), "first enqueue");
    check(!take_report(pump, out), "initial input stays queued until VHF is ready");
    pump.set_ready();
    check(take_report(pump, out), "first report is taken");
    check(out.data[0] == 1, "first report is the one enqueued");
    check(!pump.ready(), "taking a report consumes readiness");
    check(!take_report(pump, out), "nothing more is sent until VHF is ready again");

    // While waiting, newer continuous state replaces older: a consumer that
    // was busy should see where the stick is now, not where it was.
    check(pump.enqueue(b, sizeof(b), 1, report_kind::continuous), "second continuous");
    check(pump.enqueue(c, sizeof(c), 1, report_kind::continuous), "third continuous");
    pump.set_ready();
    check(take_report(pump, out), "a report is waiting");
    check(out.data[0] == 3, "the newest continuous state is sent, got " + std::to_string(out.data[0]));
    pump.set_ready();
    check(!take_report(pump, out), "the superseded state was not also queued");
  }

  {
    // Transitions must survive: dropping one loses a press or a release.
    report_pump pump;
    pump.reset();
    pump.set_ready();
    report_buffer out {};
    const std::uint8_t press[2] = {0xA1, 0};
    const std::uint8_t release[2] = {0xA0, 0};
    const std::uint8_t moving[2] = {0xB0, 0};

    check(pump.enqueue(press, sizeof(press), 1, report_kind::transition), "press");
    check(take_report(pump, out) && out.data[0] == 0xA1, "press sent");

    check(pump.enqueue(release, sizeof(release), 1, report_kind::transition), "release");
    check(pump.enqueue(moving, sizeof(moving), 1, report_kind::continuous), "movement");
    pump.set_ready();
    check(take_report(pump, out), "something is waiting");
    check(out.data[0] == 0xA0, "the release is sent before newer movement");
    pump.set_ready();
    check(take_report(pump, out) && out.data[0] == 0xB0, "movement follows");
  }

  {
    // A continuous snapshot predating a transition must never be replayed
    // afterwards, or it can restore the button state the transition changed.
    report_pump pump;
    pump.reset();
    pump.set_ready();
    report_buffer out {};
    const std::uint8_t press[2] = {0xA1, 0};
    const std::uint8_t moving_while_pressed[2] = {0xB1, 0};
    const std::uint8_t release[2] = {0xA0, 0};

    check(pump.enqueue(press, sizeof(press), 1, report_kind::transition), "press queued");
    check(take_report(pump, out) && out.data[0] == 0xA1, "press sent");
    check(pump.enqueue(moving_while_pressed, sizeof(moving_while_pressed), 1,
                       report_kind::continuous),
          "movement while pressed queued");
    check(pump.enqueue(release, sizeof(release), 1, report_kind::transition), "release queued");
    pump.set_ready();
    check(take_report(pump, out) && out.data[0] == 0xA0, "release supersedes older movement");
    pump.set_ready();
    check(!take_report(pump, out), "stale pressed movement was discarded");
  }

  {
    // Initialization replies go ahead of controller state.
    report_pump pump;
    pump.reset();
    pump.set_ready();
    report_buffer out {};
    const std::uint8_t state[2] = {0x30, 0};
    const std::uint8_t reply[2] = {0x21, 0};

    check(take_report(pump, out) == false, "nothing to send yet");
    check(pump.enqueue(state, sizeof(state), 0x30, report_kind::transition), "state queued");
    check(pump.enqueue(reply, sizeof(reply), 0x21, report_kind::priority), "reply queued");
    check(take_report(pump, out), "a report is taken");
    check(out.report_id == 0x21, "the handshake reply goes first");
    pump.set_ready();
    check(take_report(pump, out) && out.report_id == 0x30, "controller state follows");
  }

  {
    // A stalled consumer must not be able to strand a button down. When the
    // queue fills, the oldest transition is dropped, so a lost press whose
    // release is still queued reads as released rather than held.
    report_pump pump;
    pump.reset();
    pump.set_ready();
    report_buffer out {};
    std::uint8_t payload[2] = {0, 0};

    check(pump.enqueue(payload, sizeof(payload), 1, report_kind::transition), "prime");
    check(take_report(pump, out), "prime sent");

    for (int i = 0; i < k_transition_capacity + 4; ++i) {
      payload[0] = static_cast<std::uint8_t>(i);
      std::ignore = pump.enqueue(payload, sizeof(payload), 1, report_kind::transition);
    }
    check(pump.transitions_waiting() == k_transition_capacity, "the queue is bounded");
    check(pump.dropped_transitions() == 4, "the overflow was counted, got " +
                                             std::to_string(pump.dropped_transitions()));

    // Whatever was dropped, the newest transition still arrives, so the final
    // state is right.
    std::uint8_t last = 0;
    for (int i = 0; i < k_transition_capacity; ++i) {
      pump.set_ready();
      if (take_report(pump, out)) {
        last = out.data[0];
      }
    }
    check(last == k_transition_capacity + 3,
          "the newest transition survives, got " + std::to_string(last));
  }

  {
    // Classification drives all of the above, so it has to agree with what a
    // player would call a discrete change.
    report_pump pump;
    pump.reset();
    pump.set_ready();
    check(pump.classify(0, 0, 0) == report_kind::transition,
          "the first state is always a transition");
    check(pump.classify(0, 0, 0) == report_kind::continuous, "an unchanged state is continuous");
    check(pump.classify(button_mask::south, 0, 0) == report_kind::transition, "a press is discrete");
    check(pump.classify(button_mask::south, 0, 0) == report_kind::continuous, "holding is not");
    check(pump.classify(0, 0, 0) == report_kind::transition, "a release is discrete");
    // Analog travel is continuous until it crosses the digital threshold.
    check(pump.classify(0, 40, 0) == report_kind::transition, "a trigger leaving rest is discrete");
    check(pump.classify(0, 200, 0) == report_kind::continuous, "further travel is continuous");
    check(pump.classify(0, 0, 0) == report_kind::transition, "a trigger returning to rest is discrete");
  }

  {
    // ---- PlayStation output reports, built from raw bytes ----
    // These buffers are filled at literal byte offsets rather than through the
    // report structs, so the test still fails if a struct field moves. Decoding
    // our own encoding would only prove the code agrees with itself; the point
    // here is that it agrees with the console.
    check(k_ds5_output_report_id == 0x02, "the DualSense output report is 0x02");

    std::uint8_t raw[48] {};
    raw[0] = 0x02;  // report id
    raw[1] = 0x01 | 0x04 | 0x08;  // flag0: vibration, right trigger, left trigger
    raw[2] = 0x01 | 0x04 | 0x10;  // flag1: mic LED, light bar, player indicator
    raw[3] = 0x40;  // motor_right - the small, high frequency motor
    raw[4] = 0x80;  // motor_left  - the large, low frequency motor
    raw[9] = 0x01;  // mute button LED
    raw[11] = 0x26;  // right trigger effect mode
    raw[12] = 0xAA;  // first right trigger parameter
    raw[22] = 0x21;  // left trigger effect mode
    raw[23] = 0xBB;  // first left trigger parameter
    raw[44] = 0x05;  // player LEDs
    raw[45] = 0x10;  // red
    raw[46] = 0x20;  // green
    raw[47] = 0x30;  // blue

    ds5_output_report ds5 {};
    std::memcpy(&ds5, raw, sizeof(ds5));
    playstation_output_feedback fb {};
    check(decode_ds5_output(ds5, &fb), "a DualSense output report decodes");

    // Left drives the low frequency motor and right the high, which is the
    // pairing every PlayStation pad uses. Swapping them puts a heavy rumble
    // where a game asked for a light one.
    check(fb.low_frequency == 0x8000, "motor_left becomes the low frequency motor");
    check(fb.high_frequency == 0x4000, "motor_right becomes the high frequency motor");
    check(fb.red == 0x10 && fb.green == 0x20 && fb.blue == 0x30, "the light bar colour survives");
    check((fb.valid & ps_output_lightbar_valid) != 0, "the light bar is marked valid");
    check(fb.player_leds == 0x05, "the player LEDs survive");
    check((fb.valid & ps_output_player_leds_valid) != 0, "the player LEDs are marked valid");
    check(fb.microphone_led == 0x01, "the microphone LED survives");
    check((fb.valid & ps_output_microphone_led_valid) != 0, "the microphone LED is marked valid");
    check(fb.right_trigger.mode == 0x26, "the right trigger effect is not the left one");
    check(fb.right_trigger.parameters[0] == 0xAA, "the right trigger parameters follow its mode");
    check(fb.left_trigger.mode == 0x21, "the left trigger effect is not the right one");
    check(fb.left_trigger.parameters[0] == 0xBB, "the left trigger parameters follow its mode");
    check((fb.valid & ps_output_triggers_valid) != 0, "the trigger effects are marked valid");

    // Stateless decoding ignores data without its validity bit. Stateful
    // application below preserves the previously enabled actuator values.
    std::uint8_t idle[48] {};
    idle[0] = 0x02;
    idle[3] = 0x40;
    idle[4] = 0x80;
    idle[45] = 0x10;
    ds5_output_report quiet {};
    std::memcpy(&quiet, idle, sizeof(quiet));
    playstation_output_feedback quiet_fb {};
    check(decode_ds5_output(quiet, &quiet_fb), "an output report with no enable bits decodes");
    check(quiet_fb.low_frequency == 0 && quiet_fb.high_frequency == 0,
          "rumble is ignored without its enable bit");
    check(quiet_fb.red == 0, "the light bar is ignored without its enable bit");
    check(quiet_fb.valid == 0, "nothing is marked valid");
    check(quiet_fb.left_trigger.mode == 0 && quiet_fb.right_trigger.mode == 0,
          "invalid trigger bytes are ignored by stateless decoding");

    // A foreign report id must be refused rather than parsed as ours.
    ds5_output_report wrong {};
    std::memcpy(&wrong, raw, sizeof(wrong));
    wrong.report_id = 0x31;
    playstation_output_feedback ignored {};
    check(!decode_ds5_output(wrong, &ignored), "a foreign report id is refused");
  }

  {
    // The DualShock 4 shares the pairing but has its own, smaller report.
    check(k_ds4_output_report_id == 0x05, "the DualShock 4 output report is 0x05");

    std::uint8_t raw[32] {};
    raw[0] = 0x05;
    raw[1] = 0x01 | 0x02;  // rumble and light bar
    raw[4] = 0x30;  // right rumble
    raw[5] = 0x90;  // left rumble
    raw[6] = 0x11;
    raw[7] = 0x22;
    raw[8] = 0x33;

    ds4_output_report ds4 {};
    std::memcpy(&ds4, raw, sizeof(ds4));
    playstation_output_feedback fb {};
    check(decode_ds4_output(ds4, &fb), "a DualShock 4 output report decodes");
    check(fb.low_frequency == 0x9000, "left drives the low frequency motor");
    check(fb.high_frequency == 0x3000, "right drives the high frequency motor");
    check(fb.red == 0x11 && fb.green == 0x22 && fb.blue == 0x33, "the light bar colour survives");
    check((fb.valid & ps_output_lightbar_valid) != 0, "the light bar is marked valid");
    // A DualShock 4 has no adaptive triggers, player LEDs or microphone LED.
    check((fb.valid & ps_output_triggers_valid) == 0, "no trigger effects are claimed");
    check((fb.valid & ps_output_player_leds_valid) == 0, "no player LEDs are claimed");

    std::uint8_t idle[32] {};
    idle[0] = 0x05;
    idle[4] = 0x30;
    idle[5] = 0x90;
    ds4_output_report quiet {};
    std::memcpy(&quiet, idle, sizeof(quiet));
    playstation_output_feedback quiet_fb {};
    check(decode_ds4_output(quiet, &quiet_fb), "an output report with no enable bits decodes");
    check(quiet_fb.low_frequency == 0 && quiet_fb.high_frequency == 0,
          "rumble is ignored without its enable bit");
    check(quiet_fb.valid == 0, "nothing is marked valid");
  }

  {
    // ---- Xbox One ----
    std::size_t one_size = 0;
    const std::uint8_t *const one = xbox_one_descriptor(&one_size);
    const walk_result_t one_parsed = walk(one, one_size);
    check(one_parsed.well_formed, "xbox one descriptor parses: " + one_parsed.error);
    if (one_parsed.well_formed) {
      check(one_parsed.collection_depth == 0, "xbox one collections balance");
      check(one_parsed.top_level_collections == 1, "xbox one has one application collection");
      std::printf("xbox one descriptor: %zu bytes, %zu items, %zu report ids\n",
                  one_size, one_parsed.item_count, one_parsed.reports.size());

      const auto input = one_parsed.reports.find(k_xbox_one_input_report_id);
      if (input == one_parsed.reports.end()) {
        check(false, "xbox one input report exists");
      } else {
        // One byte shorter than the Series report: no Share button.
        check(input->second.input == 120,
              "xbox one input report is 120 bits, got " + std::to_string(input->second.input));
        check(input->second.input / 8 + 1 == sizeof(xbox_one_input_report),
              "xbox one input report matches its struct");
      }

      // The rumble report is shared, so it must still be there and unchanged.
      const auto output = one_parsed.reports.find(k_xbox_one_output_report_id);
      if (output == one_parsed.reports.end()) {
        check(false, "xbox one output report exists");
      } else {
        check(output->second.output == 64,
              "xbox one output report is 64 bits, got " + std::to_string(output->second.output));
      }
    }

    // The two descriptors are meant to differ by exactly the Share block.
    // Deriving one from the other here means a later edit to the Series
    // descriptor that is not mirrored fails this test instead of shipping two
    // pads that disagree about their own stick range.
    std::size_t series_size = 0;
    const std::uint8_t *const series = xbox_series_descriptor(&series_size);
    static const std::uint8_t share_block[] = {
      0x05, 0x0C,        // Usage Page (Consumer)
      0x0A, 0xB2, 0x00,  // Usage (Record)
      0x15, 0x00,
      0x25, 0x01,
      0x95, 0x01,
      0x75, 0x01,
      0x81, 0x02,
      0x15, 0x00,
      0x25, 0x00,
      0x75, 0x07,
      0x95, 0x01,
      0x81, 0x03,
    };
    const std::vector<std::uint8_t> series_bytes(series, series + series_size);
    const auto found = std::search(series_bytes.begin(), series_bytes.end(),
                                   std::begin(share_block), std::end(share_block));
    check(found != series_bytes.end(), "the Share block is present in the Series descriptor");
    if (found != series_bytes.end()) {
      std::vector<std::uint8_t> derived(series_bytes.begin(), found);
      derived.insert(derived.end(), found + sizeof(share_block), series_bytes.end());
      const bool same = derived.size() == one_size &&
                        std::equal(derived.begin(), derived.end(), one);
      check(same, "the Xbox One descriptor is the Series one without the Share block");
      if (!same) {
        std::printf("  derived %zu bytes, xbox one is %zu\n", derived.size(), one_size);
      }
    }

    // The shared prefix has to encode identically, or the two pads would report
    // different sticks from the same input.
    input_state_request sample {};
    sample.left_x = 12000;
    sample.left_y = -9000;
    sample.right_x = -400;
    sample.right_y = 25000;
    sample.left_trigger = 200;
    sample.right_trigger = 30;
    sample.buttons = button_mask::south | button_mask::dpad_left | button_mask::start;

    const xbox_series_input_report full = encode_xbox_series_input(sample);
    const xbox_one_input_report short_report = encode_xbox_one_input(sample);
    check(std::memcmp(&full, &short_report, sizeof(short_report)) == 0,
          "the Xbox One report is the Series report without its last byte");

    // Share is the one thing an Xbox One pad cannot report. Asking for it must
    // not change anything else.
    input_state_request with_share = sample;
    with_share.buttons |= button_mask::misc;
    const xbox_one_input_report unchanged = encode_xbox_one_input(with_share);
    check(std::memcmp(&short_report, &unchanged, sizeof(unchanged)) == 0,
          "an Xbox One pad silently has no Share button");
    const xbox_series_input_report series_with_share = encode_xbox_series_input(with_share);
    check(series_with_share.share == 1, "a Series pad still reports Share");
  }

  {
    playstation_output_feedback state {};
    ds4_output_report rumble {};
    rumble.report_id = k_ds4_output_report_id;
    rumble.flags = 0x01;
    rumble.left_rumble = 0x80;
    rumble.right_rumble = 0x40;
    check(apply_ds4_output(rumble, &state), "ds4 starts rumble");

    ds4_output_report light {};
    light.report_id = k_ds4_output_report_id;
    light.flags = 0x02;
    light.red = 0x20;
    check(apply_ds4_output(light, &state), "ds4 applies a later LED-only report");
    const auto pending = encode_playstation_feedback(0, state);
    playstation_output_feedback polled {};
    std::memcpy(&polled, pending.payload, sizeof(polled));
    check(polled.low_frequency == 0x8000 && polled.high_frequency == 0x4000,
          "ds4 LED output does not erase rumble before a feedback poll");
    check(polled.red == 0x20, "ds4 coalesced report also carries the new LED");

    rumble.left_rumble = rumble.right_rumble = 0;
    check(apply_ds4_output(rumble, &state), "ds4 accepts an explicit motor stop");
    check(state.low_frequency == 0 && state.high_frequency == 0,
          "ds4 stops both motors when their enabled values are zero");
    check(state.red == 0x20 && (state.valid & ps_output_lightbar_valid),
          "ds4 rumble-only output retains the LED across polls");
  }

  {
    playstation_output_feedback state {};
    ds5_output_report output {};
    output.report_id = k_ds5_output_report_id;
    output.valid_flag0 = k_ds5_flag0_compatible_vibration | k_ds5_flag0_right_trigger_effect;
    output.motor_left = 0x90;
    output.motor_right = 0x30;
    output.right_trigger.mode = 0x26;
    output.right_trigger.parameters[0] = 0xAB;
    check(apply_ds5_output(output, &state), "ds5 applies rumble and right trigger");

    output = {};
    output.report_id = k_ds5_output_report_id;
    output.valid_flag0 = k_ds5_flag0_left_trigger_effect;
    output.left_trigger.mode = 0x21;
    output.left_trigger.parameters[0] = 0xCD;
    check(apply_ds5_output(output, &state), "ds5 accepts a separate left trigger program");
    check(state.right_trigger.mode == 0x26 && state.right_trigger.parameters[0] == 0xAB,
          "ds5 left trigger update preserves the right trigger");
    check(state.low_frequency == 0x9000 && state.high_frequency == 0x3000,
          "ds5 trigger-only update preserves rumble");

    output = {};
    output.report_id = k_ds5_output_report_id;
    output.valid_flag1 = k_ds5_flag1_lightbar;
    output.lightbar_blue = 0x55;
    check(apply_ds5_output(output, &state), "ds5 applies an independent lightbar update");
    const auto pending = encode_playstation_feedback(1, state);
    playstation_output_feedback polled {};
    std::memcpy(&polled, pending.payload, sizeof(polled));
    check(polled.left_trigger.mode == 0x21 && polled.right_trigger.mode == 0x26 &&
            polled.low_frequency == 0x9000 && polled.blue == 0x55,
          "ds5 polling receives all accumulated actuators after coalescing");

    output = {};
    output.report_id = k_ds5_output_report_id;
    output.valid_flag0 = k_ds5_flag0_left_trigger_effect;
    check(apply_ds5_output(output, &state), "ds5 accepts an explicit left trigger off program");
    check(state.left_trigger.mode == 0 && state.left_trigger.parameters[0] == 0 &&
            state.right_trigger.mode == 0x26,
          "ds5 disabling one trigger leaves the other programmed");

    const auto retained = state;
    output.report_id = 0x7F;
    check(!apply_ds5_output(output, &state), "ds5 refuses a foreign output report");
    check(std::memcmp(&state, &retained, sizeof(state)) == 0,
          "an invalid output report cannot change retained actuators");
  }

  {
    ds5_output_report output {};
    output.report_id = k_ds5_output_report_id;
    output.valid_flag0 = k_ds5_flag0_haptics_select;
    output.valid_flag2 = 0x04; // Compatible vibration v2, independent of legacy flag0 bit 0
    output.motor_left = 0x60;
    output.motor_right = 0x20;
    playstation_output_feedback feedback {};
    check(decode_ds5_output(output, &feedback), "ds5 vibration-v2 report decodes");
    check(feedback.low_frequency == 0x6000 && feedback.high_frequency == 0x2000,
          "ds5 vibration-v2 motor amplitudes reach feedback");
    output.motor_left = output.motor_right = 0;
    check(apply_ds5_output(output, &feedback), "ds5 vibration-v2 stop applies");
    check(feedback.low_frequency == 0 && feedback.high_frequency == 0,
          "ds5 vibration-v2 can stop the motors");
  }

  if (g_failures == 0) {
    std::printf("all PID descriptor and engine checks passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", g_failures);
  return 1;
}
