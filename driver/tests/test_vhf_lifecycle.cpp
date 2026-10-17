// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Execute the complete shipping driver body, with real profile/pump code and
// modeled OS boundaries. This does not install a driver or validate WDK/IRQL.
#include "../src/driver.cpp"

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace {
std::atomic<int> checks {}, failures {};
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (false)
NTSTATUS create_result = STATUS_SUCCESS, start_result = STATUS_SUCCESS, submit_result = STATUS_SUCCESS;
std::function<void(VHFHANDLE)> on_start, on_delete, on_submit;
std::function<void()> on_complete;
int creates {}, starts {}, deletes {}, submits {};
}

struct fake_vhf {
  VHF_CONFIG config;
  bool deleted {}, deleting {}, credit {};
  PHID_XFER_PACKET retained {};
  HID_XFER_PACKET packet {};
  std::vector<UCHAR> bytes;
};

namespace {
std::vector<std::unique_ptr<fake_vhf>> handles;
controller_slot &slot_of(VHFHANDLE h) { return *static_cast<controller_slot *>(h->config.VhfClientContext); }
void boundary(controller_slot &slot) {
  CHECK(!fake_holds(slot.parent->state_lock));
}
void retained_unchanged(VHFHANDLE h) {
  if (!h->retained) return;
  CHECK(h->retained->reportBuffer == h->packet.reportBuffer);
  CHECK(h->retained->reportBufferLen == h->packet.reportBufferLen);
  CHECK(h->retained->reportId == h->packet.reportId);
  CHECK(std::memcmp(h->packet.reportBuffer, h->bytes.data(), h->bytes.size()) == 0);
}
void ready(VHFHANDLE h) {
  retained_unchanged(h);
  h->retained = nullptr;
  h->credit = true;
  h->config.EvtVhfReadyForNextReadReport(h->config.VhfClientContext);
}
NTSTATUS call(EVT_VHF_ASYNC_OPERATION *callback, controller_slot &slot, PHID_XFER_PACKET packet) {
  fake_operation op;
  op.state_lock = slot.parent->state_lock;
  callback(&slot, &op, nullptr, packet);
  CHECK(op.completions == 1);
  return op.status;
}
NTSTATUS input(controller_slot &slot, UCHAR id = 0, ULONG length = 64) {
  std::array<UCHAR, 64> buffer {};
  HID_XFER_PACKET packet {buffer.data(), length, id};
  return call(evt_vhf_get_input_report, slot, &packet);
}
NTSTATUS output(controller_slot &slot) {
  using namespace lvg::driver;
  std::array<UCHAR, 80> buffer {};
  HID_XFER_PACKET packet {buffer.data(), 0, 0};
  switch (slot.selected_profile) {
    case lvg::profile::dualshock_4: {
      ds4_output_report report {}; report.report_id = k_ds4_output_report_id;
      report.flags = 1; report.left_rumble = 51;
      std::memcpy(buffer.data(), &report, sizeof(report));
      packet.reportBufferLen = sizeof(report); packet.reportId = report.report_id;
      break;
    }
    case lvg::profile::dualsense: {
      ds5_output_report report {}; report.report_id = k_ds5_output_report_id;
      report.valid_flag0 = 3; report.motor_left = 51;
      std::memcpy(buffer.data(), &report, sizeof(report));
      packet.reportBufferLen = sizeof(report); packet.reportId = report.report_id;
      break;
    }
    case lvg::profile::switch_pro:
      buffer[0] = k_switch_usb_command_id; buffer[1] = k_switch_usb_handshake;
      packet.reportId = buffer[0]; packet.reportBufferLen = 2;
      break;
    case lvg::profile::generic_hid:
    case lvg::profile::generic_pid: {
      generic_output_report report {}; report.report_id = k_generic_output_report_id;
      report.low_frequency_rumble = 51;
      std::memcpy(buffer.data(), &report, sizeof(report));
      packet.reportBufferLen = sizeof(report); packet.reportId = report.report_id;
      break;
    }
    default: {
      xbox_series_output_report report {};
      report.report_id = k_xbox_series_output_report_id;
      report.enable_mask = k_xbox_enable_left_motor; report.left_motor = 51;
      std::memcpy(buffer.data(), &report, sizeof(report));
      packet.reportBufferLen = sizeof(report); packet.reportId = report.report_id;
      break;
    }
  }
  return call(evt_vhf_write_report, slot, &packet);
}
NTSTATUS feature(controller_slot &slot, bool set = false) {
  std::array<UCHAR, 64> buffer {};
  const bool ds4 = slot.selected_profile == lvg::profile::dualshock_4;
  buffer[0] = set ? (ds4 ? 0x14 : 0x08) : (ds4 ? 0xa3 : 0x20);
  buffer[1] = 2;
  HID_XFER_PACKET packet {buffer.data(), set && ds4 ? 17u : 64u, buffer[0]};
  return call(set ? evt_vhf_set_feature : evt_vhf_get_feature, slot, &packet);
}
struct fixture {
  device_context context {};
  fake_wdf_object lifetime, state, target, device, timer, owner, stranger;
  fixture() {
    create_result = start_result = submit_result = fake_open_status = STATUS_SUCCESS;
    on_start = on_delete = on_submit = {}; on_complete = {}; fake_close_hook = {};
    fake_missing_file = false;
    creates = starts = deletes = submits = fake_opens = fake_closes = 0;
    handles.clear();
    device.context = &context; target.context = &target_data;
    target_data.device = &context;
    owner.parent = stranger.parent = timer.parent = &device;
    context.lifetime_gate = &lifetime; context.state_lock = &state;
    context.local_vhf_target = &target; context.pid_timer = &timer;
    for (std::uint32_t n = 0; n < lvg::k_max_controllers; ++n) {
      context.controllers[n].parent = &context;
      context.controllers[n].controller_id = n;
    }
  }
  ~fixture() {
    on_delete = {}; on_submit = {}; on_complete = {};
    stop_owned_controllers(&context, false);
    CHECK(fake_held_locks.empty());
    for (auto &h : handles) CHECK(h->deleted);
  }
  target_context target_data {};
  NTSTATUS create(lvg::profile profile, std::uint32_t id = 0) {
    lvg::create_controller_request request {};
    request.controller_id = id; request.requested_profile = profile;
    return create_controller(&context, &owner, request);
  }
};
constexpr std::array profiles {lvg::profile::dualshock_4, lvg::profile::dualsense,
  lvg::profile::switch_pro, lvg::profile::xbox_one, lvg::profile::xbox_series};

void check_initial_callbacks(VHFHANDLE h) {
  auto &slot = slot_of(h);
  CHECK(input(slot) == STATUS_SUCCESS);
  CHECK(input(slot, 0xff) == STATUS_INVALID_DEVICE_REQUEST);
  CHECK(input(slot, 0, 1) == STATUS_BUFFER_TOO_SMALL);
  CHECK(call(evt_vhf_get_input_report, slot, nullptr) == STATUS_INVALID_DEVICE_REQUEST);
  if (is_playstation(slot.selected_profile)) {
    CHECK(feature(slot) == STATUS_SUCCESS);
    CHECK(feature(slot, true) == STATUS_SUCCESS);
    std::array<UCHAR, 64> buffer {};
    HID_XFER_PACKET unknown {buffer.data(), 64, 0xff};
    CHECK(call(evt_vhf_get_feature, slot, &unknown) == STATUS_INVALID_DEVICE_REQUEST);
  }
  ready(h);
  CHECK(output(slot) == STATUS_SUCCESS);
  if (slot.selected_profile != lvg::profile::switch_pro) CHECK(slot.feedback_pending);
}

void test_profiles() {
  for (auto profile : profiles) {
    fixture f;
    on_start = [&](VHFHANDLE h) {
      auto &slot = slot_of(h);
      CHECK(slot.state == slot_state::starting);
      CHECK(slot.vhf == h);
      // Internal report service must not expose a Starting slot to its owner.
      lvg::controller_id_request request {}; lvg::feedback_event event {};
      CHECK(poll_feedback(&f.context, &f.owner, request, &event) == STATUS_DEVICE_NOT_READY);
      CHECK(poll_feedback(&f.context, &f.stranger, request, &event) == STATUS_ACCESS_DENIED);
      // Exercise a framework callback arriving on another thread during start.
      std::thread callback([&] { check_initial_callbacks(h); }); callback.join();
    };
    CHECK(f.create(profile) == STATUS_SUCCESS);
    auto &slot = f.context.controllers[0];
    CHECK(slot.state == slot_state::active);
    CHECK(input(slot) == STATUS_SUCCESS);
    CHECK(output(slot) == STATUS_SUCCESS);
    CHECK(f.create(profile) == STATUS_DEVICE_BUSY);
    const int before = deletes;
    destroy_owned_controller(&f.context, &f.stranger, 0);
    CHECK(deletes == before); CHECK(slot.state == slot_state::active);
    on_delete = [&](VHFHANDLE h) {
      auto &draining = slot_of(h);
      CHECK(draining.owner == &f.owner);
      CHECK(draining.selected_profile == profile);
      CHECK(draining.state == slot_state::stopping);
      CHECK(draining.vhf == nullptr);
      const bool was_ready = draining.pump.ready();
      std::thread callback([&] {
        CHECK(input(draining) == STATUS_DEVICE_NOT_READY);
        CHECK(output(draining) == STATUS_DEVICE_NOT_READY);
        if (is_playstation(profile)) {
          CHECK(feature(draining) == STATUS_DEVICE_NOT_READY);
          CHECK(feature(draining, true) == STATUS_DEVICE_NOT_READY);
        }
        ready(h);
      }); callback.join();
      CHECK(draining.pump.ready() == was_ready);
      CHECK(!draining.feedback_pending);
    };
    evt_file_close(&f.owner);
    CHECK(deletes == before + 1); CHECK(slot.state == slot_state::empty);
    CHECK(slot.owner == nullptr); CHECK(slot.parent == &f.context); CHECK(slot.controller_id == 0);
    evt_file_close(&f.owner); CHECK(deletes == before + 1);
    on_delete = {}; on_start = {};
    CHECK(f.create(profile) == STATUS_SUCCESS);
  }
}

void test_start_rollback() {
  for (auto profile : profiles) for (int mode = 0; mode < 3; ++mode) {
    fixture f;
    if (mode == 0) start_result = STATUS_NOT_SUPPORTED;
    on_start = [&](VHFHANDLE h) {
      check_initial_callbacks(h);
      if (mode != 0) {
        // Fault injection at the adoption boundary. Real close/release callers
        // serialize behind lifetime_gate; this exercises the defensive branch.
        lock_context(&f.context);
        if (mode == 1) f.context.stopping = true;
        else slot_of(h).state = slot_state::stopping;
        unlock_context(&f.context);
      }
    };
    on_delete = [&](VHFHANDLE h) {
      auto &slot = slot_of(h);
      CHECK(slot.state == slot_state::stopping); CHECK(slot.vhf == nullptr);
      CHECK(slot.owner == &f.owner); CHECK(slot.selected_profile == profile);
      CHECK(input(slot) == STATUS_DEVICE_NOT_READY);
      CHECK(!slot.feedback_pending);
      // Snapshot is checked before and after this callback by VhfDelete.
      CHECK(slot.parent == &f.context);
    };
    CHECK(f.create(profile) == (mode == 0 ? STATUS_NOT_SUPPORTED : STATUS_CANCELLED));
    CHECK(starts == 1); CHECK(deletes == 1);
    CHECK(f.context.controllers[0].state == slot_state::empty);
    CHECK(f.context.controllers[0].read_transfer.reportBuffer == nullptr);
    on_start = {}; on_delete = {}; start_result = STATUS_SUCCESS;
    f.context.stopping = false;
    CHECK(f.create(profile) == STATUS_SUCCESS);
  }
}

void test_prestart_failures() {
  for (int mode = 0; mode < 3; ++mode) {
    fixture f;
    if (mode == 0) fake_open_status = STATUS_ACCESS_DENIED;
    if (mode == 1) fake_missing_file = true;
    if (mode == 2) create_result = STATUS_NOT_SUPPORTED;
    const auto result = f.create(lvg::profile::dualsense);
    CHECK(result == (mode == 0 ? STATUS_ACCESS_DENIED : mode == 1 ? STATUS_DEVICE_NOT_READY : STATUS_NOT_SUPPORTED));
    CHECK(starts == 0); CHECK(deletes == 0);
    CHECK(f.context.controllers[0].state == slot_state::empty);
    CHECK(f.context.controllers[0].owner == nullptr);
    CHECK(fake_closes == (mode == 1 ? 1 : 0));
    fake_missing_file = false; fake_open_status = create_result = STATUS_SUCCESS;
    CHECK(f.create(lvg::profile::dualsense) == STATUS_SUCCESS);
  }
  fixture f;
  CHECK(f.create(lvg::profile::generic_pid) == STATUS_NOT_SUPPORTED);
  CHECK(f.create(lvg::profile::generic_hid) == STATUS_NOT_SUPPORTED);
  CHECK(f.create(lvg::profile::xbox_360) == STATUS_NOT_SUPPORTED);
  CHECK(f.create(lvg::profile::dualsense, lvg::k_max_controllers) == STATUS_INVALID_PARAMETER);
  CHECK(creates == 0);
}

void test_reentrant_reads() {
  for (bool ready_first : {false, true}) {
    fixture f;
    on_start = [&](VHFHANDLE h) {
      if (ready_first) ready(h);
      CHECK(output(slot_of(h)) == STATUS_SUCCESS);
      CHECK(submits == (ready_first ? 1 : 0));
      if (!ready_first) ready(h);
      CHECK(submits == 1);
      CHECK(!slot_of(h).pump.ready());
      // Queued priority reply must not overwrite the retained packet/payload.
      CHECK(output(slot_of(h)) == STATUS_SUCCESS);
      CHECK(submits == 1); retained_unchanged(h);
      bool reentered = false;
      on_submit = [&](VHFHANDLE submitted) {
        if (!reentered) { reentered = true; ready(submitted); }
      };
      ready(h);
      CHECK(reentered); CHECK(submits == 2);
      // Operation completion can itself reenter a registered callback.
      bool completed = false;
      on_complete = [&] {
        if (!completed) { completed = true; CHECK(input(slot_of(h)) == STATUS_SUCCESS); }
      };
      CHECK(input(slot_of(h)) == STATUS_SUCCESS); CHECK(completed);
      on_complete = {}; on_submit = {};
    };
    CHECK(f.create(lvg::profile::switch_pro) == STATUS_SUCCESS);
  }
  fixture f;
  submit_result = STATUS_NOT_SUPPORTED;
  on_start = [&](VHFHANDLE h) {
    ready(h); CHECK(output(slot_of(h)) == STATUS_SUCCESS); CHECK(submits == 1);
    CHECK(!slot_of(h).pump.ready());
    CHECK(output(slot_of(h)) == STATUS_SUCCESS); CHECK(submits == 1);
    ready(h); CHECK(submits == 2);
  };
  CHECK(f.create(lvg::profile::switch_pro) == STATUS_SUCCESS);
}

// Reserved generic profiles remain unavailable through create_controller. Direct
// setup exercises their dormant callback branches without advertising/enabling
// those identities. State combinations are adversarial admission tests, not
// claims that VHF calls before VhfStart or after synchronous deletion.
void test_callback_admission() {
  using namespace lvg::driver;
  fixture f;
  for (auto profile : {lvg::profile::dualshock_4, lvg::profile::dualsense,
       lvg::profile::switch_pro, lvg::profile::xbox_one, lvg::profile::xbox_series,
       lvg::profile::generic_hid, lvg::profile::generic_pid}) {
    for (auto state : {slot_state::empty, slot_state::starting, slot_state::active, slot_state::stopping}) {
      for (bool stopping : {false, true}) for (bool attached : {false, true}) {
        auto &slot = f.context.controllers[0];
        reset_slot(&slot); slot.state = state; slot.owner = &f.owner;
        slot.selected_profile = profile; slot.force_feedback = profile == lvg::profile::generic_pid;
        slot.pid.reset(); slot.ds4.reset(); slot.ds5.reset(); slot.switch_pro.reset(); slot.pump.reset();
        fake_vhf handle {}; handle.config.VhfClientContext = &slot;
        slot.vhf = attached ? &handle : nullptr; f.context.stopping = stopping;
        const bool admitted = attached && !stopping &&
          (state == slot_state::starting || state == slot_state::active);
        CHECK(input(slot) == (admitted ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY));
        CHECK(output(slot) == (admitted ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY));
        if (is_playstation(profile)) {
          CHECK(feature(slot) == (admitted ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY));
          CHECK(feature(slot, true) == (admitted ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY));
        }
        if (slot.force_feedback) {
          pid_create_new_effect_report create {k_pid_create_new_effect_report_id,
            static_cast<std::uint8_t>(pid_effect_type::constant_force), 0};
          HID_XFER_PACKET request {reinterpret_cast<PUCHAR>(&create), sizeof(create), create.report_id};
          CHECK(call(evt_vhf_set_feature, slot, &request) ==
                (admitted ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY));
          std::array<UCHAR, 64> buffer {};
          HID_XFER_PACKET query {buffer.data(), 64, k_pid_pool_report_id};
          CHECK(call(evt_vhf_get_feature, slot, &query) ==
                (admitted ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY));
          pid_device_gain_report gain {k_pid_device_gain_report_id, 100};
          HID_XFER_PACKET write {reinterpret_cast<PUCHAR>(&gain), sizeof(gain), gain.report_id};
          CHECK(call(evt_vhf_write_report, slot, &write) ==
                (admitted ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY));
        }
        const bool queued = !slot.pump.empty();
        handle.credit = admitted;
        evt_vhf_ready_for_next_report(&slot);
        CHECK(slot.pump.ready() == (admitted && !queued));
        if (!admitted) CHECK(!slot.feedback_pending);
        reset_slot(&slot);
      }
    }
  }
  f.context.stopping = false;
  const std::array<EVT_VHF_ASYNC_OPERATION *, 4> callbacks {
    evt_vhf_write_report, evt_vhf_get_feature, evt_vhf_set_feature, evt_vhf_get_input_report};
  for (auto callback : callbacks) {
    fake_operation op;
    callback(nullptr, &op, nullptr, nullptr);
    CHECK(op.completions == 1); CHECK(op.status == STATUS_INVALID_DEVICE_REQUEST);
    callback(nullptr, nullptr, nullptr, nullptr); // No operation to complete.
  }
  evt_vhf_ready_for_next_report(nullptr);
}

void test_release_restart() {
  fixture f;
  for (std::uint32_t n = 0; n < profiles.size(); ++n) CHECK(f.create(profiles[n], n) == STATUS_SUCCESS);
  fake_close_hook = [&] { for (auto &h : handles) CHECK(h->deleted); };
  on_delete = [&](VHFHANDLE h) {
    CHECK(f.context.stopping); CHECK(f.context.vhf_file_handle == nullptr);
    CHECK(input(slot_of(h)) == STATUS_DEVICE_NOT_READY);
  };
  CHECK(evt_release_hardware(&f.device, nullptr) == STATUS_SUCCESS);
  CHECK(deletes == static_cast<int>(profiles.size())); CHECK(fake_closes == 1);
  for (auto &slot : f.context.controllers) CHECK(slot.state == slot_state::empty);
  CHECK(!f.timer.armed); CHECK(!f.context.vhf_target_open);
  CHECK(evt_prepare_hardware(&f.device, nullptr, nullptr) == STATUS_SUCCESS);
  CHECK(!f.context.stopping); CHECK(fake_opens == 2);
  CHECK(f.create(lvg::profile::dualsense) == STATUS_SUCCESS);
  fake_close_hook = {};
  evt_vhf_target_cleanup(&f.target);
  CHECK(f.context.local_vhf_target == nullptr); CHECK(!f.context.vhf_target_open);
  CHECK(fake_closes == 1); // Cleanup lets WDF close the still-valid target.
  CHECK(f.context.controllers[0].state == slot_state::empty);
  CHECK(f.create(lvg::profile::dualsense) == STATUS_DEVICE_NOT_READY);
  const int before = deletes;
  evt_vhf_target_cleanup(&f.target); CHECK(deletes == before);
}

void test_concurrent_teardown() {
  for (int mode = 0; mode < 3; ++mode) {
    fixture f;
    std::future<void> teardown;
    on_start = [&](VHFHANDLE h) {
      teardown = std::async(std::launch::async, [&] {
        if (mode == 0) evt_file_close(&f.owner);
        else if (mode == 1) static_cast<void>(evt_release_hardware(&f.device, nullptr));
        else evt_vhf_target_cleanup(&f.target);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (f.lifetime.waiters == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
      CHECK(f.lifetime.waiters == 1);
      CHECK(teardown.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
      check_initial_callbacks(h); // No callback takes the blocked outer gate.
    };
    CHECK(f.create(lvg::profile::dualsense) == STATUS_SUCCESS);
    teardown.get();
    CHECK(deletes == 1); CHECK(f.context.controllers[0].state == slot_state::empty);
  }
}
} // namespace

NTSTATUS VhfCreate(VHF_CONFIG *config, VHFHANDLE *out) {
  ++creates;
  auto &slot = *static_cast<controller_slot *>(config->VhfClientContext);
  boundary(slot); CHECK(fake_holds(slot.parent->lifetime_gate));
  CHECK(config->FileHandle != nullptr); CHECK(config->ReportDescriptorLength != 0);
  CHECK(config->VendorID != 0); CHECK(config->ProductID != 0);
  if (!NT_SUCCESS(create_result)) return create_result;
  handles.push_back(std::make_unique<fake_vhf>());
  *out = handles.back().get(); (*out)->config = *config;
  return STATUS_SUCCESS;
}
NTSTATUS VhfStart(VHFHANDLE h) {
  ++starts; boundary(slot_of(h)); CHECK(!h->deleted);
  CHECK(fake_holds(slot_of(h).parent->lifetime_gate));
  if (on_start) on_start(h);
  return start_result;
}
void VhfDelete(VHFHANDLE h, BOOLEAN wait) {
  ++deletes; boundary(slot_of(h)); CHECK(wait); CHECK(!h->deleted); CHECK(!h->deleting);
  CHECK(fake_holds(slot_of(h).parent->lifetime_gate)); h->deleting = true;
  retained_unchanged(h);
  if (on_delete) on_delete(h);
  retained_unchanged(h);
  h->config.EvtVhfCleanup(h->config.VhfClientContext);
  h->retained = nullptr; h->deleted = true; h->deleting = false;
}
NTSTATUS VhfAsyncOperationComplete(VHFOPERATIONHANDLE op, NTSTATUS status) {
  // state_lock must be dropped before any potentially reentrant VHF call.
  CHECK(!fake_holds(static_cast<WDFWAITLOCK>(op->state_lock)));
  ++op->completions; op->status = status;
  if (on_complete) on_complete();
  return STATUS_SUCCESS;
}
NTSTATUS VhfReadReportSubmit(VHFHANDLE h, PHID_XFER_PACKET packet) {
  ++submits; boundary(slot_of(h)); CHECK(!h->deleted); CHECK(!h->deleting);
  CHECK(h->credit); h->credit = false;
  CHECK(packet->reportBuffer != nullptr); CHECK(packet->reportBufferLen <= 64);
  h->retained = packet; h->packet = *packet;
  h->bytes.assign(packet->reportBuffer, packet->reportBuffer + packet->reportBufferLen);
  if (on_submit) on_submit(h);
  return submit_result;
}
int main() {
  test_profiles(); test_start_rollback(); test_prestart_failures();
  test_reentrant_reads(); test_callback_admission(); test_release_restart(); test_concurrent_teardown();
  std::printf("VHF production lifecycle: %d checks, %d failures\n", checks.load(), failures.load());
  return failures == 0 ? 0 : 1;
}
