#!/usr/bin/env python3
"""Execute production keep-alive functions with deterministic Windows/VHF fakes.

Tests scheduling/lifetime logic, not WDK compatibility or real timer precision.
Pass --baseline to compare against the checked-out commit before local edits.
"""
import pathlib
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

prefix = r'''
#include <algorithm>
#include <cstdint>
#include <tuple>
#include <cstdio>
#include <cassert>
using HANDLE=void*; using VHFHANDLE=void*; using LONGLONG=long long;
using NTSTATUS=int; struct LARGE_INTEGER{long long QuadPart;};
#define FALSE 0
#define INFINITE 0xffffffff
#define THREAD_PRIORITY_HIGHEST 2
#define NT_SUCCESS(s) ((s)==0)
namespace lvg { constexpr int k_max_controllers=2;
namespace driver {
 enum class report_kind {continuous}; struct report_buffer{int value;};
 struct sc26_state {uint64_t last_report_us=0;};
 void sc26_tick(sc26_state*, uint64_t){} int encode_sc26_input(int state,sc26_state*){return state;}
 constexpr int k_sc26_input_report_id=1;
}}
enum class slot_state{active,free};
struct controller_slot {slot_state state=slot_state::active; int selected_profile=1;
 bool have_last_input=true; VHFHANDLE vhf=(void*)1; int last_input=42;
 lvg::driver::sc26_state sc26; int submits_in_flight=0;};
struct device_context {controller_slot controllers[2]; bool stopping=false,sc26_timer_running=true;
 HANDLE sc26_keepalive_timer=(void*)1,sc26_keepalive_thread=nullptr,sc26_keepalive_stop=(void*)1;};
// The driver's values: a 4 ms tick, resent when at least 3 ms old (early-wake tolerance).
constexpr uint64_t k_sc26_resend_after_us=3000; constexpr int k_sc26_tick_ms=4;
uint64_t clock_us=0; long long timer_due=0; bool locked=false,inspect_release=false,protected_release=false;
int submitted=0; device_context *joining=nullptr;
uint64_t now_us(){return clock_us;} bool is_steam_controller(int p){return p==1;}
void lock_context(device_context*){assert(!locked);locked=true;}
void unlock_context(device_context* c){assert(locked);locked=false;
 if(inspect_release){protected_release=c->controllers[0].submits_in_flight>0 && c->controllers[1].submits_in_flight>0;inspect_release=false;}}
bool SetWaitableTimer(HANDLE,LARGE_INTEGER* d,int,void*,void*,int){timer_due=d->QuadPart;return true;}
void arm_sc26_keepalive(device_context *const context) noexcept;
int pump_report_locked(device_context* c,controller_slot& slot,const void*,unsigned,int,
 lvg::driver::report_kind,lvg::driver::report_buffer* next,bool* have,VHFHANDLE* vhf){
 assert(locked);slot.sc26.last_report_us=clock_us;arm_sc26_keepalive(c);
 ++slot.submits_in_flight;*have=true;*vhf=slot.vhf;next->value=slot.last_input;return 0;}
int submit_taken(device_context*,controller_slot& slot,const lvg::driver::report_buffer& next,VHFHANDLE vhf){
 assert(!locked && slot.submits_in_flight>0 && next.value==42 && vhf==slot.vhf);
 --slot.submits_in_flight;++submitted;return 0;}
int submit_profile_report(device_context*,controller_slot&){++submitted;return 0;}
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
 clock_us=8000;inspect_release=true;sc26_keepalive_tick(&c);
 check(protected_release && submitted==2,"VHF handles have in-flight references before unlocking");
 c.sc26_keepalive_thread=(void*)1;joining=&c;start_sc26_keepalive(&c);
 check(c.sc26_timer_running,"exiting worker cannot clear replacement worker's running flag");
 return failures!=0;}
'''
signatures = [
 'bool sc26_keepalive_tick(device_context *const context) noexcept',
 'void arm_sc26_keepalive(device_context *const context) noexcept',
 'void start_sc26_keepalive(device_context *const context) noexcept']
with tempfile.TemporaryDirectory(prefix='sc26-keepalive-') as directory:
    work = pathlib.Path(directory)
    (work/'test.cpp').write_text(prefix+'\n'.join(map(function, signatures))+suffix)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined',
                    str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    raise SystemExit(subprocess.run([str(work/'test')]).returncode)
