#!/usr/bin/env python3
"""Execute production keep-alive functions with deterministic Windows/VHF fakes.

Tests scheduling/lifetime logic, not WDK compatibility or real timer precision:
the tick, arming, the take-time clock stamp (mark_sc26_report_taken), the
sequence numbering, pump_report_locked and submit_taken run as written in
driver.cpp against a pump fake with the production take order.
Pass --baseline to compare against the checked-out commit before local edits.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (subprocess.check_output(['git', 'show', 'HEAD:driver/src/driver.cpp'], cwd=ROOT, text=True)
          if '--baseline' in sys.argv else (ROOT/'driver/src/driver.cpp').read_text())

def function(signature):
    start = source.index(signature + ' {')
    end = source.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == '{') - (source[end] == '}')
    return source[start:end+1]

def constant(name):
    # Copy the real timing values as well as the real functions. A fake that
    # equates the early-wake threshold with the tick can hide cadence bugs.
    match = re.search(r'^constexpr [^\n]*\b' + re.escape(name) + r'\s*=[^\n]*;', source, re.MULTILINE)
    if match is None:
        raise ValueError('Production constant not found: ' + name)
    return match.group(0)

prefix = r'''
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <cstdio>
#include <cassert>
using HANDLE=void*; using VHFHANDLE=void*; using LONGLONG=long long; using LONG=std::int32_t; using ULONG=std::uint32_t;
using UCHAR=std::uint8_t; using PUCHAR=std::uint8_t*;
using NTSTATUS=int; struct LARGE_INTEGER{long long QuadPart;};
#define FALSE 0
#define INFINITE 0xffffffff
#define THREAD_PRIORITY_HIGHEST 2
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY (-1)
#define STATUS_INVALID_PARAMETER (-2)
#define NT_SUCCESS(s) ((s)==0)
struct HID_XFER_PACKET{PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId;};
int batteries_enqueued=0,states_enqueued=0,submitted=0; NTSTATUS vhf_submit_status=0; UCHAR last_submitted_id=0;
NTSTATUS VhfReadReportSubmit(VHFHANDLE h,HID_XFER_PACKET* p){assert(h==(void*)1);last_submitted_id=p->reportId;++submitted;return vhf_submit_status;}
namespace lvg { constexpr int k_max_controllers=2;
namespace driver {
 enum class report_kind {continuous,transition,aside};
 struct report_buffer {std::uint8_t data[64]; std::uint32_t length; std::uint8_t report_id;};
 struct sc26_state {uint64_t last_report_us=0; uint64_t last_battery_us=0;};
 struct ds4_state{}; struct ds5_state{}; struct switch_state{};
 using sc26_input_report=int; struct sc26_battery_report{int value;};
 void sc26_tick(sc26_state*, uint64_t){} int encode_sc26_input(int state,sc26_state*){return state;}
 sc26_battery_report encode_sc26_battery(const sc26_state&){return {7};}
 constexpr std::uint8_t k_sc26_input_report_id=0x42; constexpr std::uint8_t k_sc26_battery_report_id=0x43;
 // The production pump's contract: asides are taken before the latest
 // continuous snapshot, a take clears readiness, nothing is taken while VHF
 // has no read pending.
 struct fake_pump {bool ready_=true; bool have_latest=false; report_buffer latest{}; report_buffer asides[8]{}; int aside_count=0;
  bool ready() const {return ready_;} void set_ready(){ready_=true;}
  bool enqueue(const void* d,ULONG len,UCHAR id,report_kind k){report_buffer b{};assert(len<=sizeof(b.data));memcpy(b.data,d,len);b.length=len;b.report_id=id;
   if(k==report_kind::aside){assert(aside_count<8);asides[aside_count++]=b;if(id==k_sc26_battery_report_id)++batteries_enqueued;}
   else{latest=b;have_latest=true;if(id==k_sc26_input_report_id)++states_enqueued;}return true;}
  bool take(report_buffer* out){if(!ready_)return false;
   if(aside_count){*out=asides[0];for(int i=1;i<aside_count;++i)asides[i-1]=asides[i];--aside_count;ready_=false;return true;}
   if(have_latest){*out=latest;have_latest=false;ready_=false;return true;}return false;}};
}}
enum class slot_state{active,free};
struct controller_slot {slot_state state=slot_state::active; int selected_profile=1;
 bool have_last_input=true; VHFHANDLE vhf=(void*)1; int last_input=42; std::uint8_t sc26_wire_sequence=0;
 lvg::driver::fake_pump pump; lvg::driver::sc26_state sc26; int submits_in_flight=0;
 lvg::driver::ds4_state ds4; lvg::driver::ds5_state ds5; lvg::driver::switch_state switch_pro;};
struct device_context {controller_slot controllers[2]; bool stopping=false,sc26_timer_running=true,sc26_timer_parked=false;
 HANDLE sc26_keepalive_timer=(void*)1,sc26_keepalive_thread=nullptr,sc26_keepalive_stop=(void*)1;};
// PRODUCTION_TIMING_CONSTANTS
uint64_t clock_us=0; long long timer_due=0; bool locked=false,inspect_release=false,protected_release=false;
int arms=0; device_context *joining=nullptr;
uint64_t now_us(){return clock_us;} bool is_steam_controller(int p){return p==1;}
void lock_context(device_context*){assert(!locked);locked=true;}
void unlock_context(device_context* c){assert(locked);locked=false;
 if(inspect_release){protected_release=c->controllers[0].submits_in_flight>0 && c->controllers[1].submits_in_flight>0;inspect_release=false;}}
bool SetWaitableTimer(HANDLE,LARGE_INTEGER* d,int,void*,void*,int){timer_due=d->QuadPart;++arms;return true;}
void arm_sc26_keepalive(device_context *const context) noexcept;
int submit_profile_report(device_context*,controller_slot&){++submitted;return 0;}
// submit_motion_state scaffolding: the fold always applies; the slot is the SC26 one.
using WDFFILEOBJECT=void*; int lifetime_locks=0;
void lock_lifetime(device_context*){++lifetime_locks;} void unlock_lifetime(device_context*){--lifetime_locks;}
NTSTATUS begin_state_update(device_context* c,WDFFILEOBJECT,int id,controller_slot** out){*out=&c->controllers[id];return 0;}
int pumped_asides=0;
NTSTATUS pump_report(device_context*,controller_slot&,const void*,ULONG,UCHAR id,lvg::driver::report_kind k){
 assert(id==lvg::driver::k_sc26_battery_report_id&&k==lvg::driver::report_kind::aside);++pumped_asides;return 0;}
namespace lvg { enum class profile {xbox_360,dualshock_4,dualsense,switch_pro,steam_controller};
 struct motion_state_request {int controller_id;};
 struct battery_state_request {int controller_id;};
 namespace driver {
  bool apply_ds4_motion(const motion_state_request&,ds4_state*){return true;}
  bool apply_ds5_motion(const motion_state_request&,ds5_state*){return true;}
  bool apply_switch_motion(const motion_state_request&,switch_state*){return true;}
  bool apply_sc26_motion(const motion_state_request&,sc26_state*,uint64_t){return true;}
  bool apply_ds4_battery(const battery_state_request&,ds4_state*){return true;}
  bool apply_ds5_battery(const battery_state_request&,ds5_state*){return true;}
  bool apply_sc26_battery(const battery_state_request&,sc26_state*){return true;}
  bool apply_switch_battery(const battery_state_request&,switch_state*){return true;} } }
void SwitchToThread(){}
void WaitForSingleObject(HANDLE,unsigned){joining->sc26_timer_running=false;}
void CloseHandle(HANDLE){} void ResetEvent(HANDLE){} void CancelWaitableTimer(HANDLE){}
void sc26_keepalive_thread(void*){} HANDLE CreateThread(void*,int,void(*)(void*),void*,int,void*){return (void*)2;}
void SetThreadPriority(HANDLE,int){}
'''
suffix = r'''
int main(){int failures=0;
 auto check=[&](bool ok,const char* text){printf("%s %s\n",ok?"PASS":"FAIL",text);failures+=!ok;};
 device_context c;clock_us=3000;c.controllers[0].sc26.last_report_us=3000;c.controllers[1].sc26.last_report_us=3000;
 lock_context(&c);arm_sc26_keepalive(&c);unlock_context(&c);
 check(timer_due==-40000,"a fresh report arms the full 4ms tick, not the early-wake threshold");
 c.controllers[1].sc26.last_report_us=0;
 lock_context(&c);arm_sc26_keepalive(&c);unlock_context(&c);
 check(timer_due==-10000,"busy controller preserves idle controller's 4ms deadline");
 clock_us=8000;inspect_release=true;arms=0;sc26_keepalive_tick(&c);
 check(protected_release && submitted==2,"VHF handles have in-flight references before unlocking");
 check(arms==1,"the tick arms the timer once, not once per resent controller");
 check(batteries_enqueued==0,"battery is not queued while the driver clock is younger than a period");
 check(c.controllers[0].sc26.last_report_us==8000 && c.controllers[1].sc26.last_report_us==8000,
       "a resent state report stamps the keep-alive clock when it is taken");
 check(!c.controllers[0].pump.ready() && c.controllers[0].submits_in_flight==0,"a take clears readiness and the submit returns the in-flight count");
 // The readiness callbacks come back; the resend numbered the reports.
 c.controllers[0].pump.set_ready();c.controllers[1].pump.set_ready();
 check(c.controllers[0].sc26_wire_sequence==1,"a taken state report carries the wire sequence");
 clock_us=20000;c.controllers[0].pump.ready_=false;c.controllers[0].sc26.last_report_us=0;c.controllers[1].sc26.last_report_us=20000;
 submitted=0;batteries_enqueued=0;sc26_keepalive_tick(&c);
 check(submitted==0 && timer_due==-40000,"a slot without a pending VHF read is skipped and the timer idles at the tick");
 check(batteries_enqueued==0,"battery is not repeated inside its period");
 clock_us=8000+k_sc26_battery_period_us;c.controllers[1].sc26.last_report_us=clock_us;submitted=0;sc26_keepalive_tick(&c);
 check(batteries_enqueued==1 && submitted==1 && last_submitted_id==lvg::driver::k_sc26_battery_report_id,
       "battery period elapsed: the aside is queued and taken even without a state resend");
 check(c.controllers[1].sc26.last_report_us==clock_us && c.controllers[1].sc26_wire_sequence==1,
       "a battery aside neither moves the keep-alive clock nor consumes a wire sequence number");
 c.controllers[1].pump.ready_=false;arms=0;timer_due=0;sc26_keepalive_tick(&c);
 check(arms==0 && c.sc26_timer_parked && c.sc26_timer_running,"no reader on any controller parks the timer instead of re-arming it");
 lock_context(&c);arm_sc26_keepalive(&c);unlock_context(&c);
 check(!c.sc26_timer_parked && arms==1,"a readiness arm resumes a parked timer");
 c.controllers[0].pump.ready_=true;c.controllers[1].pump.ready_=true;
 // Input path: a snapshot queued while VHF has no read pending is not stamped;
 // the stamp lands when the readiness callback takes it.
 {auto& s=c.controllers[0];s.pump.ready_=false;s.sc26.last_report_us=1;s.sc26_wire_sequence=0;int report=42;
  lvg::driver::report_buffer next{};bool have=false;VHFHANDLE vhf=nullptr;clock_us=50000;arms=0;
  lock_context(&c);auto st=pump_report_locked(&c,s,&report,sizeof(report),lvg::driver::k_sc26_input_report_id,lvg::driver::report_kind::continuous,&next,&have,&vhf);unlock_context(&c);
  check(NT_SUCCESS(st) && !have && s.sc26.last_report_us==1 && arms==0 && s.submits_in_flight==0,
        "a state report queued without a pending read is neither stamped nor armed");
  clock_us=52000;s.pump.set_ready();
  lock_context(&c);st=pump_report_locked(&c,s,nullptr,0,lvg::driver::k_sc26_input_report_id,lvg::driver::report_kind::continuous,&next,&have,&vhf);unlock_context(&c);
  check(NT_SUCCESS(st) && have && vhf==s.vhf && s.submits_in_flight==1 && next.data[1]==0 && s.sc26_wire_sequence==1,
        "the readiness callback takes the waiting snapshot with the handle, the in-flight count and the sequence");
  check(s.sc26.last_report_us==52000 && arms==1,"the keep-alive clock is stamped at take time and the timer re-armed");
  submitted=0;check(NT_SUCCESS(submit_taken(&c,s,next,vhf)) && submitted==1 && last_submitted_id==lvg::driver::k_sc26_input_report_id && s.submits_in_flight==0,
        "submit_taken hands the taken report to VHF and releases the in-flight count");
  // The tick's rearm=false path leaves arming to the tick itself.
  s.pump.set_ready();arms=0;clock_us=60000;
  lock_context(&c);st=pump_report_locked(&c,s,&report,sizeof(report),lvg::driver::k_sc26_input_report_id,lvg::driver::report_kind::continuous,&next,&have,&vhf,false);unlock_context(&c);
  check(have && s.sc26.last_report_us==60000 && arms==0,"rearm=false stamps the clock but leaves the timer to the caller");
  std::ignore=submit_taken(&c,s,next,vhf);
  // A failed submit while the worker is parked is the only chance to arm again.
  s.pump.set_ready();
  lock_context(&c);std::ignore=pump_report_locked(&c,s,&report,sizeof(report),lvg::driver::k_sc26_input_report_id,lvg::driver::report_kind::continuous,&next,&have,&vhf,false);unlock_context(&c);
  c.sc26_timer_parked=true;vhf_submit_status=-5;arms=0;
  check(!NT_SUCCESS(submit_taken(&c,s,next,vhf)) && s.pump.ready() && s.submits_in_flight==0 && arms==1,
        "a failed submit restores readiness and arms a parked keep-alive");
  vhf_submit_status=0;c.sc26_timer_parked=false;}
 c.sc26_keepalive_thread=(void*)1;joining=&c;start_sc26_keepalive(&c);
 check(c.sc26_timer_running,"exiting worker cannot clear replacement worker's running flag");
 {// D14-01: with a running worker a motion sample is folded and carried by the next tick;
  // without one (no timer, or the cadence not started) it is sent immediately.
  device_context m; lvg::motion_state_request req{0}; submitted=0;
  check(NT_SUCCESS(submit_motion_state(&m,nullptr,req)) && submitted==0 && lifetime_locks==0,
        "motion sample with a running keep-alive worker is folded, not submitted");
  m.sc26_timer_running=false;
  check(NT_SUCCESS(submit_motion_state(&m,nullptr,req)) && submitted==1 && lifetime_locks==0,
        "motion sample without a running worker is submitted at once");
  m.sc26_timer_running=true; m.sc26_keepalive_timer=nullptr;
  check(NT_SUCCESS(submit_motion_state(&m,nullptr,req)) && submitted==2 && lifetime_locks==0,
        "motion sample without a keep-alive timer is submitted at once");
  m.controllers[0].selected_profile=(int)lvg::profile::switch_pro; m.sc26_keepalive_timer=(void*)1;
  check(NT_SUCCESS(submit_motion_state(&m,nullptr,req)) && submitted==3,
        "non-SC26 profiles always submit the motion sample");}
 {// D16-02: the client's battery update is queued as an aside only while a reader exists;
  // without one the period is cleared so the first ready tick sends the fresh value.
  device_context b; lvg::battery_state_request req{0}; pumped_asides=0; clock_us=10'000'000;
  b.controllers[0].pump.ready_=true;
  check(NT_SUCCESS(submit_battery_state(&b,nullptr,req)) && pumped_asides==1 && b.controllers[0].sc26.last_battery_us==clock_us,
        "a battery update with a pending read goes out as an aside and restarts the period");
  b.controllers[0].pump.ready_=false;
  check(NT_SUCCESS(submit_battery_state(&b,nullptr,req)) && pumped_asides==1 && b.controllers[0].sc26.last_battery_us==0,
        "a battery update without a reader is not queued and clears the period");
  b.controllers[1].pump.ready_=false; batteries_enqueued=0; b.controllers[0].pump.ready_=true;
  sc26_keepalive_tick(&b);
  check(batteries_enqueued==1,"the first ready tick after a reader-less update carries the battery");
  b.controllers[0].selected_profile=(int)lvg::profile::switch_pro; submitted=0;
  check(NT_SUCCESS(submit_battery_state(&b,nullptr,req)) && submitted==1 && pumped_asides==1,
        "non-SC26 profiles fold battery into a full report");}
 return failures!=0;}
'''
signatures = [
 'void stamp_sc26_sequence(controller_slot &slot, lvg::driver::report_buffer &report) noexcept',
 'void mark_sc26_report_taken(\n  device_context *const context,\n  controller_slot &slot,\n'
 '  lvg::driver::report_buffer &report,\n  const bool rearm_keepalive) noexcept',
 '[[nodiscard]] NTSTATUS pump_report_locked(\n  device_context *const context,\n  controller_slot &slot,\n'
 '  const void *const data,\n  const ULONG length,\n  const UCHAR report_id,\n  const lvg::driver::report_kind kind,\n'
 '  lvg::driver::report_buffer *const next,\n  bool *const have_next,\n  VHFHANDLE *const vhf,\n'
 '  const bool rearm_keepalive = true) noexcept',
 'NTSTATUS submit_taken(\n  device_context *const context,\n  controller_slot &slot,\n'
 '  const lvg::driver::report_buffer &next,\n  const VHFHANDLE vhf) noexcept',
 'bool sc26_keepalive_tick(device_context *const context) noexcept',
 'void arm_sc26_keepalive(device_context *const context) noexcept',
 'void start_sc26_keepalive(device_context *const context) noexcept']
prefix = prefix.replace('// PRODUCTION_TIMING_CONSTANTS', '\n'.join(
    constant(name) for name in ('k_sc26_tick_ms', 'k_sc26_resend_after_us', 'k_sc26_battery_period_us')))
with tempfile.TemporaryDirectory(prefix='sc26-keepalive-') as directory:
    work = pathlib.Path(directory)
    motion = function('[[nodiscard]] NTSTATUS submit_motion_state(\n  device_context *const context,\n'
                      '  const WDFFILEOBJECT owner,\n  const lvg::motion_state_request &request) noexcept')
    motion = motion.replace('slot->selected_profile == lvg::profile::', 'slot->selected_profile == (int)lvg::profile::')
    battery = function('[[nodiscard]] NTSTATUS submit_battery_state(\n  device_context *const context,\n'
                       '  const WDFFILEOBJECT owner,\n  const lvg::battery_state_request &request) noexcept')
    battery = battery.replace('slot->selected_profile == lvg::profile::', 'slot->selected_profile == (int)lvg::profile::')
    (work/'test.cpp').write_text(prefix+'\n'.join(map(function, signatures))+'\n'+motion+'\n'+battery+suffix)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined',
                    str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    raise SystemExit(subprocess.run([str(work/'test')]).returncode)
