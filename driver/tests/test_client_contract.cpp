// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// The client library's error contract against a fake DeviceIoControl: the
// pre-flight checks, the protocol-generation mapping and poll_feedback's event
// validation. Everything else about the client is exercised only by the probes
// against a real driver, which no CI job runs.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
BOOL WINAPI fake_DeviceIoControl(
  HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DWORD WINAPI fake_GetLastError();
}  // namespace

#define DeviceIoControl fake_DeviceIoControl
#define GetLastError fake_GetLastError
#define private public
#include "../../client/client.cpp"
#undef private
#undef DeviceIoControl
#undef GetLastError

namespace {
int failures = 0;
void check(const bool condition, const std::string &what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

struct fake_driver {
  BOOL result {TRUE};
  DWORD error {ERROR_SUCCESS};
  DWORD last_code {0};
  DWORD last_input_size {0};
  DWORD output_bytes {0};
  lvg::feedback_event event {};
  lvg::query_info_response info {};
} driver;

BOOL WINAPI fake_DeviceIoControl(
  HANDLE,
  const DWORD code,
  LPVOID,
  const DWORD input_size,
  LPVOID output,
  const DWORD output_size,
  LPDWORD bytes,
  LPOVERLAPPED) {
  driver.last_code = code;
  driver.last_input_size = input_size;
  if (!driver.result) {
    return FALSE;
  }
  if (output != nullptr && output_size >= sizeof(driver.event)) {
    std::memcpy(output, &driver.event, sizeof(driver.event));
  } else if (output != nullptr && output_size >= sizeof(driver.info)) {
    std::memcpy(output, &driver.info, sizeof(driver.info));
  }
  *bytes = driver.output_bytes;
  return TRUE;
}

DWORD WINAPI fake_GetLastError() {
  return driver.error;
}

lvg::client connected_client() {
  lvg::client c;
  c.handle_ = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234));
  c.info_.maximum_controllers = 4;
  c.info_.available_profiles = lvg::profile_bit(lvg::profile::steam_controller);
  return c;
}

lvg::feedback_event good_event(const std::uint32_t controller_id) {
  lvg::feedback_event event {};
  lvg::initialize_header(&event.header, sizeof(event));
  event.controller_id = controller_id;
  event.type = lvg::feedback_type::generic_rumble;
  event.payload_size = sizeof(lvg::generic_rumble_rgb_feedback);
  return event;
}
}  // namespace

int main() {
  {
    lvg::client c;
    check(c.create_controller(0, lvg::profile::steam_controller) == ERROR_INVALID_HANDLE,
          "create on an unconnected client is ERROR_INVALID_HANDLE");
    check(c.destroy_controller(0) == ERROR_INVALID_HANDLE,
          "destroy on an unconnected client is ERROR_INVALID_HANDLE");
    lvg::feedback_event event {};
    check(c.poll_feedback(0, &event) == ERROR_INVALID_HANDLE,
          "poll on an unconnected client is ERROR_INVALID_HANDLE");
    lvg::feedback_counters counters {};
    check(c.query_feedback_counters(0, &counters) == ERROR_INVALID_HANDLE,
          "counters on an unconnected client is ERROR_INVALID_HANDLE, not the"
          " ERROR_INVALID_PARAMETER a zero maximum_controllers() would imply");
    c.handle_ = INVALID_HANDLE_VALUE;
  }
  {
    auto c = connected_client();
    check(c.create_controller(4, lvg::profile::steam_controller) == ERROR_INVALID_PARAMETER,
          "create with an out-of-range slot is ERROR_INVALID_PARAMETER");
    check(c.destroy_controller(4) == ERROR_INVALID_PARAMETER,
          "destroy with an out-of-range slot is ERROR_INVALID_PARAMETER");
    check(c.create_controller(0, lvg::profile::dualshock_4) == ERROR_NOT_SUPPORTED,
          "create with an unavailable profile is ERROR_NOT_SUPPORTED");
    check(driver.last_code == 0, "pre-flight failures never reach the driver");
    check(c.create_controller(3, lvg::profile::steam_controller) == ERROR_SUCCESS &&
            driver.last_code == lvg::ioctl_create_controller &&
            driver.last_input_size == sizeof(lvg::create_controller_request),
          "a valid create reaches the driver with the request size");
    c.handle_ = INVALID_HANDLE_VALUE;
  }
  {
    // Every IOCTL maps the driver's protocol-generation rejection the same way.
    auto c = connected_client();
    driver.result = FALSE;
    driver.error = ERROR_INVALID_USER_BUFFER;
    lvg::input_state_request input {};
    driver.last_code = 0;
    check(c.submit_input_state(input) == ERROR_INVALID_PARAMETER && driver.last_code == 0,
          "a submit without a protocol header is refused before the driver");
    lvg::initialize_header(&input.header, sizeof(input));
    check(c.create_controller(0, lvg::profile::steam_controller) == ERROR_REVISION_MISMATCH,
          "STATUS_INVALID_BUFFER_SIZE from create is ERROR_REVISION_MISMATCH");
    check(c.destroy_controller(0) == ERROR_REVISION_MISMATCH,
          "STATUS_INVALID_BUFFER_SIZE from destroy is ERROR_REVISION_MISMATCH");
    check(c.submit_input_state(input) == ERROR_REVISION_MISMATCH,
          "STATUS_INVALID_BUFFER_SIZE from submit is ERROR_REVISION_MISMATCH");
    check(c.query_info() == ERROR_REVISION_MISMATCH,
          "STATUS_INVALID_BUFFER_SIZE from query_info is ERROR_REVISION_MISMATCH");
    driver.error = ERROR_BUSY;
    check(c.create_controller(0, lvg::profile::steam_controller) == ERROR_BUSY,
          "other driver errors pass through unchanged");
    driver.result = TRUE;
    driver.error = ERROR_SUCCESS;
    c.handle_ = INVALID_HANDLE_VALUE;
  }
  {
    // poll_feedback validates the driver's event before handing it out.
    auto c = connected_client();
    lvg::feedback_event out {};
    check(c.poll_feedback(1, nullptr) == ERROR_INVALID_PARAMETER, "poll with no event is refused");
    driver.last_code = 0;
    driver.event = good_event(1);
    driver.output_bytes = sizeof(driver.event);
    check(c.poll_feedback(1, &out) == ERROR_SUCCESS && out.payload_size == sizeof(lvg::generic_rumble_rgb_feedback),
          "a well-formed event is accepted");
    driver.event = good_event(1);
    driver.event.type = static_cast<lvg::feedback_type>(99);
    check(c.poll_feedback(1, &out) == ERROR_INVALID_DATA, "an unknown feedback type is rejected");
    driver.event = good_event(1);
    driver.event.type = lvg::feedback_type::xbox_rumble;
    driver.event.payload_size = 4;
    check(c.poll_feedback(1, &out) == ERROR_INVALID_DATA, "a payload size that is not the type's struct is rejected");
    driver.event = good_event(1);
    driver.event.type = lvg::feedback_type::playstation_output;
    driver.event.payload_size = sizeof(lvg::playstation_output_feedback);
    check(c.poll_feedback(1, &out) == ERROR_SUCCESS && out.type == lvg::feedback_type::playstation_output,
          "every known type is accepted with its whole struct");
    driver.event = good_event(1);
    driver.event.type = lvg::feedback_type::none;
    driver.event.payload_size = 0;
    check(c.poll_feedback(1, &out) == ERROR_SUCCESS, "a 'none' event carries no payload");
    driver.output_bytes = sizeof(driver.event) - 1;
    check(c.poll_feedback(1, &out) == ERROR_INVALID_DATA, "a short event is rejected");
    driver.output_bytes = sizeof(driver.event);
    driver.event = good_event(2);
    check(c.poll_feedback(1, &out) == ERROR_INVALID_DATA, "an event for another slot is rejected");
    driver.event = good_event(1);
    driver.event.payload_size = sizeof(driver.event.payload) + 1;
    check(c.poll_feedback(1, &out) == ERROR_INVALID_DATA, "an oversized payload is rejected");
    driver.event = good_event(1);
    driver.event.header.version = lvg::k_protocol_version + 1;
    check(c.poll_feedback(1, &out) == ERROR_INVALID_DATA, "a foreign protocol version is rejected");
    driver.event = good_event(1);
    driver.event.header.size = sizeof(driver.event) - 8;
    check(c.poll_feedback(1, &out) == ERROR_INVALID_DATA, "a wrong header size is rejected");
    c.handle_ = INVALID_HANDLE_VALUE;
  }
  {
    // query_feedback_counters is the controller shape of ioctl_query_info: the
    // counters ride in the word the device query keeps zero.
    auto c = connected_client();
    lvg::feedback_counters counters {};
    driver.last_code = 0;
    check(c.query_feedback_counters(0, nullptr) == ERROR_INVALID_PARAMETER, "counters with no output are refused");
    check(c.query_feedback_counters(4, &counters) == ERROR_INVALID_PARAMETER,
          "counters for an out-of-range slot are refused");
    check(driver.last_code == 0, "those pre-flight failures never reach the driver");
    driver.info = {};
    driver.info.header.size = sizeof(driver.info);
    driver.info.header.version = lvg::k_protocol_version;
    driver.info.minimum_protocol_version = lvg::k_protocol_version;
    driver.info.maximum_protocol_version = lvg::k_protocol_version;
    driver.info.maximum_controllers = 4;
    driver.info.counters = {5, 1, 3, 2};
    driver.output_bytes = sizeof(driver.info);
    check(c.query_feedback_counters(2, &counters) == ERROR_SUCCESS &&
            driver.last_code == lvg::ioctl_query_info &&
            driver.last_input_size == sizeof(lvg::controller_id_request) &&
            counters.haptic_evicted == 5 && counters.settings_evicted == 1 &&
            counters.transitions_dropped == 3 && counters.unknown_commands == 2,
          "the counters query sends a controller_id_request on ioctl_query_info and reads back all four");
    check(c.query_info() == ERROR_REVISION_MISMATCH,
          "a device query answered with a non-zero counters word is another protocol generation");
    driver.info.counters = {};
    check(c.query_info() == ERROR_SUCCESS && driver.last_input_size == sizeof(lvg::query_info_request),
          "the device query still sends the 8-byte request and accepts a zero counters word");
    driver.output_bytes = sizeof(driver.info) - 1;
    check(c.query_feedback_counters(2, &counters) == ERROR_INVALID_DATA, "a short counters response is rejected");
    driver.output_bytes = sizeof(driver.info);
    driver.result = FALSE;
    driver.error = ERROR_INVALID_USER_BUFFER;
    check(c.query_feedback_counters(2, &counters) == ERROR_REVISION_MISMATCH,
          "a driver that predates the counters (STATUS_INVALID_BUFFER_SIZE) is ERROR_REVISION_MISMATCH");
    driver.result = TRUE;
    driver.error = ERROR_SUCCESS;
    c.handle_ = INVALID_HANDLE_VALUE;
  }
  if (failures == 0) {
    std::printf("client contract: all checks passed\n");
  }
  return failures == 0 ? 0 : 1;
}
