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
  event.payload_size = 4;
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
    check(c.poll_feedback(1, &out) == ERROR_SUCCESS && out.payload_size == 4,
          "a well-formed event is accepted");
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
  if (failures == 0) {
    std::printf("client contract: all checks passed\n");
  }
  return failures == 0 ? 0 : 1;
}
