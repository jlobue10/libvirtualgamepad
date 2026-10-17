#!/usr/bin/env python3
"""Execute the production feedback-drop accounting with deterministic WDF/VHF fakes.

Tests the counters, not WDK compatibility: the real haptic and settings rings
(steam_haptic_queue.h), the real set-feature callback that fills the settings
ring, poll_feedback, feedback_counters_of, query_controller_info, fill_query_info
and reset_slot run as written in driver.cpp against a fake slot. The wire
structs are the real protocol.h, compiled on any platform through a two-file
<windows.h>/<winioctl.h> shim that defines only what protocol.h uses.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT/'driver/src/driver.cpp').read_text()

def function(signature):
    start = source.index(signature + ' {')
    end = source.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == '{') - (source[end] == '}')
    return source[start:end+1]

def constant(name):
    match = re.search(r'^constexpr [^\n]*\b' + re.escape(name) + r'\s*=[^\n]*;', source, re.MULTILINE)
    if match is None:
        raise ValueError('Production constant not found: ' + name)
    return match.group(0)

def pin(pattern, what):
    # The haptic call site lives in evt_vhf_write_report, which folds in every
    # other profile's output path; pin its argument list instead of faking all
    # of that. The IOCTL dispatch is pinned the same way.
    if re.search(pattern, source) is None:
        raise SystemExit('FAIL production source no longer ' + what)
    print('PASS production source ' + what)

pin(r'enqueue_haptic\(slot->haptic_queue, slot->haptic_head, slot->haptic_count, event,\s*&slot->haptic_evicted\)',
    'counts haptic evictions into the slot at the write-report call site')
pin(r'valid_request\(static_cast<const lvg::controller_id_request \*>\(raw\), bytes\)',
    'accepts a controller_id_request on ioctl_query_info')
pin(r'query_controller_info\(context, owner, controller_query, output\)',
    'answers the controller shape of ioctl_query_info with query_controller_info')

windows_shim = r'''
#pragma once
#include <cstdint>
typedef std::uint32_t DWORD;
struct GUID { std::uint32_t Data1; std::uint16_t Data2; std::uint16_t Data3; std::uint8_t Data4[8]; };
'''
winioctl_shim = r'''
#pragma once
#define FILE_DEVICE_UNKNOWN 0x22
#define METHOD_BUFFERED 0
#define FILE_READ_DATA 1
#define FILE_WRITE_DATA 2
#define CTL_CODE(DeviceType, Function, Method, Access) \
  (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
'''

prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include "libvirtualgamepad/protocol.h"
#include "steam_haptic_queue.h"
using PVOID=void*; using VHFHANDLE=void*; using VHFOPERATIONHANDLE=void*; using WDFFILEOBJECT=void*;
using ULONG=std::uint32_t; using UCHAR=std::uint8_t; using PUCHAR=std::uint8_t*; using NTSTATUS=int;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_NOT_FOUND (-2)
#define STATUS_ACCESS_DENIED (-3)
#define STATUS_DEVICE_NOT_READY (-4)
#define STATUS_NO_MORE_ENTRIES (-5)
#define STATUS_INVALID_DEVICE_REQUEST (-6)
#define NT_SUCCESS(s) ((s)==0)
struct HID_XFER_PACKET { PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId; };
using PHID_XFER_PACKET = HID_XFER_PACKET*;
bool locked=false; int completions=0; NTSTATUS last_completion=99; std::uint8_t next_chunk=0;
void VhfAsyncOperationComplete(VHFOPERATIONHANDLE h, NTSTATUS s){ assert(h==(void*)7); ++completions; last_completion=s; }
// PRODUCTION_CONSTANTS
enum class slot_state : std::uint8_t { empty, starting, active, stopping };
constexpr std::uint8_t k_pid_create_new_effect_report_id = 0x1B;
struct pid_create_new_effect_report { std::uint8_t id; };
template<class report_t> bool read_pid_report(const PHID_XFER_PACKET, report_t*) { return false; }
namespace lvg::driver {
 [[nodiscard]] profile_mask_t available_profiles() noexcept { return profile_bit(profile::steam_controller); }
 bool is_steam_controller(profile p){ return p==profile::steam_controller; }
 bool is_playstation(profile p){ return p==profile::dualshock_4 || p==profile::dualsense; }
 constexpr std::uint8_t k_sc26_features_report_id = 0x01;
 struct sc26_state { struct { std::uint32_t unknown_commands; } features; };
 struct ds4_state { int features; }; struct ds5_state { int features; };
 // Only the counter is read from the pump here; it asserts the production reader holds state_lock.
 struct fake_pump { std::uint32_t dropped_; std::uint32_t dropped_transitions() const noexcept { assert(locked); return dropped_; } };
 bool set_sc26_feature(UCHAR, PUCHAR buffer, ULONG, sc26_state*) { return buffer != nullptr; }
 // A settings write always splits into `capacity` events here; each carries its sequence in payload[0].
 std::size_t apply_sc26_settings(PUCHAR, ULONG, std::uint32_t id, feedback_event* events, std::size_t capacity) {
  assert(locked);
  for (std::size_t i = 0; i < capacity; ++i) { events[i] = {}; events[i].controller_id = id; events[i].type = feedback_type::steam_settings;
   events[i].payload_size = sizeof(steam_settings_feedback); events[i].payload[0] = next_chunk++; }
  return capacity; }
}
namespace lvg::ds4_usb { bool set_feature(UCHAR, PUCHAR, ULONG, int&) { return true; } }
namespace lvg::ds5_usb { bool set_feature(UCHAR, PUCHAR, ULONG, int&) { return true; } }
struct fake_pid { bool create_new_effect(const pid_create_new_effect_report&) { return true; } };
struct device_context;
struct controller_slot {
 device_context* parent; VHFHANDLE vhf; WDFFILEOBJECT owner; lvg::profile selected_profile; std::uint32_t controller_id;
 slot_state state; bool feedback_pending; lvg::feedback_event feedback;
 lvg::feedback_event haptic_queue[k_haptic_queue_capacity]; std::uint8_t haptic_head; std::uint8_t haptic_count;
 lvg::feedback_event settings_queue[k_settings_queue_capacity]; std::uint8_t settings_head; std::uint8_t settings_count;
 std::uint32_t haptic_evicted; std::uint32_t settings_evicted;
 bool force_feedback; lvg::driver::ds4_state ds4; lvg::driver::ds5_state ds5; lvg::driver::sc26_state sc26;
 lvg::driver::fake_pump pump; fake_pid pid; };
struct device_context { bool stopping; controller_slot controllers[lvg::k_max_controllers]; };
void lock_context(device_context*){ assert(!locked); locked=true; }
void unlock_context(device_context*){ assert(locked); locked=false; }
'''

suffix = r'''
static lvg::feedback_event haptic_event(bool stop) {
  lvg::feedback_event event {}; event.type = lvg::feedback_type::steam_haptic; event.payload_size = sizeof(lvg::steam_haptic_feedback);
  lvg::steam_haptic_feedback haptic {}; haptic.length = 12; haptic.report[0] = 0x81; haptic.report[1] = 0;
  if (!stop) { haptic.report[2] = 3; haptic.report[4] = 1; haptic.report[6] = 1; haptic.report[7] = 1; }
  std::memcpy(event.payload, &haptic, sizeof(haptic)); return event; }
int main(){ int failures=0;
 auto check=[&](bool ok,const char* text){ printf("%s %s\n",ok?"PASS":"FAIL",text); failures+=!ok; };
 static device_context c {}; auto& s=c.controllers[0]; s.parent=&c; s.owner=(void*)1; s.state=slot_state::active;
 s.vhf=(void*)1; // a published handle: the production admission gate serves callbacks only with one
 s.selected_profile=lvg::profile::steam_controller; s.controller_id=0; c.controllers[1].controller_id=1;
 lvg::controller_id_request q {}; q.controller_id=0; lvg::query_info_response info {};
 auto query=[&](std::uint32_t id, WDFFILEOBJECT owner){ q.controller_id=id; info={}; return query_controller_info(&c,owner,q,&info); };
 auto zero=[&](const lvg::feedback_counters& k){ return k.haptic_evicted==0 && k.settings_evicted==0 && k.transitions_dropped==0 && k.unknown_commands==0; };
 check(NT_SUCCESS(query(0,(void*)1)) && zero(info.counters) && info.header.size==sizeof(info) && info.header.version==lvg::k_protocol_version
       && info.maximum_controllers==lvg::k_max_controllers && info.available_profiles==lvg::profile_bit(lvg::profile::steam_controller) && !locked,
       "a fresh slot answers the controller query with the device fields and zero counters");
 check(query(1,(void*)1)==STATUS_NOT_FOUND && query(0,(void*)2)==STATUS_ACCESS_DENIED && query(lvg::k_max_controllers,(void*)1)==STATUS_INVALID_PARAMETER,
       "the controller query uses poll_feedback's ladder: empty, then not owned, and refuses an out-of-range id");
 // Haptic ring: the production enqueue with the slot's counter, as evt_vhf_write_report calls it.
 for (unsigned n=0;n<k_haptic_queue_capacity;++n) std::ignore=lvg::driver::enqueue_haptic(s.haptic_queue,s.haptic_head,s.haptic_count,haptic_event(false),&s.haptic_evicted);
 check(s.haptic_count==k_haptic_queue_capacity && s.haptic_evicted==0, "filling the haptic ring exactly evicts nothing");
 for (unsigned n=0;n<5;++n) std::ignore=lvg::driver::enqueue_haptic(s.haptic_queue,s.haptic_head,s.haptic_count,haptic_event(false),&s.haptic_evicted);
 check(s.haptic_count==k_haptic_queue_capacity && s.haptic_evicted==5, "five ordinary events beyond capacity evict five");
 std::ignore=lvg::driver::enqueue_haptic(s.haptic_queue,s.haptic_head,s.haptic_count,haptic_event(true),&s.haptic_evicted);
 std::ignore=lvg::driver::enqueue_haptic(s.haptic_queue,s.haptic_head,s.haptic_count,haptic_event(true),&s.haptic_evicted);
 check(s.haptic_count==k_haptic_queue_capacity && s.haptic_evicted==6, "a stop into a full ring evicts one ordinary event; its repeat coalesces and is not counted");
 check(NT_SUCCESS(query(0,(void*)1)) && info.counters.haptic_evicted==6 && info.counters.settings_evicted==0, "the controller query reports the haptic evictions");
 // Settings ring through the production set-feature callback: three writes of three chunks into an eight-deep ring.
 std::uint8_t report[64] {}; report[0]=lvg::driver::k_sc26_features_report_id; HID_XFER_PACKET transfer { report, sizeof(report), lvg::driver::k_sc26_features_report_id };
 for (unsigned w=0;w<2;++w) evt_vhf_set_feature(&s,(void*)7,nullptr,&transfer);
 check(completions==2 && last_completion==STATUS_SUCCESS && s.settings_count==2*k_settings_events_per_write && s.settings_evicted==0,
       "two settings writes (six chunks) are queued whole and evict nothing");
 evt_vhf_set_feature(&s,(void*)7,nullptr,&transfer);
 check(last_completion==STATUS_SUCCESS && s.settings_count==k_settings_queue_capacity && s.settings_evicted==1 && s.settings_queue[s.settings_head].payload[0]==1,
       "the ninth undrained settings chunk evicts chunk 1 of the first write and counts it once");
 check(NT_SUCCESS(query(0,(void*)1)) && info.counters.haptic_evicted==6 && info.counters.settings_evicted==1 && !locked,
       "the controller query reports the settings eviction next to the haptic one");
 // Draining does not move the counters: they count what the host never saw.
 lvg::feedback_event out {}; unsigned settings_seen=0, haptics_seen=0; NTSTATUS st;
 while (NT_SUCCESS(st=poll_feedback(&c,(void*)1,q,&out))) { settings_seen += out.type==lvg::feedback_type::steam_settings; haptics_seen += out.type==lvg::feedback_type::steam_haptic; }
 check(st==STATUS_NO_MORE_ENTRIES && settings_seen==k_settings_queue_capacity && haptics_seen==k_haptic_queue_capacity, "polling drains settings ahead of haptics and ends with STATUS_NO_MORE_ENTRIES");
 check(NT_SUCCESS(query(0,(void*)1)) && info.counters.haptic_evicted==6 && info.counters.settings_evicted==1, "draining the rings leaves the counters monotonic");
 s.pump.dropped_=3; s.sc26.features.unknown_commands=2;
 check(NT_SUCCESS(query(0,(void*)1)) && info.counters.transitions_dropped==3 && info.counters.unknown_commands==2, "the pump's dropped transitions and the feature channel's unknown commands ride along");
 s.haptic_evicted=1000; s.pump.dropped_=0x10000;
 check(NT_SUCCESS(query(0,(void*)1)) && info.counters.haptic_evicted==255 && info.counters.transitions_dropped==255 && s.haptic_evicted==1000, "the wire saturates at 255 while the slot keeps the full count");
 s.state=slot_state::stopping;
 check(NT_SUCCESS(query(0,(void*)1)) && info.counters.haptic_evicted==255, "a stopping slot still answers with what it dropped");
 // A set-feature on a slot that is not active does not touch the ring.
 s.settings_evicted=0; s.settings_count=k_settings_queue_capacity; completions=0; evt_vhf_set_feature(&s,(void*)7,nullptr,&transfer);
 check(completions==1 && last_completion==STATUS_DEVICE_NOT_READY && s.settings_evicted==0 && !locked, "a stopping slot refuses the write and evicts nothing");
 reset_slot(&s);
 check(s.state==slot_state::empty && s.haptic_evicted==0 && s.settings_evicted==0 && s.pump.dropped_==0 && s.sc26.features.unknown_commands==0 && s.controller_id==0 && s.parent==&c,
       "releasing the slot zeroes every counter and keeps its identity");
 check(query(0,(void*)1)==STATUS_NOT_FOUND, "a released slot answers STATUS_NOT_FOUND");
 fill_query_info(&info); info.counters.haptic_evicted=1; fill_query_info(&info);
 check(zero(info.counters) && info.header.size==sizeof(info), "the device query clears the counters word, as hosts built before it expect");
 return failures!=0; }
'''

signatures = [
 '[[nodiscard]] bool accepts_vhf_callbacks(const controller_slot &slot) noexcept',
 '[[nodiscard]] bool is_owned_by(\n  const controller_slot &slot,\n  const WDFFILEOBJECT owner) noexcept',
 'void reset_slot(controller_slot *slot) noexcept',
 'void fill_query_info(lvg::query_info_response *const output) noexcept',
 '[[nodiscard]] lvg::feedback_counters feedback_counters_of(const controller_slot &slot) noexcept',
 '[[nodiscard]] NTSTATUS query_controller_info(\n  device_context *const context,\n  const WDFFILEOBJECT owner,\n'
 '  const lvg::controller_id_request &request,\n  lvg::query_info_response *const output) noexcept',
 '[[nodiscard]] NTSTATUS poll_feedback(\n  device_context *const context,\n  const WDFFILEOBJECT owner,\n'
 '  const lvg::controller_id_request &request,\n  lvg::feedback_event *const output) noexcept',
 'void evt_vhf_set_feature(\n  PVOID vhf_client_context,\n  VHFOPERATIONHANDLE operation_handle,\n  PVOID,\n  PHID_XFER_PACKET transfer)']
prefix = prefix.replace('// PRODUCTION_CONSTANTS', '\n'.join(
    constant(name) for name in ('k_haptic_queue_capacity', 'k_settings_queue_capacity', 'k_settings_events_per_write')))
with tempfile.TemporaryDirectory(prefix='sc26-counters-') as directory:
    work = pathlib.Path(directory)
    (work/'shim').mkdir()
    (work/'shim'/'windows.h').write_text(windows_shim)
    (work/'shim'/'winioctl.h').write_text(winioctl_shim)
    (work/'test.cpp').write_text(prefix + '\n'.join(map(function, signatures)) + suffix)
    sanitize = [] if sys.platform == 'win32' else ['-fsanitize=address,undefined']
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', *sanitize,
                    '-I', str(work/'shim'), '-I', str(ROOT/'include'), '-I', str(ROOT/'driver/src'),
                    str(work/'test.cpp'), '-o', str(work/'test')], check=True)
    raise SystemExit(subprocess.run([str(work/'test')]).returncode)
