// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// The VHF source driver is a root-enumerated device. This tool owns only the
// ROOT\VIBESHINEVIRTUALGAMEPAD source node; it never touches physical HID
// devices or third-party virtual-controller packages.

#include <windows.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <newdev.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <exception>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libvirtualgamepad/protocol.h"

#include "install_decision.h"

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "setupapi.lib")

namespace {

constexpr wchar_t k_device_description[] = L"Vibeshine Virtual Gamepad";
constexpr std::wstring_view k_root_enumerator_prefix = L"ROOT\\";
constexpr std::wstring_view k_root_device_id = L"VIBESHINEVIRTUALGAMEPAD";
constexpr std::wstring_view k_root_instance_prefix = L"ROOT\\VIBESHINEVIRTUALGAMEPAD\\";
constexpr DWORD k_interface_wait_milliseconds = 10000;
constexpr DWORD k_interface_poll_milliseconds = 200;
constexpr DWORD k_protocol_open_timeout_milliseconds = 1000;
constexpr DWORD k_protocol_query_timeout_milliseconds = 1000;
constexpr DWORD k_protocol_cancel_timeout_milliseconds = 1000;
constexpr int k_exit_reboot_required = 3010;

// DEVPKEY_Device_DriverVersion is declared by devpkey.h rather than exported
// by a system library. Keep the one property key we use local to this helper
// so the helper has no INITGUID/link-time dependency.
constexpr DEVPROPKEY k_device_driver_version {
  {0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}},
  3,
};

static_assert(std::wstring_view(lvg::k_root_hardware_id).starts_with(k_root_enumerator_prefix));
static_assert(
    std::wstring_view(lvg::k_root_hardware_id).substr(k_root_enumerator_prefix.size()) == k_root_device_id);

class device_info_set {
 public:
  explicit device_info_set(HDEVINFO value):
      value_ {value} {}

  device_info_set(const device_info_set &) = delete;
  device_info_set &operator=(const device_info_set &) = delete;

  device_info_set(device_info_set &&other) noexcept:
      value_ {std::exchange(other.value_, INVALID_HANDLE_VALUE)} {}

  device_info_set &operator=(device_info_set &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }

  ~device_info_set() {
    reset();
  }

  [[nodiscard]] HDEVINFO get() const noexcept {
    return value_;
  }

 private:
  void reset() noexcept {
    if (value_ != INVALID_HANDLE_VALUE) {
      SetupDiDestroyDeviceInfoList(value_);
      value_ = INVALID_HANDLE_VALUE;
    }
  }

  HDEVINFO value_ {INVALID_HANDLE_VALUE};
};

[[noreturn]] void throw_last_error(std::string_view operation) {
  throw std::runtime_error(std::string(operation) + " failed with Win32 error " + std::to_string(GetLastError()));
}

[[nodiscard]] bool is_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    throw_last_error("OpenProcessToken");
  }

  TOKEN_ELEVATION elevation {};
  DWORD returned = 0;
  const bool result = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE;
  const DWORD error = result ? ERROR_SUCCESS : GetLastError();
  CloseHandle(token);
  if (!result) {
    SetLastError(error);
    throw_last_error("GetTokenInformation(TokenElevation)");
  }
  return elevation.TokenIsElevated != 0;
}

void require_elevation() {
  if (!is_elevated()) {
    throw std::runtime_error("install and remove require an elevated Administrator session");
  }
}

[[nodiscard]] std::wstring absolute_existing_path(const std::wstring &path) {
  const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0) {
    throw_last_error("GetFullPathNameW");
  }

  std::vector<wchar_t> buffer(required);
  const DWORD copied = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  if (copied == 0 || copied >= buffer.size()) {
    throw_last_error("GetFullPathNameW");
  }

  std::wstring absolute_path(buffer.data(), copied);
  const DWORD attributes = GetFileAttributesW(absolute_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    throw std::runtime_error("INF path is not a regular file");
  }
  return absolute_path;
}

void validate_owned_inf_path(const std::wstring &absolute_path) {
  const auto separator = absolute_path.find_last_of(L"\\/");
  const std::wstring file_name = absolute_path.substr(separator == std::wstring::npos ? 0 : separator + 1);
  if (_wcsicmp(file_name.c_str(), L"VibeshineVhfGamepad.inf") != 0) {
    throw std::runtime_error("refusing an INF that is not named VibeshineVhfGamepad.inf");
  }
}

[[nodiscard]] std::wstring trim_driver_version(std::wstring value) {
  const auto first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(L" \t\r\n");
  value = value.substr(first, last - first + 1);
  const auto separator = value.find(L',');
  if (separator != std::wstring::npos) {
    value = value.substr(separator + 1);
    const auto version_first = value.find_first_not_of(L" \t\r\n");
    if (version_first == std::wstring::npos) {
      return {};
    }
    value = value.substr(version_first);
  }
  return value;
}

[[nodiscard]] std::wstring driver_version_from_inf(const std::wstring &inf_path) {
  const HINF inf = SetupOpenInfFileW(inf_path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
  if (inf == INVALID_HANDLE_VALUE) {
    throw_last_error("SetupOpenInfFileW");
  }

  try {
    INFCONTEXT context {};
    if (!SetupFindFirstLineW(inf, L"Version", L"DriverVer", &context)) {
      throw_last_error("SetupFindFirstLineW(DriverVer)");
    }
    DWORD characters = 0;
    // DriverVer has two INF fields: date,version. The version is the value
    // Windows publishes through DEVPKEY_Device_DriverVersion.
    const BOOL size_query_succeeded = SetupGetStringFieldW(&context, 2, nullptr, 0, &characters);
    if (!size_query_succeeded && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_last_error("SetupGetStringFieldW(DriverVer size)");
    }
    if (characters == 0) {
      throw std::runtime_error("VibeshineVhfGamepad.inf has an empty DriverVer field");
    }
    std::vector<wchar_t> buffer(characters);
    if (!SetupGetStringFieldW(&context, 2, buffer.data(), static_cast<DWORD>(buffer.size()), &characters)) {
      throw_last_error("SetupGetStringFieldW(DriverVer)");
    }
    const std::wstring version = trim_driver_version(buffer.data());
    if (version.empty()) {
      throw std::runtime_error("VibeshineVhfGamepad.inf has an empty DriverVer version");
    }
    SetupCloseInfFile(inf);
    return version;
  } catch (...) {
    SetupCloseInfFile(inf);
    throw;
  }
}

[[nodiscard]] bool device_has_our_hardware_id(HDEVINFO device_set, SP_DEVINFO_DATA *device) {
  DWORD property_type = 0;
  DWORD required_bytes = 0;
  if (SetupDiGetDeviceRegistryPropertyW(
          device_set,
          device,
          SPDRP_HARDWAREID,
          &property_type,
          nullptr,
          0,
          &required_bytes)) {
    return false;
  }

  const DWORD error = GetLastError();
  if ((error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA) || required_bytes < sizeof(wchar_t) * 2) {
    return false;
  }

  std::vector<wchar_t> values(required_bytes / sizeof(wchar_t));
  if (!SetupDiGetDeviceRegistryPropertyW(
          device_set,
          device,
          SPDRP_HARDWAREID,
          &property_type,
          reinterpret_cast<PBYTE>(values.data()),
          required_bytes,
          &required_bytes) ||
      property_type != REG_MULTI_SZ) {
    return false;
  }

  const wchar_t *cursor = values.data();
  const wchar_t *const end = values.data() + values.size();
  while (cursor < end && *cursor != L'\0') {
    const wchar_t *terminator = cursor;
    while (terminator < end && *terminator != L'\0') {
      ++terminator;
    }
    if (terminator == end) {
      return false;
    }

    const std::wstring value(cursor, terminator);
    if (_wcsicmp(value.c_str(), lvg::k_root_hardware_id) == 0) {
      return true;
    }
    cursor = terminator + 1;
  }

  return false;
}

[[nodiscard]] std::wstring device_instance_id(HDEVINFO device_set, SP_DEVINFO_DATA *device) {
  DWORD characters = 0;
  if (SetupDiGetDeviceInstanceIdW(device_set, device, nullptr, 0, &characters)) {
    return {};
  }
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || characters == 0) {
    throw_last_error("SetupDiGetDeviceInstanceIdW");
  }

  std::vector<wchar_t> buffer(characters);
  if (!SetupDiGetDeviceInstanceIdW(device_set, device, buffer.data(), static_cast<DWORD>(buffer.size()), &characters)) {
    throw_last_error("SetupDiGetDeviceInstanceIdW");
  }
  return buffer.data();
}

[[nodiscard]] bool device_is_from_root_enumerator(HDEVINFO device_set, SP_DEVINFO_DATA *device) {
  DWORD property_type = 0;
  DWORD required_bytes = 0;
  if (SetupDiGetDeviceRegistryPropertyW(
          device_set,
          device,
          SPDRP_ENUMERATOR_NAME,
          &property_type,
          nullptr,
          0,
          &required_bytes)) {
    return false;
  }

  const DWORD error = GetLastError();
  if ((error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA) ||
      required_bytes < sizeof(wchar_t)) {
    return false;
  }

  std::vector<wchar_t> value((required_bytes / sizeof(wchar_t)) + 1, L'\0');
  if (!SetupDiGetDeviceRegistryPropertyW(
          device_set,
          device,
          SPDRP_ENUMERATOR_NAME,
          &property_type,
          reinterpret_cast<PBYTE>(value.data()),
          required_bytes,
          &required_bytes) ||
      property_type != REG_SZ) {
    return false;
  }
  return _wcsicmp(value.data(), L"ROOT") == 0;
}

[[nodiscard]] bool is_our_root_device(HDEVINFO device_set, SP_DEVINFO_DATA *device) {
  if (!device_has_our_hardware_id(device_set, device) || !device_is_from_root_enumerator(device_set, device)) {
    return false;
  }

  const std::wstring instance_id = device_instance_id(device_set, device);
  return instance_id.size() >= k_root_instance_prefix.size() &&
         _wcsnicmp(instance_id.c_str(), k_root_instance_prefix.data(), k_root_instance_prefix.size()) == 0;
}

[[nodiscard]] std::wstring device_driver_version(HDEVINFO device_set, SP_DEVINFO_DATA *device) {
  DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
  DWORD required_bytes = 0;
  if (SetupDiGetDevicePropertyW(
          device_set,
          device,
          &k_device_driver_version,
          &property_type,
          nullptr,
          0,
          &required_bytes,
          0) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || required_bytes < sizeof(wchar_t)) {
    throw_last_error("SetupDiGetDevicePropertyW(DEVPKEY_Device_DriverVersion size)");
  }
  if (property_type != DEVPROP_TYPE_STRING) {
    throw std::runtime_error("DEVPKEY_Device_DriverVersion is not a string");
  }

  std::vector<wchar_t> buffer((required_bytes / sizeof(wchar_t)) + 1, L'\0');
  if (!SetupDiGetDevicePropertyW(
          device_set,
          device,
          &k_device_driver_version,
          &property_type,
          reinterpret_cast<PBYTE>(buffer.data()),
          required_bytes,
          &required_bytes,
          0) ||
      property_type != DEVPROP_TYPE_STRING) {
    throw_last_error("SetupDiGetDevicePropertyW(DEVPKEY_Device_DriverVersion)");
  }
  return trim_driver_version(buffer.data());
}

[[nodiscard]] bool owned_root_devices_match_driver_version(const std::wstring &expected_version) {
  device_info_set device_set {SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES)};
  if (device_set.get() == INVALID_HANDLE_VALUE) {
    return false;
  }

  bool found = false;
  for (DWORD index = 0;; ++index) {
    SP_DEVINFO_DATA device {};
    device.cbSize = sizeof(device);
    if (!SetupDiEnumDeviceInfo(device_set.get(), index, &device)) {
      if (GetLastError() == ERROR_NO_MORE_ITEMS) {
        break;
      }
      return false;
    }
    if (!is_our_root_device(device_set.get(), &device)) {
      continue;
    }
    found = true;
    try {
      const std::wstring actual_version = device_driver_version(device_set.get(), &device);
      if (_wcsicmp(actual_version.c_str(), expected_version.c_str()) != 0) {
        return false;
      }
    } catch (...) {
      return false;
    }
  }
  return found;
}

struct root_device_state {
  std::wstring instance_id;
  ULONG status {};
  ULONG problem {};
};

[[nodiscard]] std::vector<root_device_state> enumerate_root_devices() {
  device_info_set device_set {SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES)};
  if (device_set.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("SetupDiGetClassDevsW");
  }

  std::vector<root_device_state> devices;
  for (DWORD index = 0;; ++index) {
    SP_DEVINFO_DATA device {};
    device.cbSize = sizeof(device);
    if (!SetupDiEnumDeviceInfo(device_set.get(), index, &device)) {
      if (GetLastError() == ERROR_NO_MORE_ITEMS) {
        break;
      }
      throw_last_error("SetupDiEnumDeviceInfo");
    }

    if (!is_our_root_device(device_set.get(), &device)) {
      continue;
    }

    root_device_state state {};
    state.instance_id = device_instance_id(device_set.get(), &device);
    const CONFIGRET result = CM_Get_DevNode_Status(&state.status, &state.problem, device.DevInst, 0);
    if (result != CR_SUCCESS) {
      state.status = 0;
      state.problem = static_cast<ULONG>(result);
    }
    devices.push_back(std::move(state));
  }

  return devices;
}

struct protocol_query_probe {
  std::atomic<HANDLE> device_handle {INVALID_HANDLE_VALUE};
  HANDLE completion_event {nullptr};
  HANDLE open_thread {nullptr};
  std::wstring device_path;
  OVERLAPPED overlapped {};
  lvg::query_info_request request {};
  lvg::query_info_response response {};

  ~protocol_query_probe() {
    if (completion_event != nullptr) {
      CloseHandle(completion_event);
    }
    if (open_thread != nullptr) {
      CloseHandle(open_thread);
    }
    const HANDLE handle = device_handle.exchange(INVALID_HANDLE_VALUE);
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

DWORD WINAPI open_protocol_device_worker(void *context) {
  auto *probe = static_cast<protocol_query_probe *>(context);
  probe->device_handle.store(CreateFileW(
      probe->device_path.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
      nullptr));
  return 0;
}

// A broken UMDF endpoint can ignore CreateFile or overlapped-I/O cancellation.
// Retain at most one such probe until it completes, rather than returning while
// its worker/request/response storage is still in use. The setup helper is
// short-lived, so a non-completing probe is safely reclaimed by process teardown.
protocol_query_probe *g_pending_protocol_query_probe = nullptr;

void reap_completed_protocol_query_probe() {
  if (g_pending_protocol_query_probe == nullptr) {
    return;
  }

  if (g_pending_protocol_query_probe->open_thread != nullptr) {
    if (WaitForSingleObject(g_pending_protocol_query_probe->open_thread, 0) != WAIT_OBJECT_0) {
      return;
    }
    // The open worker has finished. Its handle may now be closed; this late
    // completion is intentionally discarded and the next readiness attempt
    // will begin a fresh, bounded protocol query.
    CloseHandle(g_pending_protocol_query_probe->open_thread);
    g_pending_protocol_query_probe->open_thread = nullptr;
    delete g_pending_protocol_query_probe;
    g_pending_protocol_query_probe = nullptr;
    return;
  }

  if (WaitForSingleObject(g_pending_protocol_query_probe->completion_event, 0) != WAIT_OBJECT_0) {
    return;
  }

  DWORD ignored_bytes = 0;
  static_cast<void>(GetOverlappedResult(
      g_pending_protocol_query_probe->device_handle.load(),
      &g_pending_protocol_query_probe->overlapped,
      &ignored_bytes,
      FALSE));
  delete g_pending_protocol_query_probe;
  g_pending_protocol_query_probe = nullptr;
}

[[nodiscard]] bool protocol_query_response_is_valid(const lvg::query_info_response &response, const DWORD bytes_returned) {
  return bytes_returned == sizeof(response) &&
         lvg::valid_request(&response, bytes_returned) &&
         response.minimum_protocol_version <= lvg::k_protocol_version &&
         response.maximum_protocol_version >= lvg::k_protocol_version &&
         response.maximum_controllers > 0 && response.maximum_controllers <= lvg::k_max_controllers &&
         response.reserved == 0;
}

[[nodiscard]] bool complete_protocol_query_probe(protocol_query_probe *probe) {
  DWORD bytes_returned = 0;
  const bool completed = GetOverlappedResult(
      probe->device_handle.load(),
      &probe->overlapped,
      &bytes_returned,
      FALSE) != FALSE;
  const bool valid = completed && protocol_query_response_is_valid(probe->response, bytes_returned);
  delete probe;
  return valid;
}

[[nodiscard]] bool source_interface_accepts_protocol(const wchar_t *device_path) {
  reap_completed_protocol_query_probe();
  if (g_pending_protocol_query_probe != nullptr) {
    return false;
  }

  auto *probe = new protocol_query_probe {};
  probe->device_path = device_path;
  probe->open_thread = CreateThread(nullptr, 0, open_protocol_device_worker, probe, 0, nullptr);
  if (probe->open_thread == nullptr) {
    delete probe;
    return false;
  }
  if (WaitForSingleObject(probe->open_thread, k_protocol_open_timeout_milliseconds) != WAIT_OBJECT_0) {
    static_cast<void>(CancelSynchronousIo(probe->open_thread));
    if (WaitForSingleObject(probe->open_thread, k_protocol_cancel_timeout_milliseconds) != WAIT_OBJECT_0) {
      // CreateFile can block while a broken UMDF stack processes IRP_MJ_CREATE.
      // Keep the worker-owned data alive until it returns, and keep this setup
      // helper bounded rather than blocking the MSI on that endpoint.
      g_pending_protocol_query_probe = probe;
      return false;
    }
  }
  CloseHandle(probe->open_thread);
  probe->open_thread = nullptr;
  if (probe->device_handle.load() == INVALID_HANDLE_VALUE) {
    delete probe;
    return false;
  }
  probe->completion_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (probe->completion_event == nullptr) {
    delete probe;
    return false;
  }
  probe->overlapped.hEvent = probe->completion_event;
  probe->request.header.size = sizeof(probe->request);
  probe->request.header.version = lvg::k_protocol_version;

  DWORD bytes_returned = 0;
  if (DeviceIoControl(
          probe->device_handle.load(),
          lvg::ioctl_query_info,
          &probe->request,
          sizeof(probe->request),
          &probe->response,
          sizeof(probe->response),
          &bytes_returned,
          &probe->overlapped)) {
    const bool valid = protocol_query_response_is_valid(probe->response, bytes_returned);
    delete probe;
    return valid;
  }
  if (GetLastError() != ERROR_IO_PENDING) {
    delete probe;
    return false;
  }

  if (WaitForSingleObject(probe->completion_event, k_protocol_query_timeout_milliseconds) == WAIT_OBJECT_0) {
    return complete_protocol_query_probe(probe);
  }

  static_cast<void>(CancelIoEx(probe->device_handle.load(), &probe->overlapped));
  if (WaitForSingleObject(probe->completion_event, k_protocol_cancel_timeout_milliseconds) == WAIT_OBJECT_0) {
    return complete_protocol_query_probe(probe);
  }

  g_pending_protocol_query_probe = probe;
  return false;
}

[[nodiscard]] bool source_device_interface_ready() {
  device_info_set interfaces {
      SetupDiGetClassDevsW(
          &lvg::k_device_interface_guid,
          nullptr,
          nullptr,
          DIGCF_DEVICEINTERFACE | DIGCF_PRESENT)};
  if (interfaces.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_NO_SUCH_DEVINST || error == ERROR_FILE_NOT_FOUND) {
      return false;
    }
    SetLastError(error);
    throw_last_error("SetupDiGetClassDevsW(source interface)");
  }

  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interface_data {};
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(
            interfaces.get(),
            nullptr,
            &lvg::k_device_interface_guid,
            index,
            &interface_data)) {
      const DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_ITEMS) {
        return false;
      }
      SetLastError(error);
      throw_last_error("SetupDiEnumDeviceInterfaces(source interface)");
    }

    DWORD detail_size = 0;
    if (SetupDiGetDeviceInterfaceDetailW(
            interfaces.get(),
            &interface_data,
            nullptr,
            0,
            &detail_size,
            nullptr) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        detail_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
      throw_last_error("SetupDiGetDeviceInterfaceDetailW(source interface size)");
    }

    std::vector<std::byte> detail_buffer(detail_size);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detail_buffer.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA device {};
    device.cbSize = sizeof(device);
    if (!SetupDiGetDeviceInterfaceDetailW(
            interfaces.get(),
            &interface_data,
            detail,
            detail_size,
            nullptr,
            &device)) {
      throw_last_error("SetupDiGetDeviceInterfaceDetailW(source interface)");
    }

    if (is_our_root_device(interfaces.get(), &device) && source_interface_accepts_protocol(detail->DevicePath)) {
      return true;
    }
  }
}

[[nodiscard]] bool wait_for_source_device_interface() {
  const DWORD attempts = k_interface_wait_milliseconds / k_interface_poll_milliseconds;
  for (DWORD attempt = 0; attempt <= attempts; ++attempt) {
    if (source_device_interface_ready()) {
      return true;
    }
    if (attempt != attempts) {
      Sleep(k_interface_poll_milliseconds);
    }
  }
  return false;
}

[[nodiscard]] bool device_install_requires_reboot(HDEVINFO device_set, SP_DEVINFO_DATA *device) {
  SP_DEVINSTALL_PARAMS_W parameters {};
  parameters.cbSize = sizeof(parameters);
  if (!SetupDiGetDeviceInstallParamsW(device_set, device, &parameters)) {
    throw_last_error("SetupDiGetDeviceInstallParamsW");
  }
  return (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
}

[[nodiscard]] bool remove_open_device(HDEVINFO device_set, SP_DEVINFO_DATA *device) {
  SP_REMOVEDEVICE_PARAMS parameters {};
  parameters.ClassInstallHeader.cbSize = sizeof(parameters.ClassInstallHeader);
  parameters.ClassInstallHeader.InstallFunction = DIF_REMOVE;
  parameters.Scope = DI_REMOVEDEVICE_GLOBAL;
  parameters.HwProfile = 0;
  if (!SetupDiSetClassInstallParamsW(device_set, device, &parameters.ClassInstallHeader, sizeof(parameters))) {
    throw_last_error("SetupDiSetClassInstallParamsW(DIF_REMOVE)");
  }
  if (!SetupDiCallClassInstaller(DIF_REMOVE, device_set, device)) {
    throw_last_error("SetupDiCallClassInstaller(DIF_REMOVE)");
  }
  return device_install_requires_reboot(device_set, device);
}

struct created_root_device {
  std::wstring instance_id;
  bool reboot_required {};
};

[[nodiscard]] created_root_device create_root_device(const std::wstring &inf_path) {
  GUID class_guid {};
  wchar_t class_name[MAX_CLASS_NAME_LEN] {};
  DWORD required = 0;
  if (!SetupDiGetINFClassW(
          inf_path.c_str(),
          &class_guid,
          class_name,
          static_cast<DWORD>(std::size(class_name)),
          &required)) {
    throw_last_error("SetupDiGetINFClassW");
  }

  device_info_set device_set {SetupDiCreateDeviceInfoList(&class_guid, nullptr)};
  if (device_set.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("SetupDiCreateDeviceInfoList");
  }

  SP_DEVINFO_DATA device {};
  device.cbSize = sizeof(device);
  if (!SetupDiCreateDeviceInfoW(
          device_set.get(),
          k_root_device_id.data(),
          &class_guid,
          k_device_description,
          nullptr,
          DICD_GENERATE_ID,
          &device)) {
    throw_last_error("SetupDiCreateDeviceInfoW");
  }

  std::vector<wchar_t> hardware_ids(std::wcslen(lvg::k_root_hardware_id) + 2, L'\0');
  std::copy_n(lvg::k_root_hardware_id, std::wcslen(lvg::k_root_hardware_id), hardware_ids.data());
  if (!SetupDiSetDeviceRegistryPropertyW(
          device_set.get(),
          &device,
          SPDRP_HARDWAREID,
          reinterpret_cast<PBYTE>(hardware_ids.data()),
          static_cast<DWORD>(hardware_ids.size() * sizeof(wchar_t)))) {
    throw_last_error("SetupDiSetDeviceRegistryPropertyW(SPDRP_HARDWAREID)");
  }

  if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, device_set.get(), &device)) {
    throw_last_error("SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
  }
  return {
      .instance_id = device_instance_id(device_set.get(), &device),
      .reboot_required = device_install_requires_reboot(device_set.get(), &device),
  };
}

struct driver_update_result {
  bool updated {};
  bool reboot_required {};
};

[[nodiscard]] driver_update_result update_driver(const std::wstring &inf_path) {
  BOOL reboot_required = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(
          nullptr,
          lvg::k_root_hardware_id,
          inf_path.c_str(),
          0,
          &reboot_required)) {
    const DWORD error = GetLastError();
    // With zero install flags Windows correctly declines to downgrade an
    // equal/newer already-selected package. The source node still exists and
    // may already be ready, so this is not an installation failure.
    if (error == ERROR_NO_MORE_ITEMS) {
      return {};
    }
    SetLastError(error);
    throw_last_error("UpdateDriverForPlugAndPlayDevicesW");
  }
  return {.updated = true, .reboot_required = reboot_required != FALSE};
}

struct remove_device_result {
  bool removed {};
  bool reboot_required {};
};

struct reactivate_device_result {
  bool reenumerated {};
  bool restarted {};
  bool reboot_required {};
};

[[nodiscard]] remove_device_result remove_device_instance(const std::wstring &instance_id) {
  device_info_set device_set {SetupDiCreateDeviceInfoList(nullptr, nullptr)};
  if (device_set.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("SetupDiCreateDeviceInfoList");
  }

  SP_DEVINFO_DATA device {};
  device.cbSize = sizeof(device);
  if (!SetupDiOpenDeviceInfoW(device_set.get(), instance_id.c_str(), nullptr, 0, &device)) {
    const DWORD error = GetLastError();
    if (error == ERROR_NO_SUCH_DEVINST || error == ERROR_FILE_NOT_FOUND) {
      return {};
    }
    SetLastError(error);
    throw_last_error("SetupDiOpenDeviceInfoW");
  }

  if (!is_our_root_device(device_set.get(), &device)) {
    throw std::runtime_error("refusing to remove a device that is not the Vibeshine root source node");
  }
  return {.removed = true, .reboot_required = remove_open_device(device_set.get(), &device)};
}

// A driver installation flag is a request to reboot, not proof that the VHF
// source stack cannot start in this session. Before surfacing that fallback,
// ask PnP to rediscover and restart only the root node we own, then require the
// private source interface as the success proof.
[[nodiscard]] reactivate_device_result reactivate_device_instance(const std::wstring &instance_id) {
  DEVINST dev_inst {};
  const CONFIGRET locate_result = CM_Locate_DevNodeW(
      &dev_inst,
      const_cast<wchar_t *>(instance_id.c_str()),
      CM_LOCATE_DEVNODE_NORMAL);
  if (locate_result != CR_SUCCESS) {
    throw std::runtime_error("CM_Locate_DevNodeW failed with CONFIGRET " + std::to_string(locate_result));
  }
  const CONFIGRET reenumerate_result = CM_Reenumerate_DevNode(dev_inst, CM_REENUMERATE_SYNCHRONOUS);
  if (reenumerate_result != CR_SUCCESS) {
    throw std::runtime_error("CM_Reenumerate_DevNode failed with CONFIGRET " + std::to_string(reenumerate_result));
  }

  device_info_set device_set {SetupDiCreateDeviceInfoList(nullptr, nullptr)};
  if (device_set.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("SetupDiCreateDeviceInfoList");
  }

  SP_DEVINFO_DATA device {};
  device.cbSize = sizeof(device);
  if (!SetupDiOpenDeviceInfoW(device_set.get(), instance_id.c_str(), nullptr, 0, &device)) {
    throw_last_error("SetupDiOpenDeviceInfoW");
  }
  if (!is_our_root_device(device_set.get(), &device)) {
    throw std::runtime_error("refusing to restart a device that is not the Vibeshine root source node");
  }

  SP_PROPCHANGE_PARAMS parameters {};
  parameters.ClassInstallHeader.cbSize = sizeof(parameters.ClassInstallHeader);
  parameters.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
  parameters.StateChange = DICS_PROPCHANGE;
  parameters.Scope = DICS_FLAG_GLOBAL;
  parameters.HwProfile = 0;
  if (!SetupDiSetClassInstallParamsW(
          device_set.get(),
          &device,
          &parameters.ClassInstallHeader,
          sizeof(parameters))) {
    throw_last_error("SetupDiSetClassInstallParamsW(DIF_PROPERTYCHANGE)");
  }
  if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, device_set.get(), &device)) {
    throw_last_error("SetupDiCallClassInstaller(DIF_PROPERTYCHANGE)");
  }

  return {
      .reenumerated = true,
      .restarted = true,
      .reboot_required = device_install_requires_reboot(device_set.get(), &device),
  };
}

struct reactivate_root_devices_result {
  DWORD reenumerated {};
  DWORD restarted {};
  bool reboot_required {};
};

[[nodiscard]] reactivate_root_devices_result reactivate_owned_root_devices() {
  reactivate_root_devices_result result {};
  for (const auto &device : enumerate_root_devices()) {
    const auto reactivated = reactivate_device_instance(device.instance_id);
    result.reenumerated += reactivated.reenumerated ? 1 : 0;
    result.restarted += reactivated.restarted ? 1 : 0;
    result.reboot_required = result.reboot_required || reactivated.reboot_required;
  }
  return result;
}

// DICS_PROPCHANGE is the least disruptive PnP nudge. If Windows has left the
// UMDF source stack inert after that nudge, a single owned-node disable/enable
// cycle is the final live-activation tier. Never apply this to a class, a HID
// child, or a shared WUDFHost process.
struct cycle_device_result {
  bool disabled {};
  bool enabled {};
  bool reboot_required {};
  bool recovery_failed {};
  std::string failure_detail;
};

void issue_owned_device_state_change(
    HDEVINFO device_set,
    SP_DEVINFO_DATA *device,
    DWORD state_change,
    bool *operation_may_have_run = nullptr) {
  SP_PROPCHANGE_PARAMS parameters {};
  parameters.ClassInstallHeader.cbSize = sizeof(parameters.ClassInstallHeader);
  parameters.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
  parameters.StateChange = state_change;
  parameters.Scope = DICS_FLAG_GLOBAL;
  parameters.HwProfile = 0;
  if (!SetupDiSetClassInstallParamsW(
          device_set,
          device,
          &parameters.ClassInstallHeader,
          sizeof(parameters))) {
    throw_last_error("SetupDiSetClassInstallParamsW(DIF_PROPERTYCHANGE)");
  }
  // A class/co-installer can apply a state transition and still return a
  // failure from post-processing. Let a DICS_DISABLE caller recover that
  // exact owned node even when SetupDiCallClassInstaller reports failure.
  if (operation_may_have_run != nullptr) {
    *operation_may_have_run = true;
  }
  if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, device_set, device)) {
    throw_last_error("SetupDiCallClassInstaller(DIF_PROPERTYCHANGE)");
  }
}

[[nodiscard]] cycle_device_result cycle_owned_root_device_instance(const std::wstring &instance_id) {
  device_info_set device_set {SetupDiCreateDeviceInfoList(nullptr, nullptr)};
  if (device_set.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("SetupDiCreateDeviceInfoList");
  }

  SP_DEVINFO_DATA device {};
  device.cbSize = sizeof(device);
  if (!SetupDiOpenDeviceInfoW(device_set.get(), instance_id.c_str(), nullptr, 0, &device)) {
    throw_last_error("SetupDiOpenDeviceInfoW");
  }
  if (!is_our_root_device(device_set.get(), &device)) {
    throw std::runtime_error("refusing to cycle a device that is not the Vibeshine root source node");
  }

  cycle_device_result result {};
  bool disable_may_have_run = false;
  const auto restore_after_failure = [&]() {
    if (!result.disabled) {
      return;
    }
    try {
      issue_owned_device_state_change(device_set.get(), &device, DICS_ENABLE);
      result.enabled = true;
      result.reboot_required = device_install_requires_reboot(device_set.get(), &device) || result.reboot_required;
    } catch (...) {
      result.failure_detail += "; exact-node re-enable attempt also failed";
    }
  };
  try {
    issue_owned_device_state_change(
        device_set.get(),
        &device,
        DICS_DISABLE,
        &disable_may_have_run);
    result.disabled = true;
    result.reboot_required = device_install_requires_reboot(device_set.get(), &device);
    issue_owned_device_state_change(device_set.get(), &device, DICS_ENABLE);
    result.enabled = true;
    result.reboot_required = device_install_requires_reboot(device_set.get(), &device) || result.reboot_required;
  } catch (const std::exception &error) {
    // A failed recovery must never strand an existing owned source device in
    // the disabled state. Retry enable once before reporting the original
    // error; any retry failure is secondary to the operation that failed.
    result.disabled = result.disabled || disable_may_have_run;
    result.recovery_failed = true;
    result.failure_detail = error.what();
    restore_after_failure();
    return result;
  } catch (...) {
    result.disabled = result.disabled || disable_may_have_run;
    result.recovery_failed = true;
    result.failure_detail = "owned source-device disable/enable operation failed";
    restore_after_failure();
    return result;
  }
  return result;
}

struct cycle_root_devices_result {
  DWORD disabled {};
  DWORD enabled {};
  bool reboot_required {};
  bool recovery_failed {};
  std::string failure_detail;
};

[[nodiscard]] cycle_root_devices_result cycle_owned_root_devices() {
  cycle_root_devices_result result {};
  for (const auto &device : enumerate_root_devices()) {
    const auto cycled = cycle_owned_root_device_instance(device.instance_id);
    result.disabled += cycled.disabled ? 1 : 0;
    result.enabled += cycled.enabled ? 1 : 0;
    result.reboot_required = result.reboot_required || cycled.reboot_required;
    if (cycled.recovery_failed) {
      result.recovery_failed = true;
      if (result.failure_detail.empty()) {
        result.failure_detail = cycled.failure_detail;
      }
    }
  }
  return result;
}

[[nodiscard]] std::wstring json_escape(std::wstring_view value) {
  std::wstring escaped;
  escaped.reserve(value.size());
  for (const wchar_t character : value) {
    switch (character) {
      case L'\\':
        escaped += L"\\\\";
        break;
      case L'\"':
        escaped += L"\\\"";
        break;
      case L'\b':
        escaped += L"\\b";
        break;
      case L'\f':
        escaped += L"\\f";
        break;
      case L'\n':
        escaped += L"\\n";
        break;
      case L'\r':
        escaped += L"\\r";
        break;
      case L'\t':
        escaped += L"\\t";
        break;
      default:
        escaped += character < 0x20 ? L'?' : character;
        break;
    }
  }
  return escaped;
}

[[nodiscard]] const wchar_t *problem_code_name(const ULONG problem) {
  switch (problem) {
    case 0:
      return L"none";
    case 1:
      return L"CM_PROB_NOT_CONFIGURED";
    case 10:
      return L"CM_PROB_FAILED_START";
    case 18:
      return L"CM_PROB_REINSTALL";
    case 19:
      return L"CM_PROB_REGISTRY";
    case 22:
      return L"CM_PROB_DISABLED";
    case 24:
      return L"CM_PROB_DEVICE_NOT_THERE";
    case 28:
      return L"CM_PROB_FAILED_INSTALL";
    case 31:
      return L"CM_PROB_FAILED_ADD";
    case 37:
      return L"CM_PROB_FAILED_DRIVER_ENTRY";
    case 38:
      return L"CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD";
    case 39:
      return L"CM_PROB_DRIVER_FAILED_LOAD";
    case 41:
      return L"CM_PROB_FAILED_POST_START";
    case 51:
      return L"CM_PROB_WAITING_ON_DEPENDENCY";
    case 52:
      return L"CM_PROB_UNSIGNED_DRIVER";
    default:
      return L"unknown";
  }
}

// Records the PnP state of every owned node while it still exists. Rollback
// deletes the evidence, so an installer log without this line cannot explain
// why the source device never started.
void print_device_diagnostics(const wchar_t *const stage) {
  std::vector<root_device_state> devices;
  try {
    devices = enumerate_root_devices();
  } catch (const std::exception &error) {
    std::cerr << "Vibeshine VHF gamepad diagnostics unavailable: " << error.what() << std::endl;
    return;
  }

  std::wcout << L"{\"diagnostic\":\"" << stage << L"\",\"devices\":[";
  bool first = true;
  for (const auto &device : devices) {
    if (!first) {
      std::wcout << L",";
    }
    first = false;
    std::wcout << L"{\"instance_id\":\"" << json_escape(device.instance_id)
               << L"\",\"device_status\":" << device.status
               << L",\"problem_code\":" << device.problem
               << L",\"problem\":\"" << problem_code_name(device.problem) << L"\"}";
  }
  std::wcout << L"]}" << std::endl;
}

void print_status() {
  const auto devices = enumerate_root_devices();
  const bool ready = source_device_interface_ready();
  std::wcout << L"{\"root_device_count\":" << devices.size()
             << L",\"source_interface_ready\":" << (ready ? L"true" : L"false");
  if (!devices.empty()) {
    std::wcout << L",\"instance_id\":\"" << json_escape(devices.front().instance_id)
               << L"\",\"device_status\":" << devices.front().status
               << L",\"problem_code\":" << devices.front().problem;
  }
  std::wcout << L"}" << std::endl;
}

[[nodiscard]] int run_install(const std::wstring &inf_path) {
  require_elevation();
  const std::wstring absolute_inf = absolute_existing_path(inf_path);
  validate_owned_inf_path(absolute_inf);
  const std::wstring expected_driver_version = driver_version_from_inf(absolute_inf);
  bool created = false;
  std::wstring created_instance;
  bool reboot_required = false;
  if (enumerate_root_devices().empty()) {
    auto created_device = create_root_device(absolute_inf);
    created_instance = std::move(created_device.instance_id);
    reboot_required = created_device.reboot_required;
    created = true;
  }

  driver_update_result update_result {};
  try {
    update_result = update_driver(absolute_inf);
  } catch (...) {
    if (created) {
      try {
        static_cast<void>(remove_device_instance(created_instance));
      } catch (...) {
      }
    }
    throw;
  }
  reboot_required = reboot_required || update_result.reboot_required;
  // If the selected driver changed, an already open interface could still
  // belong to the old UMDF host even when Windows did not request a reboot.
  // Force the bounded owned-node recovery sequence and version attestation
  // before claiming that the newly staged package is live.
  bool requires_live_reload = reboot_required || update_result.updated;
  bool ready = wait_for_source_device_interface();
  bool reactivation_attempted = false;
  bool reactivation_reboot_required = false;
  bool device_cycle_attempted = false;
  bool device_cycle_reboot_required = false;
  bool device_cycle_succeeded = false;
  bool selected_driver_verified = false;
  bool owned_reload_verified = false;
  if (!ready || requires_live_reload) {
    try {
      const auto reactivation = reactivate_owned_root_devices();
      reactivation_attempted = reactivation.reenumerated != 0 || reactivation.restarted != 0;
      reactivation_reboot_required = reactivation.reboot_required;
      reboot_required = reboot_required || reactivation.reboot_required;
      requires_live_reload = requires_live_reload || reactivation.reboot_required;
      ready = wait_for_source_device_interface();
    } catch (const std::exception &error) {
      // This is a recovery attempt, not a replacement for normal PnP install.
      // Preserve the original reboot fallback while recording why live
      // activation was not available in the installer log.
      std::cerr << "Vibeshine VHF gamepad source-device activation retry failed: " << error.what() << std::endl;
    }
  }
  if (!ready || requires_live_reload) {
    try {
      const auto cycle = cycle_owned_root_devices();
      device_cycle_attempted = cycle.disabled != 0 || cycle.enabled != 0;
      device_cycle_reboot_required = cycle.reboot_required;
      device_cycle_succeeded = device_cycle_attempted && !cycle.recovery_failed;
      reboot_required = reboot_required || cycle.reboot_required;
      requires_live_reload = requires_live_reload || cycle.reboot_required;
      if (cycle.recovery_failed) {
        requires_live_reload = true;
        owned_reload_verified = false;
        std::cerr << "Vibeshine VHF gamepad source-device disable/enable retry failed: "
                  << cycle.failure_detail << std::endl;
      }
      ready = wait_for_source_device_interface();
    } catch (const std::exception &error) {
      // This is intentionally a final, exact-node recovery tier. A failure
      // must not broaden recovery to shared UMDF hosts or unrelated devices.
      std::cerr << "Vibeshine VHF gamepad source-device disable/enable retry failed: " << error.what() << std::endl;
    }
  }
  // A queried interface after a successful disable/enable proves that this
  // exact source node reloaded in the current session. When Windows requested
  // a reboot, also attest the driver version selected for every owned node so
  // an old compatible DLL cannot masquerade as the newly staged package.
  // Also attest when Windows kept an already-selected package (update_driver()
  // returned updated=false): that is an idempotent re-run of this package only
  // if every owned node runs this INF's DriverVer. A same-day package from
  // another branch (lower rev-list count) or an equal-DriverVer rebuild was
  // otherwise reported as installed with the old DLL still live.
  lvg::device_setup::install_outcome outcome {
    .ready = ready,
    .updated = update_result.updated,
    .reboot_required = reboot_required,
    .device_cycle_succeeded = device_cycle_succeeded,
    .device_cycle_reboot_required = device_cycle_reboot_required,
    .version_matches = false,
  };
  if (lvg::device_setup::attestation_due(outcome)) {
    selected_driver_verified = owned_root_devices_match_driver_version(expected_driver_version);
    outcome.version_matches = selected_driver_verified;
    if (!selected_driver_verified) {
      std::cerr << "Vibeshine VHF gamepad: the owned root device does not run the staged package's DriverVer"
                << (update_result.updated ? "" : " (Windows kept an already-selected package)") << std::endl;
    }
  }
  if (lvg::device_setup::reload_verified(outcome)) {
    owned_reload_verified = true;
    reboot_required = false;
  }
  if (!ready) {
    print_device_diagnostics(L"source_interface_not_ready");
  }
  bool rolled_back = false;
  bool rollback_reboot_required = false;
  if (!ready && !reboot_required && created) {
    const auto rollback = remove_device_instance(created_instance);
    if (!rollback.removed) {
      throw std::runtime_error("could not roll back the newly created root device after the source interface failed to start");
    }
    rolled_back = true;
    rollback_reboot_required = rollback.reboot_required;
    reboot_required = reboot_required || rollback.reboot_required;
  }
  print_status();
  std::wcout << L"{\"created_root_device\":" << (created ? L"true" : L"false")
             << L",\"driver_updated\":" << (update_result.updated ? L"true" : L"false")
             << L",\"reboot_required\":" << (reboot_required ? L"true" : L"false")
              << L",\"reactivation_attempted\":" << (reactivation_attempted ? L"true" : L"false")
              << L",\"reactivation_reboot_required\":" << (reactivation_reboot_required ? L"true" : L"false")
              << L",\"device_cycle_attempted\":" << (device_cycle_attempted ? L"true" : L"false")
              << L",\"device_cycle_reboot_required\":" << (device_cycle_reboot_required ? L"true" : L"false")
              << L",\"device_cycle_succeeded\":" << (device_cycle_succeeded ? L"true" : L"false")
              << L",\"selected_driver_verified\":" << (selected_driver_verified ? L"true" : L"false")
              << L",\"owned_reload_verified\":" << (owned_reload_verified ? L"true" : L"false")
              << L",\"rolled_back\":" << (rolled_back ? L"true" : L"false")
             << L",\"rollback_reboot_required\":" << (rollback_reboot_required ? L"true" : L"false")
             << L",\"ready\":" << (ready ? L"true" : L"false") << L"}" << std::endl;
  static_assert(lvg::device_setup::k_exit_reboot == k_exit_reboot_required);
  outcome.reboot_required = reboot_required;  // a rollback above may have added its own request
  return lvg::device_setup::install_exit_code(outcome);
}

[[nodiscard]] int run_remove() {
  require_elevation();
  const auto devices = enumerate_root_devices();
  DWORD removed = 0;
  bool reboot_required = false;
  for (const auto &device : devices) {
    const auto result = remove_device_instance(device.instance_id);
    if (result.removed) {
      ++removed;
    }
    reboot_required = reboot_required || result.reboot_required;
  }
  std::wcout << L"{\"removed_root_devices\":" << removed
             << L",\"reboot_required\":" << (reboot_required ? L"true" : L"false") << L"}" << std::endl;
  return reboot_required ? k_exit_reboot_required : 0;
}

void print_usage() {
  std::wcerr << L"Usage:" << std::endl;
  std::wcerr << L"  VibeshineVhfGamepadDeviceSetup.exe install --inf <path>" << std::endl;
  std::wcerr << L"  VibeshineVhfGamepadDeviceSetup.exe status" << std::endl;
  std::wcerr << L"  VibeshineVhfGamepadDeviceSetup.exe remove" << std::endl;
}

}  // namespace

int wmain(int argc, wchar_t *argv[]) {
  try {
    if (argc == 2 && std::wstring_view(argv[1]) == L"status") {
      print_status();
      const auto devices = enumerate_root_devices();
      if (source_device_interface_ready()) {
        return 0;
      }
      return devices.empty() ? 2 : 3;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"remove") {
      return run_remove();
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"install" && std::wstring_view(argv[2]) == L"--inf") {
      return run_install(argv[3]);
    }

    print_usage();
    return 64;
  } catch (const std::exception &error) {
    std::cerr << "Vibeshine VHF gamepad device setup failed: " << error.what() << std::endl;
    return 1;
  }
}
