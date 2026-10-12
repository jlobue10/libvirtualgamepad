// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

// This is a UMDF2 VHF source driver, not a HID minidriver. Its WDF device
// exposes a private control interface to Vibeshine; VHF creates the HID child
// for each active controller.

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <wdf.h>
#include <vhf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>

#include "libvirtualgamepad/protocol.h"
#include "dualsense.h"
#include "dualshock4.h"
#include "pid_ff.h"
#include "profile.h"
#include "report_pump.h"
#include "switch_pro.h"
#include "xbox_one.h"
#include "xbox_series.h"

namespace {

using lvg::driver::encode_generic_feedback;
using lvg::driver::encode_generic_input;
using lvg::driver::find_profile;
using lvg::driver::generic_input_report;
using lvg::driver::generic_output_report;
using lvg::driver::k_generic_input_report_id;
using lvg::driver::k_generic_output_report_id;
using lvg::driver::pid_engine;
using lvg::driver::pid_rumble_t;
using lvg::driver::profile_definition;

// How often playing force-feedback effects are advanced. Envelopes and finite
// durations only need to look continuous to a human hand, and the timer stops
// itself as soon as nothing is playing.
constexpr LONG k_pid_tick_ms = 10;

enum class slot_state : std::uint8_t {
  empty,
  starting,
  active,
  stopping,
};

struct device_context;

struct controller_slot {
  device_context *parent;
  VHFHANDLE vhf;
  WDFFILEOBJECT owner;
  lvg::profile selected_profile;
  std::uint32_t controller_id;
  slot_state state;
  bool feedback_pending;
  lvg::feedback_event feedback;
  // Output reports update independently enabled fields. Keep the full state
  // across polls so coalescing never drops rumble or the other trigger.
  lvg::playstation_output_feedback playstation_feedback;
  // Set from the profile, so an output report is only interpreted as PID when
  // the descriptor actually declared the PID collection.
  bool force_feedback;
  bool pid_rumble_valid;
  pid_rumble_t pid_rumble;
  // A PlayStation report carries buttons, motion, touch, and battery together,
  // so each arrives on its own IOCTL and is folded into one accumulated state.
  // The last input state is kept so a touch or motion update can rebuild and
  // resend the whole report, the way real hardware streams it.
  bool have_last_input;
  lvg::input_state_request last_input;
  lvg::driver::ds4_state ds4;
  lvg::driver::ds5_state ds5;
  lvg::driver::switch_state switch_pro;
  // Paces input reports so reads do not always complete instantly, which would
  // leave a polling application spinning.
  lvg::driver::report_pump pump;
  // Both the packet and its pump-owned payload survive submission until VHF
  // grants the next read. They must also survive until VhfDelete completes.
  HID_XFER_PACKET read_transfer;
  // VHF does not copy these, so they live for as long as the child does.
  GUID container_id;
  wchar_t instance_id[32];
  // Zeroed by reset_slot and initialized by create_controller: WDF allocates
  // the device context as raw memory, so no constructor runs for this member.
  pid_engine pid;
};

struct device_context {
  WDFWAITLOCK lifetime_gate;
  WDFWAITLOCK state_lock;
  WDFIOTARGET local_vhf_target;
  HANDLE vhf_file_handle;
  bool vhf_target_open;
  bool stopping;
  WDFTIMER pid_timer;
  controller_slot controllers[lvg::k_max_controllers];
};

struct target_context {
  device_context *device;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(device_context, get_device_context);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(target_context, get_target_context);

EVT_WDF_DRIVER_DEVICE_ADD evt_device_add;
EVT_WDF_DEVICE_PREPARE_HARDWARE evt_prepare_hardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE evt_release_hardware;
EVT_WDF_OBJECT_CONTEXT_CLEANUP evt_vhf_target_cleanup;
EVT_WDF_DEVICE_FILE_CREATE evt_file_create;
EVT_WDF_FILE_CLOSE evt_file_close;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL evt_io_device_control;
EVT_VHF_ASYNC_OPERATION evt_vhf_write_report;
EVT_VHF_ASYNC_OPERATION evt_vhf_get_feature;
EVT_VHF_ASYNC_OPERATION evt_vhf_get_input_report;
EVT_VHF_READY_FOR_NEXT_READ_REPORT evt_vhf_ready_for_next_report;
EVT_VHF_ASYNC_OPERATION evt_vhf_set_feature;
EVT_VHF_CLEANUP evt_vhf_cleanup;
EVT_WDF_TIMER evt_pid_tick;

// Defined below with the PlayStation submit helpers; create_controller needs it
// to decide whether to register the feature-report callbacks.
[[nodiscard]] bool is_playstation(lvg::profile profile) noexcept;
[[nodiscard]] bool is_xbox(lvg::profile profile) noexcept;

// Base for each controller's container identity. The low byte is replaced with
// the controller index so every slot is its own physical device to Windows.
constexpr GUID k_controller_container_base {
  0x9a2f1c74,
  0x6b83,
  0x4d51,
  {0xa7, 0x0e, 0x35, 0x1d, 0xc6, 0x84, 0x00, 0x00}
};

// Writes "VibeshineGamepad<n>" without pulling in a formatting library.
void format_instance_id(wchar_t *const out, const std::size_t capacity, const std::uint32_t index) noexcept {
  constexpr wchar_t prefix[] = L"VibeshineGamepad";
  const std::size_t prefix_length = RTL_NUMBER_OF(prefix) - 1;
  if (capacity < prefix_length + 4) {
    if (capacity > 0) {
      out[0] = L'\0';
    }
    return;
  }

  std::size_t position = 0;
  for (; position < prefix_length; ++position) {
    out[position] = prefix[position];
  }
  if (index >= 10) {
    out[position++] = static_cast<wchar_t>(L'0' + (index / 10) % 10);
  }
  out[position++] = static_cast<wchar_t>(L'0' + index % 10);
  out[position] = L'\0';
}

void lock_context(device_context *const context) noexcept {
  WdfWaitLockAcquire(context->state_lock, nullptr);
}

void unlock_context(device_context *const context) noexcept {
  WdfWaitLockRelease(context->state_lock);
}

void lock_lifetime(device_context *const context) noexcept {
  WdfWaitLockAcquire(context->lifetime_gate, nullptr);
}

void unlock_lifetime(device_context *const context) noexcept {
  WdfWaitLockRelease(context->lifetime_gate);
}

// Opening the local I/O target by file is a create against this device's own
// stack, so it only succeeds once PnP has started the device. Callers own the
// lifetime gate; the open is idempotent so both the start path and the first
// controller creation can drive it.
[[nodiscard]] NTSTATUS ensure_vhf_target_open(device_context *const context) noexcept {
  lock_context(context);
  const WDFIOTARGET target = context->local_vhf_target;
  const bool opened = context->vhf_file_handle != nullptr;
  const bool stopping = context->stopping;
  unlock_context(context);

  if (opened) {
    return STATUS_SUCCESS;
  }
  if (stopping || target == nullptr) {
    return STATUS_DEVICE_NOT_READY;
  }

  WDF_IO_TARGET_OPEN_PARAMS open_params;
  WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(&open_params, nullptr);
  const NTSTATUS status = WdfIoTargetOpen(target, &open_params);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  const HANDLE file_handle = WdfIoTargetWdmGetTargetFileHandle(target);
  if (file_handle == nullptr) {
    WdfIoTargetClose(target);
    return STATUS_DEVICE_NOT_READY;
  }

  lock_context(context);
  context->vhf_file_handle = file_handle;
  context->vhf_target_open = true;
  unlock_context(context);
  return STATUS_SUCCESS;
}

[[nodiscard]] bool is_owned_by(
  const controller_slot &slot,
  const WDFFILEOBJECT owner) noexcept {
  return slot.owner == owner;
}

void reset_slot(controller_slot *slot) noexcept {
  auto *const parent = slot->parent;
  const auto controller_id = slot->controller_id;
  std::memset(slot, 0, sizeof(*slot));
  slot->parent = parent;
  slot->controller_id = controller_id;
  slot->state = slot_state::empty;
}

void release_starting_slot(
  device_context *const context,
  controller_slot *const slot,
  const WDFFILEOBJECT owner) noexcept {
  lock_context(context);
  if (slot->owner == owner &&
      (slot->state == slot_state::starting || slot->state == slot_state::stopping)) {
    reset_slot(slot);
  }
  unlock_context(context);
}

// Detaches the VHF handle before calling VhfDelete. A VHF output callback can
// still arrive while VhfDelete waits, but it will observe stopping state and
// complete without publishing a new feedback event.
void destroy_owned_controller(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const std::uint32_t controller_id) noexcept {
  if (controller_id >= lvg::k_max_controllers) {
    return;
  }

  auto &slot = context->controllers[controller_id];
  VHFHANDLE vhf = nullptr;

  lock_lifetime(context);
  lock_context(context);
  if (!is_owned_by(slot, owner) || slot.state == slot_state::empty) {
    unlock_context(context);
    unlock_lifetime(context);
    return;
  }

  if (slot.state == slot_state::starting) {
    // VhfStart may invoke callbacks before it returns. The creator owns the
    // uncommitted handle and will delete it when it observes this state.
    slot.state = slot_state::stopping;
    unlock_context(context);
    unlock_lifetime(context);
    return;
  }

  slot.state = slot_state::stopping;
  slot.feedback_pending = false;
  vhf = slot.vhf;
  slot.vhf = nullptr;
  unlock_context(context);

  if (vhf != nullptr) {
    VhfDelete(vhf, TRUE);
  }

  lock_context(context);
  if (is_owned_by(slot, owner) && slot.state == slot_state::stopping) {
    reset_slot(&slot);
  }
  unlock_context(context);
  unlock_lifetime(context);
}

[[nodiscard]] NTSTATUS create_controller(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::create_controller_request &request) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }
  if (request.reserved != 0) {
    return STATUS_INVALID_PARAMETER;
  }

  const profile_definition *const definition = find_profile(request.requested_profile);
  if (definition == nullptr) {
    return STATUS_NOT_SUPPORTED;
  }

  auto &slot = context->controllers[request.controller_id];

  // The lifetime gate is distinct from state_lock. VhfStart can invoke the
  // output callback before returning, and that callback needs state_lock.
  // Holding only the outer gate across VHF calls prevents target cleanup from
  // invalidating the FileHandle without deadlocking the callback.
  lock_lifetime(context);
  lock_context(context);
  if (context->stopping) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }
  if (slot.state != slot_state::empty) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_DEVICE_BUSY;
  }

  slot.owner = owner;
  slot.selected_profile = request.requested_profile;
  slot.state = slot_state::starting;
  slot.feedback_pending = false;
  slot.playstation_feedback = {};
  slot.force_feedback = definition->force_feedback;
  slot.pid_rumble_valid = false;
  slot.pid_rumble = {};
  // reset_slot zeroed this storage, so the engine has to be put into its
  // documented initial state explicitly before any report can reach it.
  slot.pid.reset();
  slot.have_last_input = false;
  slot.last_input = {};
  slot.ds4.reset();
  slot.ds4.features.address[0] = static_cast<std::uint8_t>(request.controller_id);
  slot.ds5.reset();
  slot.ds5.features.address[0] = static_cast<std::uint8_t>(request.controller_id);
  slot.switch_pro.reset();
  slot.pump.reset();
  unlock_context(context);

  // The start path already opens this target. Retrying here keeps a source
  // device that started before its own stack could answer a create usable.
  const NTSTATUS open_status = ensure_vhf_target_open(context);
  if (!NT_SUCCESS(open_status)) {
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return open_status;
  }

  lock_context(context);
  const HANDLE file_handle = context->vhf_file_handle;
  unlock_context(context);
  if (file_handle == nullptr) {
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }

  // State is completely initialized before VhfStart: the framework is allowed
  // to enter an output callback before VhfStart returns.
  VHF_CONFIG config;
  VHF_CONFIG_INIT(
    &config,
    file_handle,
    static_cast<USHORT>(definition->report_descriptor_size),
    const_cast<PUCHAR>(definition->report_descriptor));
  config.VhfClientContext = &slot;
  config.EvtVhfAsyncOperationWriteReport = evt_vhf_write_report;
  config.EvtVhfCleanup = evt_vhf_cleanup;
  // Without this the driver never learns when VHF will take another report and
  // has to guess, which is what makes a device readable without pause.
  config.EvtVhfReadyForNextReadReport = evt_vhf_ready_for_next_report;
  // Answers HidD_GetInputReport, which some applications use to read an initial
  // state instead of waiting for the first change.
  config.EvtVhfAsyncOperationGetInputReport = evt_vhf_get_input_report;

  // Give each controller its own identity. Sharing the parent's container makes
  // several controllers look like one device to anything that groups by
  // physical device, and without a distinct instance two controllers of the
  // same model can collide on their device path.
  slot.container_id = k_controller_container_base;
  slot.container_id.Data4[7] = static_cast<UCHAR>(request.controller_id);
  config.ContainerID = slot.container_id;
  format_instance_id(slot.instance_id, RTL_NUMBER_OF(slot.instance_id), request.controller_id);
  config.InstanceID = slot.instance_id;
  config.InstanceIDLength =
    static_cast<USHORT>((wcslen(slot.instance_id) + 1) * sizeof(wchar_t));
  // Without an identity the HID child enumerates as VID/PID 0000:0000, which
  // leaves Windows and applications nothing to match on.
  config.VendorID = definition->vendor_id;
  config.ProductID = definition->product_id;
  config.VersionNumber = definition->version_number;
  if (definition->hardware_ids != nullptr && definition->hardware_ids_bytes != 0) {
    // Keep GameInput's PnP identity consistent with HID attributes. Xbox IDs
    // additionally carry IG_00 to attach xinputhid.sys. These runtime IDs never
    // appear in the signed INF.
    config.HardwareIDs = const_cast<PWSTR>(definition->hardware_ids);
    config.HardwareIDsLength = static_cast<USHORT>(definition->hardware_ids_bytes);
  }
  if (definition->force_feedback || is_playstation(definition->id)) {
    // DirectInput discovers effect capacity through feature reports, and a
    // PlayStation controller is only recognized as one by hosts that can read
    // its calibration, pairing, and firmware features.
    config.EvtVhfAsyncOperationGetFeature = evt_vhf_get_feature;
    config.EvtVhfAsyncOperationSetFeature = evt_vhf_set_feature;
  }

  VHFHANDLE vhf = nullptr;
  NTSTATUS status = VhfCreate(&config, &vhf);
  if (!NT_SUCCESS(status)) {
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return status;
  }

  status = VhfStart(vhf);
  if (!NT_SUCCESS(status)) {
    release_starting_slot(context, &slot, owner);
    VhfDelete(vhf, TRUE);
    unlock_lifetime(context);
    return status;
  }

  bool adopted = false;
  lock_context(context);
  if (!context->stopping && slot.owner == owner && slot.state == slot_state::starting) {
    slot.vhf = vhf;
    slot.state = slot_state::active;
    adopted = true;
  }
  unlock_context(context);

  if (!adopted) {
    VhfDelete(vhf, TRUE);
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return STATUS_CANCELLED;
  }

  unlock_lifetime(context);
  return STATUS_SUCCESS;
}

[[nodiscard]] NTSTATUS destroy_controller(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::controller_id_request &request) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }

  auto &slot = context->controllers[request.controller_id];
  lock_context(context);
  const bool owned = is_owned_by(slot, owner);
  const bool exists = slot.state != slot_state::empty;
  unlock_context(context);

  if (!owned) {
    return STATUS_ACCESS_DENIED;
  }
  if (!exists) {
    return STATUS_NOT_FOUND;
  }

  destroy_owned_controller(context, owner, request.controller_id);
  return STATUS_SUCCESS;
}

// Hands one report to VHF if it can take it now, otherwise leaves it queued for
// the readiness callback. Takes state_lock itself and must be called without
// it. The lifetime gate is deliberately not taken: this also runs from inside
// VHF callbacks, where the handle is already guaranteed alive because VhfDelete
// waits for the callback, and taking the gate there would deadlock that wait.
NTSTATUS pump_report(
  device_context *const context,
  controller_slot &slot,
  const void *const data,
  const ULONG length,
  const UCHAR report_id,
  const lvg::driver::report_kind kind) noexcept {
  lvg::driver::report_buffer *next = nullptr;
  VHFHANDLE vhf = nullptr;

  lock_context(context);
  if (context->stopping || slot.state != slot_state::active || slot.vhf == nullptr) {
    unlock_context(context);
    return STATUS_DEVICE_NOT_READY;
  }
  if (data != nullptr) {
    std::ignore = slot.pump.enqueue(data, length, report_id, kind);
  }
  next = slot.pump.take();
  if (next != nullptr) {
    slot.read_transfer = {next->data, next->length, next->report_id};
  }
  vhf = slot.vhf;
  unlock_context(context);

  if (next == nullptr) {
    return STATUS_SUCCESS;
  }

  // Only the VHF callback may grant another submission. A failed call must
  // not manufacture readiness, potentially overwriting an outstanding read.
  return VhfReadReportSubmit(vhf, &slot.read_transfer);
}

// VHF can accept another report. Drain one, preferring initialization replies,
// then ordered transitions, then the newest continuous state.
void evt_vhf_ready_for_next_report(PVOID vhf_client_context) {
  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  if (slot == nullptr || slot->parent == nullptr) {
    return;
  }

  lvg::driver::report_buffer *next = nullptr;
  VHFHANDLE vhf = nullptr;

  auto *const context = slot->parent;
  lock_context(context);
  slot->pump.set_ready();
  if (!context->stopping && slot->state == slot_state::active) {
    next = slot->pump.take();
    if (next != nullptr) {
      slot->read_transfer = {next->data, next->length, next->report_id};
    }
    vhf = slot->vhf;
  }
  unlock_context(context);

  if (next != nullptr && vhf != nullptr) {
    std::ignore = VhfReadReportSubmit(vhf, &slot->read_transfer);
  }
}

// True when the profile folds touch, motion, and battery into its input report.
[[nodiscard]] bool is_playstation(const lvg::profile profile) noexcept {
  return profile == lvg::profile::dualshock_4 || profile == lvg::profile::dualsense;
}

// Both Xbox profiles take the same rumble report, so the output path treats
// them alike. Only the input report differs, by the Share button.
[[nodiscard]] bool is_xbox(const lvg::profile profile) noexcept {
  return profile == lvg::profile::xbox_series || profile == lvg::profile::xbox_one;
}

// Rebuilds and submits a PlayStation input report from the accumulated state.
// The caller owns the lifetime gate and must not hold state_lock.
[[nodiscard]] NTSTATUS submit_playstation_report(
  device_context *const context,
  controller_slot &slot,
  const lvg::driver::report_kind kind) noexcept {
  using namespace lvg::driver;

  lock_context(context);
  if (context->stopping || slot.state != slot_state::active || slot.vhf == nullptr) {
    unlock_context(context);
    return STATUS_DEVICE_NOT_READY;
  }

  // A report is only meaningful once the client has sent at least one input
  // state; before that there are no stick or button values to carry.
  if (!slot.have_last_input) {
    unlock_context(context);
    return STATUS_SUCCESS;
  }

  ds4_input_report ds4_report {};
  ds5_input_report ds5_report {};
  const void *data = nullptr;
  ULONG length = 0;
  UCHAR report_id = 0;

  if (slot.selected_profile == lvg::profile::dualshock_4) {
    ds4_report = encode_ds4_input(slot.last_input, &slot.ds4);
    data = &ds4_report;
    length = sizeof(ds4_report);
    report_id = k_ds4_input_report_id;
  } else {
    ds5_report = encode_ds5_input(slot.last_input, &slot.ds5);
    data = &ds5_report;
    length = sizeof(ds5_report);
    report_id = k_ds5_input_report_id;
  }
  unlock_context(context);

  return pump_report(context, slot, data, length, report_id, kind);
}

// Resends the current state for whichever profile owns the slot. Touch, motion
// and battery all arrive as separate IOCTLs but travel inside the device's one
// input report, so each has to rebuild and resend it.
[[nodiscard]] NTSTATUS submit_profile_report(
  device_context *const context,
  controller_slot &slot,
  const lvg::driver::report_kind kind = lvg::driver::report_kind::continuous) noexcept {
  if (slot.selected_profile != lvg::profile::switch_pro) {
    return submit_playstation_report(context, slot, kind);
  }

  lock_context(context);
  if (context->stopping || slot.state != slot_state::active || slot.vhf == nullptr) {
    unlock_context(context);
    return STATUS_DEVICE_NOT_READY;
  }
  if (!slot.have_last_input) {
    unlock_context(context);
    return STATUS_SUCCESS;
  }

  const lvg::driver::switch_input_report report =
    lvg::driver::encode_switch_input(slot.last_input, &slot.switch_pro);
  unlock_context(context);

  return pump_report(context, slot, &report, sizeof(report),
                     lvg::driver::k_switch_input_report_id,
                     kind);
}

// Shared preamble for the touch, motion, and battery IOCTLs: validate the
// controller and ownership, then hand back the slot for the profile to fold the
// event into. Returns STATUS_SUCCESS when *slot_out is usable.
[[nodiscard]] NTSTATUS begin_state_update(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const std::uint32_t controller_id,
  controller_slot **const slot_out) noexcept {
  if (controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }

  auto &slot = context->controllers[controller_id];
  lock_context(context);
  const bool usable = !context->stopping && is_owned_by(slot, owner) &&
                      slot.state == slot_state::active && slot.vhf != nullptr;
  const lvg::profile profile = slot.selected_profile;
  unlock_context(context);

  if (!usable) {
    return STATUS_DEVICE_NOT_READY;
  }
  if (!is_playstation(profile) && profile != lvg::profile::switch_pro) {
    // Only the PlayStation and Switch Pro profiles have motion sensors and a
    // battery to report against.
    return STATUS_NOT_SUPPORTED;
  }

  *slot_out = &slot;
  return STATUS_SUCCESS;
}

[[nodiscard]] NTSTATUS submit_touch_state(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::touch_state_request &request) noexcept {
  controller_slot *slot = nullptr;
  lock_lifetime(context);
  NTSTATUS status = begin_state_update(context, owner, request.controller_id, &slot);
  if (!NT_SUCCESS(status)) {
    unlock_lifetime(context);
    return status;
  }

  lock_context(context);
  // Only the PlayStation pads have a touchpad.
  const bool applied = slot->selected_profile == lvg::profile::dualshock_4
                         ? lvg::driver::apply_ds4_touch(request, &slot->ds4)
                         : slot->selected_profile == lvg::profile::dualsense
                             ? lvg::driver::apply_ds5_touch(request, &slot->ds5)
                             : false;
  unlock_context(context);

  if (!applied) {
    unlock_lifetime(context);
    return STATUS_INVALID_PARAMETER;
  }

  const auto touch_event = static_cast<lvg::touch_event>(request.event_type);
  const auto kind = touch_event == lvg::touch_event::move || touch_event == lvg::touch_event::hover
                      ? lvg::driver::report_kind::continuous
                      : lvg::driver::report_kind::transition;
  status = submit_profile_report(context, *slot, kind);
  unlock_lifetime(context);
  return status;
}

[[nodiscard]] NTSTATUS submit_motion_state(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::motion_state_request &request) noexcept {
  controller_slot *slot = nullptr;
  lock_lifetime(context);
  NTSTATUS status = begin_state_update(context, owner, request.controller_id, &slot);
  if (!NT_SUCCESS(status)) {
    unlock_lifetime(context);
    return status;
  }

  lock_context(context);
  const bool applied =
    slot->selected_profile == lvg::profile::dualshock_4
      ? lvg::driver::apply_ds4_motion(request, &slot->ds4)
      : slot->selected_profile == lvg::profile::dualsense
          ? lvg::driver::apply_ds5_motion(request, &slot->ds5)
          : lvg::driver::apply_switch_motion(request, &slot->switch_pro);
  unlock_context(context);

  if (!applied) {
    unlock_lifetime(context);
    return STATUS_INVALID_PARAMETER;
  }

  status = submit_profile_report(context, *slot);
  unlock_lifetime(context);
  return status;
}

[[nodiscard]] NTSTATUS submit_battery_state(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::battery_state_request &request) noexcept {
  controller_slot *slot = nullptr;
  lock_lifetime(context);
  NTSTATUS status = begin_state_update(context, owner, request.controller_id, &slot);
  if (!NT_SUCCESS(status)) {
    unlock_lifetime(context);
    return status;
  }

  lock_context(context);
  const bool applied =
    slot->selected_profile == lvg::profile::dualshock_4
      ? lvg::driver::apply_ds4_battery(request, &slot->ds4)
      : slot->selected_profile == lvg::profile::dualsense
          ? lvg::driver::apply_ds5_battery(request, &slot->ds5)
          : lvg::driver::apply_switch_battery(request, &slot->switch_pro);
  unlock_context(context);

  if (!applied) {
    unlock_lifetime(context);
    return STATUS_INVALID_PARAMETER;
  }

  status = submit_profile_report(context, *slot);
  unlock_lifetime(context);
  return status;
}

[[nodiscard]] NTSTATUS submit_input_state(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::input_state_request &request) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }
  if (request.reserved != 0) {
    return STATUS_INVALID_PARAMETER;
  }

  auto &slot = context->controllers[request.controller_id];
  lock_lifetime(context);
  lock_context(context);
  if (context->stopping || !is_owned_by(slot, owner) ||
      slot.state != slot_state::active || slot.vhf == nullptr) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }
  if (is_playstation(slot.selected_profile)) {
    slot.last_input = request;
    slot.have_last_input = true;
    using namespace lvg::driver;

    const auto ps_kind =
      slot.pump.classify(request.buttons, request.left_trigger, request.right_trigger);
    ds4_input_report ds4_report {};
    ds5_input_report ds5_report {};
    const void *ps_data = nullptr;
    ULONG ps_length = 0;
    UCHAR ps_report_id = 0;
    if (slot.selected_profile == lvg::profile::dualshock_4) {
      ds4_report = encode_ds4_input(request, &slot.ds4);
      ps_data = &ds4_report;
      ps_length = sizeof(ds4_report);
      ps_report_id = k_ds4_input_report_id;
    } else {
      ds5_report = encode_ds5_input(request, &slot.ds5);
      ps_data = &ds5_report;
      ps_length = sizeof(ds5_report);
      ps_report_id = k_ds5_input_report_id;
    }
    unlock_context(context);
    const NTSTATUS ps_status =
      pump_report(context, slot, ps_data, ps_length, ps_report_id, ps_kind);
    unlock_lifetime(context);
    return ps_status;
  }

  if (slot.selected_profile == lvg::profile::switch_pro) {
    slot.last_input = request;
    slot.have_last_input = true;
    const lvg::driver::switch_input_report switch_report =
      lvg::driver::encode_switch_input(request, &slot.switch_pro);
    const auto switch_kind =
      slot.pump.classify(request.buttons, request.left_trigger, request.right_trigger);
    unlock_context(context);
    const NTSTATUS switch_status =
      pump_report(context, slot, &switch_report, sizeof(switch_report),
                  lvg::driver::k_switch_input_report_id, switch_kind);
    unlock_lifetime(context);
    return switch_status;
  }

  if (is_xbox(slot.selected_profile)) {
    // The two reports differ only in length, so the shorter one is built from
    // the same encoder and both travel the same path.
    lvg::driver::xbox_series_input_report xbox_report {};
    ULONG xbox_length = 0;
    if (slot.selected_profile == lvg::profile::xbox_one) {
      const lvg::driver::xbox_one_input_report one = lvg::driver::encode_xbox_one_input(request);
      std::memcpy(&xbox_report, &one, sizeof(one));
      xbox_length = sizeof(one);
    } else {
      xbox_report = lvg::driver::encode_xbox_series_input(request);
      xbox_length = sizeof(xbox_report);
    }
    const auto xbox_kind =
      slot.pump.classify(request.buttons, request.left_trigger, request.right_trigger);
    unlock_context(context);
    const NTSTATUS xbox_status =
      pump_report(context, slot, &xbox_report, xbox_length,
                  lvg::driver::k_xbox_series_input_report_id, xbox_kind);
    unlock_lifetime(context);
    return xbox_status;
  }

  if (slot.selected_profile != lvg::profile::generic_hid &&
      slot.selected_profile != lvg::profile::generic_pid) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_NOT_SUPPORTED;
  }

  const generic_input_report report = encode_generic_input(request);
  const auto kind =
    slot.pump.classify(request.buttons, request.left_trigger, request.right_trigger);
  unlock_context(context);

  const NTSTATUS status = pump_report(context, slot, &report, sizeof(report),
                                      k_generic_input_report_id, kind);
  unlock_lifetime(context);
  return status;
}

[[nodiscard]] NTSTATUS poll_feedback(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::controller_id_request &request,
  lvg::feedback_event *const output) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }

  auto &slot = context->controllers[request.controller_id];
  lock_context(context);
  NTSTATUS status = STATUS_SUCCESS;
  if (!is_owned_by(slot, owner)) {
    status = STATUS_ACCESS_DENIED;
  } else if (slot.state != slot_state::active) {
    status = STATUS_DEVICE_NOT_READY;
  } else if (!slot.feedback_pending) {
    status = STATUS_NO_MORE_ENTRIES;
  } else {
    *output = slot.feedback;
    slot.feedback_pending = false;
  }
  unlock_context(context);
  return status;
}

// Copies a PID output report out of the transfer packet. HID pads the buffer
// to the report's declared length, so a longer buffer is normal; a shorter one
// means the report is not the one the descriptor declared.
template<class report_t>
[[nodiscard]] bool read_pid_report(const PHID_XFER_PACKET transfer, report_t *const out) noexcept {
  if (transfer->reportBuffer == nullptr || transfer->reportBufferLen < sizeof(report_t)) {
    return false;
  }
  std::memcpy(out, transfer->reportBuffer, sizeof(report_t));
  return out->report_id == transfer->reportId;
}

// Applies one PID output report. The caller owns state_lock.
[[nodiscard]] NTSTATUS apply_pid_output(
  controller_slot *const slot,
  const PHID_XFER_PACKET transfer) noexcept {
  using namespace lvg::driver;

  bool handled = false;
  switch (transfer->reportId) {
    case k_pid_set_effect_report_id: {
      pid_set_effect_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_effect(report);
      break;
    }
    case k_pid_set_envelope_report_id: {
      pid_set_envelope_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_envelope(report);
      break;
    }
    case k_pid_set_condition_report_id: {
      pid_set_condition_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_condition(report);
      break;
    }
    case k_pid_set_periodic_report_id: {
      pid_set_periodic_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_periodic(report);
      break;
    }
    case k_pid_set_constant_force_report_id: {
      pid_set_constant_force_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_constant_force(report);
      break;
    }
    case k_pid_set_ramp_force_report_id: {
      pid_set_ramp_force_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_ramp_force(report);
      break;
    }
    case k_pid_effect_operation_report_id: {
      pid_effect_operation_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.effect_operation(report);
      break;
    }
    case k_pid_block_free_report_id: {
      pid_block_free_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.block_free(report);
      break;
    }
    case k_pid_device_control_report_id: {
      pid_device_control_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.device_control(report);
      break;
    }
    case k_pid_device_gain_report_id: {
      pid_device_gain_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.device_gain(report);
      break;
    }
    default:
      return STATUS_INVALID_DEVICE_REQUEST;
  }

  return handled ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

// Publishes the engine's current rumble as a feedback event when it changed.
// The caller owns state_lock.
void publish_pid_rumble(controller_slot *const slot) noexcept {
  const pid_rumble_t rumble = slot->pid.rumble();
  if (slot->pid_rumble_valid &&
      rumble.low_frequency == slot->pid_rumble.low_frequency &&
      rumble.high_frequency == slot->pid_rumble.high_frequency) {
    return;
  }

  slot->pid_rumble = rumble;
  slot->pid_rumble_valid = true;

  // Built here rather than through encode_generic_feedback so the event is
  // tagged rumble-only: a PID effect carries no colour, and forwarding a black
  // LED would switch off the light on the client's real controller.
  lvg::feedback_event event {};
  event.header.size = sizeof(event);
  event.header.version = lvg::k_protocol_version;
  event.controller_id = slot->controller_id;
  event.type = lvg::feedback_type::generic_rumble;
  const lvg::generic_rumble_rgb_feedback payload {
    rumble.low_frequency,
    rumble.high_frequency,
    0, 0, 0, 0};
  event.payload_size = sizeof(payload);
  std::memcpy(event.payload, &payload, sizeof(payload));

  slot->feedback = event;
  slot->feedback_pending = true;
}

// Applies an Xbox rumble write. The caller owns state_lock.
[[nodiscard]] NTSTATUS apply_xbox_output(
  controller_slot *const slot,
  const PHID_XFER_PACKET transfer) noexcept {
  using namespace lvg::driver;

  if (transfer->reportId != k_xbox_series_output_report_id ||
      transfer->reportBuffer == nullptr ||
      transfer->reportBufferLen < sizeof(xbox_series_output_report)) {
    return STATUS_INVALID_DEVICE_REQUEST;
  }

  xbox_series_output_report output {};
  std::memcpy(&output, transfer->reportBuffer, sizeof(output));

  lvg::xbox_rumble_feedback rumble {};
  if (!decode_xbox_series_output(output, &rumble)) {
    return STATUS_INVALID_PARAMETER;
  }

  slot->feedback = encode_xbox_series_feedback(slot->controller_id, rumble);
  slot->feedback_pending = true;  // Coalesce to the current actuator state.
  return STATUS_SUCCESS;
}

void evt_vhf_write_report(
  PVOID vhf_client_context,
  VHFOPERATIONHANDLE operation_handle,
  PVOID,
  PHID_XFER_PACKET transfer) {
  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  bool arm_tick = false;

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->selected_profile == lvg::profile::switch_pro) {
    using namespace lvg::driver;

    auto *const context = slot->parent;
    switch_usb_reply usb_reply {};
    bool queued_reply = false;

    lock_context(context);
    if (context->stopping || slot->state != slot_state::active) {
      status = STATUS_DEVICE_NOT_READY;
    } else {
      // Rumble rides along with the subcommand reports as well as arriving on
      // its own, so decode it before deciding what to answer.
      lvg::playstation_output_feedback rumble {};
      if (transfer->reportBuffer != nullptr &&
          decode_switch_rumble(transfer->reportBuffer, transfer->reportBufferLen, &rumble)) {
        slot->feedback = encode_playstation_feedback(slot->controller_id, rumble);
        slot->feedback_pending = true;
      }

      if (transfer->reportId == k_switch_usb_command_id) {
        if (handle_switch_usb_command(transfer->reportBuffer, transfer->reportBufferLen,
                                      &slot->switch_pro, &usb_reply) != 0) {
          queued_reply = slot->pump.enqueue(&usb_reply, sizeof(usb_reply),
                                            k_switch_usb_reply_id, report_kind::priority);
        }
      } else if (transfer->reportId == k_switch_rumble_subcommand_id) {
        // Queue both replies under the state lock, so a concurrent input
        // update cannot put stale state behind the mode acknowledgement.
        queued_reply = queue_switch_subcommand_reply(
          transfer->reportBuffer, transfer->reportBufferLen,
          slot->last_input, &slot->switch_pro, slot->pump);
      }
      status = STATUS_SUCCESS;
    }
    const VHFHANDLE vhf = slot->vhf;
    unlock_context(context);

    // This controller answers commands on the input pipe, so a reply is a read
    // report rather than an operation result. Submitting it from inside the
    // callback is safe without the lifetime gate: VhfDelete waits for this
    // callback to return, so the handle cannot go away underneath it, and
    // taking the gate here would deadlock against that wait.
    if (NT_SUCCESS(status) && queued_reply && vhf != nullptr) {
      // A host blocked on a handshake reply is not streaming yet, so this goes
      // ahead of any controller state already waiting.
      std::ignore = pump_report(context, *slot, nullptr, 0, 0,
                                lvg::driver::report_kind::priority);
    }

    if (operation_handle != nullptr) {
      VhfAsyncOperationComplete(operation_handle, status);
    }
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      is_playstation(slot->selected_profile)) {
    using namespace lvg::driver;

    auto *const context = slot->parent;
    lock_context(context);
    lvg::playstation_output_feedback feedback = slot->playstation_feedback;
    bool decoded = false;

    if (slot->selected_profile == lvg::profile::dualshock_4) {
      ds4_output_report output {};
      if (transfer->reportBuffer != nullptr &&
          transfer->reportBufferLen >= sizeof(output)) {
        std::memcpy(&output, transfer->reportBuffer, sizeof(output));
        decoded = apply_ds4_output(output, &feedback);
      }
    } else if (transfer->reportBuffer != nullptr && transfer->reportBufferLen > 0) {
      ds5_output_report output {};
      const auto *const data = transfer->reportBuffer;
      const auto length = transfer->reportBufferLen;
      if (data[0] == k_ds5_output_report_id && length >= sizeof(output)) {
        std::memcpy(&output, data, sizeof(output));
        decoded = apply_ds5_output(output, &feedback);
      } else if (data[0] == k_ds5_output_report_id_bt &&
                 length >= 3 + sizeof(output) - 1) {
        // libScePad writes report 0x31 when HID advertises a Bluetooth-sized
        // output. The common payload starts after id, seq, and tag 0x10.
        output.report_id = k_ds5_output_report_id;
        std::memcpy(reinterpret_cast<std::uint8_t *>(&output) + 1, data + 3,
                    sizeof(output) - 1);
        decoded = apply_ds5_output(output, &feedback);
      }
    }

    if (context->stopping || slot->state != slot_state::active) {
      status = STATUS_DEVICE_NOT_READY;
    } else if (!decoded) {
      status = STATUS_INVALID_PARAMETER;
    } else {
      slot->playstation_feedback = feedback;
      slot->feedback = encode_playstation_feedback(slot->controller_id, feedback);
      slot->feedback_pending = true;  // Coalesce to the current actuator state.
      status = STATUS_SUCCESS;
    }
    unlock_context(context);

    if (operation_handle != nullptr) {
      VhfAsyncOperationComplete(operation_handle, status);
    }
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      is_xbox(slot->selected_profile)) {
    auto *const context = slot->parent;
    lock_context(context);
    if (!context->stopping && slot->state == slot_state::active) {
      status = apply_xbox_output(slot, transfer);
    } else {
      status = STATUS_DEVICE_NOT_READY;
    }
    unlock_context(context);

    if (operation_handle != nullptr) {
      VhfAsyncOperationComplete(operation_handle, status);
    }
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->force_feedback && transfer->reportId != k_generic_output_report_id) {
    auto *const context = slot->parent;
    lock_context(context);
    if (!context->stopping && slot->state == slot_state::active) {
      status = apply_pid_output(slot, transfer);
      if (NT_SUCCESS(status)) {
        publish_pid_rumble(slot);
        arm_tick = slot->pid.needs_tick();
      }
    } else {
      status = STATUS_DEVICE_NOT_READY;
    }
    unlock_context(context);

    // Started outside the lock: the tick callback takes state_lock itself.
    if (arm_tick && context->pid_timer != nullptr) {
      WdfTimerStart(context->pid_timer, WDF_REL_TIMEOUT_IN_MS(k_pid_tick_ms));
    }

    if (operation_handle != nullptr) {
      VhfAsyncOperationComplete(operation_handle, status);
    }
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      transfer->reportId == k_generic_output_report_id &&
      transfer->reportBuffer != nullptr &&
      transfer->reportBufferLen >= sizeof(generic_output_report)) {
    generic_output_report output {};
    std::memcpy(&output, transfer->reportBuffer, sizeof(output));

    if (output.report_id != k_generic_output_report_id) {
      status = STATUS_INVALID_PARAMETER;
    } else {
      auto *const context = slot->parent;
      lock_context(context);
      if (!context->stopping && slot->state == slot_state::active &&
          (slot->selected_profile == lvg::profile::generic_hid ||
           slot->selected_profile == lvg::profile::generic_pid)) {
        slot->feedback = encode_generic_feedback(slot->controller_id, output);
        slot->feedback_pending = true;  // Coalesce to the current controller state.
        status = STATUS_SUCCESS;
      } else {
        status = STATUS_DEVICE_NOT_READY;
      }
      unlock_context(context);
    }
  }

  if (operation_handle != nullptr) {
    VhfAsyncOperationComplete(operation_handle, status);
  }
}

// DirectInput allocates an effect block by writing Create New Effect and then
// reading PID Block Load; it sizes its effect list from PID Pool.
void evt_vhf_get_feature(
  PVOID vhf_client_context,
  VHFOPERATIONHANDLE operation_handle,
  PVOID,
  PHID_XFER_PACKET transfer) {
  using namespace lvg::driver;

  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      is_playstation(slot->selected_profile) && transfer->reportBuffer != nullptr) {
    auto *const context = slot->parent;
    lock_context(context);
    if (context->stopping || slot->state != slot_state::active) {
      status = STATUS_DEVICE_NOT_READY;
    } else {
      const std::size_t written =
        slot->selected_profile == lvg::profile::dualshock_4
          ? fill_ds4_feature(transfer->reportId, transfer->reportBuffer, transfer->reportBufferLen, slot->ds4.features)
          : fill_ds5_feature(transfer->reportId, transfer->reportBuffer, transfer->reportBufferLen, slot->ds5.features);
      status = written != 0 ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
    }
    unlock_context(context);

    if (operation_handle != nullptr) {
      VhfAsyncOperationComplete(operation_handle, status);
    }
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->force_feedback && transfer->reportBuffer != nullptr) {
    auto *const context = slot->parent;
    lock_context(context);
    if (context->stopping || slot->state != slot_state::active) {
      status = STATUS_DEVICE_NOT_READY;
    } else {
      switch (transfer->reportId) {
        case k_pid_block_load_report_id: {
          const pid_block_load_report report = slot->pid.block_load();
          if (transfer->reportBufferLen >= sizeof(report)) {
            std::memcpy(transfer->reportBuffer, &report, sizeof(report));
            status = STATUS_SUCCESS;
          } else {
            status = STATUS_BUFFER_TOO_SMALL;
          }
          break;
        }
        case k_pid_pool_report_id: {
          const pid_pool_report report = slot->pid.pool();
          if (transfer->reportBufferLen >= sizeof(report)) {
            std::memcpy(transfer->reportBuffer, &report, sizeof(report));
            status = STATUS_SUCCESS;
          } else {
            status = STATUS_BUFFER_TOO_SMALL;
          }
          break;
        }
        case k_pid_state_report_id: {
          const pid_state_report report = slot->pid.state();
          if (transfer->reportBufferLen >= sizeof(report)) {
            std::memcpy(transfer->reportBuffer, &report, sizeof(report));
            status = STATUS_SUCCESS;
          } else {
            status = STATUS_BUFFER_TOO_SMALL;
          }
          break;
        }
        default:
          status = STATUS_INVALID_DEVICE_REQUEST;
          break;
      }
    }
    unlock_context(context);
  }

  if (operation_handle != nullptr) {
    VhfAsyncOperationComplete(operation_handle, status);
  }
}

void evt_vhf_set_feature(
  PVOID vhf_client_context,
  VHFOPERATIONHANDLE operation_handle,
  PVOID,
  PHID_XFER_PACKET transfer) {
  using namespace lvg::driver;

  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      is_playstation(slot->selected_profile)) {
    auto *const context = slot->parent;
    lock_context(context);
    if (context->stopping || slot->state != slot_state::active) {
      status = STATUS_DEVICE_NOT_READY;
    } else {
      const bool accepted = slot->selected_profile == lvg::profile::dualshock_4
        ? lvg::ds4_usb::set_feature(transfer->reportId, transfer->reportBuffer,
                                  transfer->reportBufferLen, slot->ds4.features)
        : lvg::ds5_usb::set_feature(transfer->reportId, transfer->reportBuffer,
                                  transfer->reportBufferLen, slot->ds5.features);
      status = accepted ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
    }
    unlock_context(context);
    if (operation_handle != nullptr) VhfAsyncOperationComplete(operation_handle, status);
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->force_feedback && transfer->reportId == k_pid_create_new_effect_report_id) {
    pid_create_new_effect_report report {};
    if (!read_pid_report(transfer, &report)) {
      status = STATUS_INVALID_PARAMETER;
    } else {
      auto *const context = slot->parent;
      lock_context(context);
      if (context->stopping || slot->state != slot_state::active) {
        status = STATUS_DEVICE_NOT_READY;
      } else {
        // A full pool is a normal answer, not a transport failure: the host
        // learns about it by reading Block Load next.
        static_cast<void>(slot->pid.create_new_effect(report));
        status = STATUS_SUCCESS;
      }
      unlock_context(context);
    }
  }

  if (operation_handle != nullptr) {
    VhfAsyncOperationComplete(operation_handle, status);
  }
}

// Advances every playing effect and stops itself once nothing is playing, so an
// idle device does not keep a 100 Hz timer alive.
void evt_pid_tick(WDFTIMER timer) {
  auto *const context = get_device_context(
    reinterpret_cast<WDFDEVICE>(WdfTimerGetParentObject(timer)));
  if (context == nullptr) {
    return;
  }

  bool still_playing = false;
  lock_context(context);
  for (auto &slot : context->controllers) {
    if (slot.state != slot_state::active || !slot.force_feedback) {
      continue;
    }
    slot.pid.advance(static_cast<std::uint32_t>(k_pid_tick_ms));
    publish_pid_rumble(&slot);
    still_playing = still_playing || slot.pid.needs_tick();
  }
  const bool stopping = context->stopping;
  unlock_context(context);

  if (!still_playing || stopping) {
    // A timer may stop itself; passing FALSE keeps this from waiting on the
    // callback it is already running inside.
    WdfTimerStop(timer, FALSE);
  }
}

// A host can ask for the current state instead of waiting for the next change.
// Answering from the last submitted state keeps that read consistent with what
// the stream has already reported.
void evt_vhf_get_input_report(
  PVOID vhf_client_context,
  VHFOPERATIONHANDLE operation_handle,
  PVOID,
  PHID_XFER_PACKET transfer) {
  using namespace lvg::driver;

  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      transfer->reportBuffer != nullptr) {
    auto *const context = slot->parent;
    lock_context(context);
    if (context->stopping || slot->state != slot_state::active) {
      status = STATUS_DEVICE_NOT_READY;
    } else {
      // Buffer big enough for the largest report any profile produces.
      std::uint8_t buffer[k_max_report_bytes] {};
      std::size_t length = 0;
      std::uint8_t report_id = 0;

      switch (slot->selected_profile) {
        case lvg::profile::dualshock_4: {
          const ds4_input_report report = encode_ds4_input(slot->last_input, &slot->ds4);
          std::memcpy(buffer, &report, sizeof(report));
          length = sizeof(report);
          report_id = k_ds4_input_report_id;
          break;
        }
        case lvg::profile::dualsense: {
          const ds5_input_report report = encode_ds5_input(slot->last_input, &slot->ds5);
          std::memcpy(buffer, &report, sizeof(report));
          length = sizeof(report);
          report_id = k_ds5_input_report_id;
          break;
        }
        case lvg::profile::switch_pro: {
          const switch_input_report report =
            encode_switch_input(slot->last_input, &slot->switch_pro);
          std::memcpy(buffer, &report, sizeof(report));
          length = sizeof(report);
          report_id = k_switch_input_report_id;
          break;
        }
        case lvg::profile::xbox_series: {
          const xbox_series_input_report report = encode_xbox_series_input(slot->last_input);
          std::memcpy(buffer, &report, sizeof(report));
          length = sizeof(report);
          report_id = k_xbox_series_input_report_id;
          break;
        }
        case lvg::profile::xbox_one: {
          const xbox_one_input_report report = encode_xbox_one_input(slot->last_input);
          std::memcpy(buffer, &report, sizeof(report));
          length = sizeof(report);
          report_id = k_xbox_one_input_report_id;
          break;
        }
        default: {
          const generic_input_report report = encode_generic_input(slot->last_input);
          std::memcpy(buffer, &report, sizeof(report));
          length = sizeof(report);
          report_id = k_generic_input_report_id;
          break;
        }
      }

      // A request naming a different report is not one this device can answer.
      if (transfer->reportId != 0 && transfer->reportId != report_id) {
        status = STATUS_INVALID_DEVICE_REQUEST;
      } else if (transfer->reportBufferLen < length) {
        status = STATUS_BUFFER_TOO_SMALL;
      } else {
        std::memcpy(transfer->reportBuffer, buffer, length);
        status = STATUS_SUCCESS;
      }
    }
    unlock_context(context);
  }

  if (operation_handle != nullptr) {
    VhfAsyncOperationComplete(operation_handle, status);
  }
}

void evt_vhf_cleanup(PVOID) {
  // VhfDelete(..., TRUE) waits until this callback runs and guarantees that no
  // asynchronous VHF operation remains. Slot storage is static in the WDF
  // device context, so no callback-owned memory needs freeing here.
}

template<class request_t>
[[nodiscard]] NTSTATUS retrieve_request(
  WDFREQUEST request,
  request_t **const output) noexcept {
  PVOID raw = nullptr;
  size_t bytes = 0;
  NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(lvg::request_header), &raw, &bytes);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  auto *const typed = static_cast<request_t *>(raw);
  if (!lvg::valid_request(typed, bytes)) {
    return STATUS_INVALID_BUFFER_SIZE;
  }
  *output = typed;
  return STATUS_SUCCESS;
}

void evt_io_device_control(
  WDFQUEUE queue,
  WDFREQUEST request,
  const size_t output_buffer_length,
  const size_t,
  const ULONG io_control_code) {
  auto *const context = get_device_context(WdfIoQueueGetDevice(queue));
  const WDFFILEOBJECT owner = WdfRequestGetFileObject(request);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  ULONG_PTR information = 0;

  if (owner == nullptr) {
    WdfRequestComplete(request, STATUS_INVALID_HANDLE);
    return;
  }

  switch (io_control_code) {
    case lvg::ioctl_query_info: {
      lvg::query_info_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (!NT_SUCCESS(status)) {
        break;
      }
      if (output_buffer_length < sizeof(lvg::query_info_response)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      lvg::query_info_response *output = nullptr;
      status = WdfRequestRetrieveOutputBuffer(
        request,
        sizeof(*output),
        reinterpret_cast<PVOID *>(&output),
        nullptr);
      if (NT_SUCCESS(status)) {
        *output = {};
        output->header.size = sizeof(*output);
        output->header.version = lvg::k_protocol_version;
        output->minimum_protocol_version = lvg::k_protocol_version;
        output->maximum_protocol_version = lvg::k_protocol_version;
        output->available_profiles = lvg::driver::available_profiles();
        // Advertised device-wide; a profile without a touchpad, motion
        // sensors, or a battery answers those IOCTLs with STATUS_NOT_SUPPORTED.
        output->available_features = lvg::feature_input_state | lvg::feature_feedback |
                                     lvg::feature_touch | lvg::feature_motion |
                                     lvg::feature_battery |
                                     lvg::feature_hid_feature_reports;
        output->maximum_controllers = lvg::k_max_controllers;
        information = sizeof(*output);
      }
      break;
    }
    case lvg::ioctl_create_controller: {
      lvg::create_controller_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        status = create_controller(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_submit_touch_state: {
      lvg::touch_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status) && input->reserved != 0) {
        status = STATUS_INVALID_PARAMETER;
      }
      if (NT_SUCCESS(status)) {
        status = submit_touch_state(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_submit_motion_state: {
      lvg::motion_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        const bool reserved_is_zero = input->reserved0[0] == 0 &&
                                      input->reserved0[1] == 0 &&
                                      input->reserved0[2] == 0;
        status = reserved_is_zero ? submit_motion_state(context, owner, *input)
                                  : STATUS_INVALID_PARAMETER;
      }
      break;
    }
    case lvg::ioctl_submit_battery_state: {
      lvg::battery_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status) && input->reserved != 0) {
        status = STATUS_INVALID_PARAMETER;
      }
      if (NT_SUCCESS(status)) {
        status = submit_battery_state(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_destroy_controller: {
      lvg::controller_id_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        status = destroy_controller(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_submit_input_state: {
      lvg::input_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        status = submit_input_state(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_poll_feedback: {
      lvg::controller_id_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (!NT_SUCCESS(status)) {
        break;
      }
      if (output_buffer_length < sizeof(lvg::feedback_event)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      lvg::feedback_event *output = nullptr;
      status = WdfRequestRetrieveOutputBuffer(
        request,
        sizeof(*output),
        reinterpret_cast<PVOID *>(&output),
        nullptr);
      if (NT_SUCCESS(status)) {
        status = poll_feedback(context, owner, *input, output);
        if (NT_SUCCESS(status)) {
          information = sizeof(*output);
        }
      }
      break;
    }
    default:
      break;
  }

  WdfRequestCompleteWithInformation(request, status, information);
}

void evt_file_create(WDFDEVICE, WDFREQUEST request, WDFFILEOBJECT) {
  WdfRequestComplete(request, STATUS_SUCCESS);
}

void evt_file_close(WDFFILEOBJECT file_object) {
  const WDFDEVICE device = WdfFileObjectGetDevice(file_object);
  auto *const context = get_device_context(device);
  for (std::uint32_t controller_id = 0; controller_id < lvg::k_max_controllers; ++controller_id) {
    destroy_owned_controller(context, file_object, controller_id);
  }
}

// Tears down every controller this device owns and drops the VHF file handle.
// Callers must hold neither lock. forget_target is set only when the target
// object itself is going away.
void stop_owned_controllers(device_context *const context, const bool forget_target) noexcept {
  VHFHANDLE handles[lvg::k_max_controllers] {};

  lock_lifetime(context);
  lock_context(context);
  const WDFTIMER timer = context->pid_timer;
  context->stopping = true;
  context->vhf_file_handle = nullptr;
  if (forget_target) {
    context->local_vhf_target = nullptr;
    context->vhf_target_open = false;
  }
  for (std::uint32_t index = 0; index < lvg::k_max_controllers; ++index) {
    auto &slot = context->controllers[index];
    slot.feedback_pending = false;
    if (slot.state == slot_state::active && slot.vhf != nullptr) {
      slot.state = slot_state::stopping;
      handles[index] = slot.vhf;
      slot.vhf = nullptr;
    } else if (slot.state == slot_state::starting) {
      slot.state = slot_state::stopping;
    }
  }
  unlock_context(context);

  for (const auto handle : handles) {
    if (handle != nullptr) {
      VhfDelete(handle, TRUE);
    }
  }

  // Stopped outside state_lock because the tick callback acquires it.
  if (timer != nullptr) {
    WdfTimerStop(timer, TRUE);
  }
  unlock_lifetime(context);
}

// WDF guarantees that an I/O target's FileHandle remains valid through this
// target cleanup callback when the target is deleted while still open. The
// target is parented below state_lock and lifetime_gate, so both locks and the
// device context remain valid for the complete callback.
//
// Do not close the target here. It is being deleted while still open, which
// keeps its UMDF file handle valid through this callback; WDF closes it after
// VhfDelete has synchronously removed every virtual HID child.
void evt_vhf_target_cleanup(WDFOBJECT object) {
  auto *const target = get_target_context(reinterpret_cast<WDFIOTARGET>(object));
  auto *const context = target->device;
  if (context == nullptr) {
    return;
  }
  stop_owned_controllers(context, true);
}

// A UMDF source driver cannot open its own local target before the device is
// started: EvtDeviceAdd runs ahead of PnP start, the create fails with
// STATUS_DEVICE_NOT_READY, and the whole device stops with CM_PROB_FAILED_ADD.
// The documented VHF flow opens the target from the start path instead.
NTSTATUS evt_prepare_hardware(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST) {
  auto *const context = get_device_context(device);

  lock_lifetime(context);
  lock_context(context);
  context->stopping = false;
  unlock_context(context);
  // A source device that cannot reach VHF yet still answers protocol queries
  // and retries the open when a controller is created, so a failure here must
  // not keep the device from starting.
  static_cast<void>(ensure_vhf_target_open(context));
  unlock_lifetime(context);
  return STATUS_SUCCESS;
}

NTSTATUS evt_release_hardware(WDFDEVICE device, WDFCMRESLIST) {
  auto *const context = get_device_context(device);
  stop_owned_controllers(context, false);

  lock_lifetime(context);
  lock_context(context);
  const WDFIOTARGET target = context->vhf_target_open ? context->local_vhf_target : nullptr;
  context->vhf_target_open = false;
  unlock_context(context);
  if (target != nullptr) {
    WdfIoTargetClose(target);
  }
  unlock_lifetime(context);
  return STATUS_SUCCESS;
}

NTSTATUS evt_device_add(WDFDRIVER, PWDFDEVICE_INIT device_init) {
  // The INF installs this UMDF component above the inbox VHF function driver.
  // Tell WDF that this is a filter so VHF remains the sole power-policy owner
  // for the device stack.
  WdfFdoInitSetFilter(device_init);

  WDF_FILEOBJECT_CONFIG file_config;
  WDF_FILEOBJECT_CONFIG_INIT(
    &file_config,
    evt_file_create,
    evt_file_close,
    WDF_NO_EVENT_CALLBACK);

  WDF_OBJECT_ATTRIBUTES file_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&file_attributes);
  WdfDeviceInitSetFileObjectConfig(device_init, &file_config, &file_attributes);

  WDF_PNPPOWER_EVENT_CALLBACKS pnp_callbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_callbacks);
  pnp_callbacks.EvtDevicePrepareHardware = evt_prepare_hardware;
  pnp_callbacks.EvtDeviceReleaseHardware = evt_release_hardware;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_callbacks);

  WDF_OBJECT_ATTRIBUTES device_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, device_context);

  WDFDEVICE device = nullptr;
  NTSTATUS status = WdfDeviceCreate(&device_init, &device_attributes, &device);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  auto *const context = get_device_context(device);
  std::memset(context, 0, sizeof(*context));
  for (std::uint32_t index = 0; index < lvg::k_max_controllers; ++index) {
    context->controllers[index].parent = context;
    context->controllers[index].controller_id = index;
    context->controllers[index].state = slot_state::empty;
  }

  // The effect clock is an enhancement, not a prerequisite. Failing the whole
  // EvtDeviceAdd when it cannot be created takes the entire device down with
  // STATUS_NOT_SUPPORTED and leaves the control interface registered but
  // disabled, which reads as "driver installed but unusable". Without the timer
  // force feedback still tracks every report the host sends; only unattended
  // duration expiry and envelope shaping are lost.
  WDF_TIMER_CONFIG timer_config;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&timer_config, evt_pid_tick, k_pid_tick_ms);
  timer_config.AutomaticSerialization = FALSE;
  WDF_OBJECT_ATTRIBUTES timer_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&timer_attributes);
  timer_attributes.ParentObject = device;
  if (!NT_SUCCESS(WdfTimerCreate(&timer_config, &timer_attributes, &context->pid_timer))) {
    context->pid_timer = nullptr;
  }

  WDF_OBJECT_ATTRIBUTES lifetime_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&lifetime_attributes);
  lifetime_attributes.ParentObject = device;
  status = WdfWaitLockCreate(&lifetime_attributes, &context->lifetime_gate);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_OBJECT_ATTRIBUTES state_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&state_attributes);
  state_attributes.ParentObject = context->lifetime_gate;
  status = WdfWaitLockCreate(&state_attributes, &context->state_lock);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_OBJECT_ATTRIBUTES target_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&target_attributes, target_context);
  target_attributes.ParentObject = context->state_lock;
  target_attributes.EvtCleanupCallback = evt_vhf_target_cleanup;
  status = WdfIoTargetCreate(device, &target_attributes, &context->local_vhf_target);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  get_target_context(context->local_vhf_target)->device = context;

  status = WdfDeviceCreateDeviceInterface(device, &lvg::k_device_interface_guid, nullptr);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_IO_QUEUE_CONFIG queue_config;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchSequential);
  // Filter queues default to non-power-managed. Keep control requests tied to
  // the device power state so stop and resume cannot race controller I/O.
  queue_config.PowerManaged = WdfTrue;
  queue_config.EvtIoDeviceControl = evt_io_device_control;
  return WdfIoQueueCreate(device, &queue_config, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
}

}  // namespace

extern "C" NTSTATUS DriverEntry(
  PDRIVER_OBJECT driver_object,
  PUNICODE_STRING registry_path) {
  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, evt_device_add);
  config.DriverPoolTag = 'gVLV';

  return WdfDriverCreate(
    driver_object,
    registry_path,
    WDF_NO_OBJECT_ATTRIBUTES,
    &config,
    WDF_NO_HANDLE);
}
