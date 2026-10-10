// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "libvirtualgamepad/client.h"

#include <setupapi.h>

#include <utility>

namespace lvg {
namespace {

void initialize_header(request_header *const header, const std::uint32_t size) noexcept {
  header->size = size;
  header->version = k_protocol_version;
  header->reserved = 0;
}

}  // namespace

client::~client() noexcept {
  close();
}

client::client(client &&other) noexcept :
    handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
    info_(std::exchange(other.info_, query_info_response {})) {}

client &client::operator=(client &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
    info_ = std::exchange(other.info_, query_info_response {});
  }
  return *this;
}

DWORD client::connect() noexcept {
  close();

  const HDEVINFO devices = SetupDiGetClassDevsW(
    &k_device_interface_guid,
    nullptr,
    nullptr,
    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devices == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }

  DWORD last_error = ERROR_NOT_FOUND;
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interface_data {};
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(
          devices,
          nullptr,
          &k_device_interface_guid,
          index,
          &interface_data)) {
      const DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_ITEMS) {
        last_error = error;
      }
      break;
    }

    DWORD detail_bytes = 0;
    SetupDiGetDeviceInterfaceDetailW(
      devices,
      &interface_data,
      nullptr,
      0,
      &detail_bytes,
      nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        detail_bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
      last_error = GetLastError();
      continue;
    }

    auto *const detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(LocalAlloc(LPTR, detail_bytes));
    if (detail == nullptr) {
      last_error = ERROR_NOT_ENOUGH_MEMORY;
      continue;
    }
    detail->cbSize = sizeof(*detail);
    const BOOL detail_ok = SetupDiGetDeviceInterfaceDetailW(
      devices,
      &interface_data,
      detail,
      detail_bytes,
      nullptr,
      nullptr);
    if (!detail_ok) {
      last_error = GetLastError();
      LocalFree(detail);
      continue;
    }

    handle_ = CreateFileW(
      detail->DevicePath,
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
    LocalFree(detail);
    if (handle_ == INVALID_HANDLE_VALUE) {
      last_error = GetLastError();
      continue;
    }

    const DWORD handshake = query_info();
    if (handshake == ERROR_SUCCESS) {
      last_error = ERROR_SUCCESS;
      break;
    }
    last_error = handshake;
    close();
  }

  SetupDiDestroyDeviceInfoList(devices);
  return last_error;
}

void client::close() noexcept {
  if (handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
  info_ = {};
}

bool client::connected() const noexcept {
  return handle_ != INVALID_HANDLE_VALUE;
}

profile_mask_t client::available_profiles() const noexcept {
  return info_.available_profiles;
}

std::uint32_t client::available_features() const noexcept {
  return info_.available_features;
}

std::uint32_t client::maximum_controllers() const noexcept {
  return info_.maximum_controllers;
}

DWORD client::create_controller(
  const std::uint32_t controller_id,
  const profile requested_profile) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }
  if (controller_id >= maximum_controllers()) {
    return ERROR_INVALID_PARAMETER;
  }
  if ((available_profiles() & profile_bit(requested_profile)) == 0) {
    return ERROR_NOT_SUPPORTED;
  }
  create_controller_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;
  request.requested_profile = requested_profile;
  return issue(ioctl_create_controller, &request, sizeof(request), nullptr, 0);
}

DWORD client::destroy_controller(const std::uint32_t controller_id) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }
  if (controller_id >= maximum_controllers()) {
    return ERROR_INVALID_PARAMETER;
  }
  controller_id_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;
  return issue(ioctl_destroy_controller, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_input_state(const input_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_input_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_touch_state(const touch_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_touch_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_motion_state(const motion_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved0[0] != 0 ||
      input.reserved0[1] != 0 || input.reserved0[2] != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_motion_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_battery_state(const battery_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_battery_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::poll_feedback(
  const std::uint32_t controller_id,
  feedback_event *const event) noexcept {
  if (event == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }

  controller_id_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;

  DWORD bytes = 0;
  const DWORD status = issue(
    ioctl_poll_feedback,
    &request,
    sizeof(request),
    event,
    sizeof(*event),
    &bytes);
  if (status != ERROR_SUCCESS) {
    return status;
  }
  if (bytes != sizeof(*event) || !valid_request(event, bytes) ||
      event->controller_id != controller_id ||
      event->payload_size > sizeof(event->payload)) {
    return ERROR_INVALID_DATA;
  }
  return ERROR_SUCCESS;
}

DWORD client::query_info() noexcept {
  query_info_request request {};
  initialize_header(&request.header, sizeof(request));

  query_info_response response {};
  DWORD bytes = 0;
  const DWORD status = issue(
    ioctl_query_info,
    &request,
    sizeof(request),
    &response,
    sizeof(response),
    &bytes);
  if (status != ERROR_SUCCESS) {
    return status;
  }
  if (bytes != sizeof(response) || !valid_request(&response, bytes) ||
      response.minimum_protocol_version > k_protocol_version ||
      response.maximum_protocol_version < k_protocol_version ||
      response.maximum_controllers == 0 || response.maximum_controllers > k_max_controllers ||
      response.reserved != 0) {
    return ERROR_REVISION_MISMATCH;
  }
  info_ = response;
  return ERROR_SUCCESS;
}

DWORD client::issue(
  const DWORD control_code,
  void *const input,
  const DWORD input_size,
  void *const output,
  const DWORD output_size,
  DWORD *const bytes_returned) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }

  DWORD bytes = 0;
  if (!DeviceIoControl(
        handle_,
        control_code,
        input,
        input_size,
        output,
        output_size,
        &bytes,
        nullptr)) {
    const DWORD error = GetLastError();
    // The driver pins every request to its own protocol version and size. We
    // built every buffer ourselves, so the only way one is "invalid" is a driver
    // from a different protocol generation: report that rather than a buffer
    // fault, on every IOCTL and not only the handshake.
    return error == ERROR_INVALID_USER_BUFFER ? ERROR_REVISION_MISMATCH : error;
  }
  if (bytes_returned != nullptr) {
    *bytes_returned = bytes;
  }
  return ERROR_SUCCESS;
}

}  // namespace lvg
