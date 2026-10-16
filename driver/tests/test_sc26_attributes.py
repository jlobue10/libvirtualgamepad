#!/usr/bin/env python3
"""Execute the production ioctl_set_steam_controller_attributes path with WDF fakes.

Tests the handle contract and what Steam reads back, not WDK compatibility:
set_steam_controller_attributes runs as written in driver.cpp against a fake
slot that holds the real sc26_state, apply_sc26_attributes is the shipping
steam_controller.cpp, and the replies come from the real sc26_usb responder.
The wire structs are the real protocol.h, compiled on any platform through a
two-file <windows.h>/<winioctl.h> shim that defines only what protocol.h uses.
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

def pin(pattern, what):
    if re.search(pattern, source) is None:
        raise SystemExit('FAIL production source no longer ' + what)
    print('PASS production source ' + what)

pin(r'case lvg::ioctl_set_steam_controller_attributes: \{\s*lvg::steam_controller_attributes_request \*input = nullptr;'
    r'\s*status = retrieve_request\(request, &input\);',
    'dispatches ioctl_set_steam_controller_attributes through retrieve_request (size and version pinned)')
pin(r'status = set_steam_controller_attributes\(context, owner, \*input\)',
    'answers ioctl_set_steam_controller_attributes with set_steam_controller_attributes')
pin(r'slot\.sc26\.reset\(\);\s*slot\.sc26_wire_sequence = 0;',
    'resets the Steam Controller feature state (attributes included) on create_controller')

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
#include "libvirtualgamepad/protocol.h"
#include "steam_controller.h"
using WDFFILEOBJECT=void*; using NTSTATUS=int;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_NOT_FOUND (-2)
#define STATUS_ACCESS_DENIED (-3)
#define STATUS_NOT_SUPPORTED (-4)
#define STATUS_INVALID_DEVICE_REQUEST (-6)
#define NT_SUCCESS(s) ((s)==0)
bool locked=false;
enum class slot_state : std::uint8_t { empty, starting, active, stopping };
bool is_steam_controller(lvg::profile p){ return p==lvg::profile::steam_controller; }
struct controller_slot { WDFFILEOBJECT owner; lvg::profile selected_profile; slot_state state; lvg::driver::sc26_state sc26; };
struct device_context { controller_slot controllers[lvg::k_max_controllers]; };
void lock_context(device_context*){ assert(!locked); locked=true; }
void unlock_context(device_context*){ assert(locked); locked=false; }
'''

suffix = r'''
static unsigned le32(const std::uint8_t* p){ return p[0] | (p[1]<<8) | (p[2]<<16) | (unsigned(p[3])<<24); }
int main(){ int failures=0;
 auto check=[&](bool ok,const char* text){ printf("%s %s\n",ok?"PASS":"FAIL",text); failures+=!ok; };
 // Zeroed WDF memory, then what create_controller does to a Steam Controller slot.
 static device_context c {}; auto& s=c.controllers[2]; s.owner=(void*)1; s.state=slot_state::starting;
 s.selected_profile=lvg::profile::steam_controller; s.sc26.reset(); s.sc26.features.unit_serial[9]='0'; s.sc26.features.unit_serial[10]='2';
 auto& xbox=c.controllers[3]; xbox.owner=(void*)1; xbox.state=slot_state::active; xbox.selected_profile=lvg::profile::xbox_series;
 std::uint8_t cmd[64] {}; std::uint8_t reply[64] {};
 auto ask=[&](std::uint8_t type, std::uint8_t len, std::uint8_t arg){ std::memset(cmd,0,sizeof cmd); cmd[0]=1; cmd[1]=type; cmd[2]=len; cmd[3]=arg;
  return lvg::driver::set_sc26_feature(1,cmd,sizeof cmd,&s.sc26) && lvg::driver::fill_sc26_feature(1,reply,sizeof reply,s.sc26)==64; };
 lvg::steam_controller_attributes_request r {}; r.header.size=sizeof r; r.header.version=lvg::k_protocol_version; r.controller_id=2;
 r.flags=lvg::sc_attr_firmware_build_time|lvg::sc_attr_bootloader_build_time|lvg::sc_attr_board_revision|lvg::sc_attr_capabilities|lvg::sc_attr_build_id|lvg::sc_attr_serials;
 r.firmware_build_time=0x6B111111; r.bootloader_build_time=0x6A222222; r.board_revision=0x4C; r.capabilities=0x8;
 std::memcpy(r.build_id,"26e1f7ef1955",12); std::memcpy(r.unit_serial,"FXA1122334455",13); std::memcpy(r.board_serial,"MXA5544332211",13);
 check(set_steam_controller_attributes(&c,(void*)1,r)==STATUS_SUCCESS && !locked, "a starting Steam Controller slot owned by the caller takes the attributes");
 check(ask(lvg::sc26_usb::cmd_get_attributes_values,0,0) && reply[2]==30 && le32(reply+4)==0x1302 && le32(reply+9)==0x8 && le32(reply+14)==0x6A222222 &&
       le32(reply+19)==0x6B111111 && le32(reply+24)==0x4C && le32(reply+29)==4000,
       "GET_ATTRIBUTES_VALUES answers the real build times, board revision and capabilities around the fixed product id and interval");
 check(ask(lvg::sc26_usb::cmd_get_device_info,1,0) && reply[2]==41 && le32(reply+4)==0x6B111111 && std::memcmp(reply+12,"26e1f7ef1955",12)==0 && reply[24]==0 &&
       std::memcmp(reply+28,"FXA1122334455",13)==0, "GET_DEVICE_INFO 0 answers the real build id and unit serial");
 check(ask(lvg::sc26_usb::cmd_get_string_attribute,0x15,0) && std::memcmp(reply+4,"MXA5544332211",13)==0, "GET_STRING_ATTRIBUTE 0 answers the real board serial");
 // Handle contract: out of range, empty, another handle's, another profile.
 r.controller_id=lvg::k_max_controllers;
 check(set_steam_controller_attributes(&c,(void*)1,r)==STATUS_INVALID_PARAMETER, "an out-of-range slot is STATUS_INVALID_PARAMETER");
 r.controller_id=5;
 check(set_steam_controller_attributes(&c,(void*)1,r)==STATUS_NOT_FOUND, "an empty slot is STATUS_NOT_FOUND");
 r.controller_id=2;
 check(set_steam_controller_attributes(&c,(void*)9,r)==STATUS_ACCESS_DENIED && !locked, "another handle's slot is STATUS_ACCESS_DENIED");
 r.controller_id=3;
 check(set_steam_controller_attributes(&c,(void*)1,r)==STATUS_NOT_SUPPORTED && !locked,
       "an Xbox slot is STATUS_NOT_SUPPORTED, not the STATUS_INVALID_DEVICE_REQUEST an old driver answers");
 // A host that read nothing useful keeps the per-slot serial; the build time still lands.
 r.controller_id=2; r.flags=lvg::sc_attr_serials|lvg::sc_attr_firmware_build_time; r.firmware_build_time=0x6B333333;
 std::memset(r.unit_serial,0,16); std::memcpy(r.board_serial,"MXA\x01",4);
 s.sc26.reset(); s.sc26.features.unit_serial[9]='0'; s.sc26.features.unit_serial[10]='2';
 check(set_steam_controller_attributes(&c,(void*)1,r)==STATUS_SUCCESS && ask(lvg::sc26_usb::cmd_get_string_attribute,0x15,1) &&
       std::memcmp(reply+4,"LVGSC260002",12)==0 && ask(lvg::sc26_usb::cmd_get_string_attribute,0x15,0) && std::memcmp(reply+4,"LVGSC26BOARD0",14)==0,
       "an empty unit serial and a non-printable board serial keep the synthetic per-slot serials");
 check(ask(lvg::sc26_usb::cmd_get_attributes_values,0,0) && le32(reply+19)==0x6B333333 && le32(reply+14)==lvg::sc26_usb::captured_bootloader_build_time,
       "the flagged build time lands while the unflagged bootloader time keeps the captured default");
 // The slot's reset (create_controller) returns to the defaults: attributes are per slot creation.
 s.sc26.reset();
 check(ask(lvg::sc26_usb::cmd_get_attributes_values,0,0) && le32(reply+19)==lvg::sc26_usb::captured_firmware_build_time &&
       ask(lvg::sc26_usb::cmd_get_device_info,1,0) && std::memcmp(reply+12,lvg::sc26_usb::device_info_build_id,12)==0,
       "reset restores the captured build time and the placeholder build id");
 return failures!=0; }
'''

signatures = [
 '[[nodiscard]] bool is_owned_by(\n  const controller_slot &slot,\n  const WDFFILEOBJECT owner) noexcept',
 '[[nodiscard]] NTSTATUS set_steam_controller_attributes(\n  device_context *const context,\n  const WDFFILEOBJECT owner,\n'
 '  const lvg::steam_controller_attributes_request &request) noexcept']
with tempfile.TemporaryDirectory(prefix='sc26-attributes-') as directory:
    work = pathlib.Path(directory)
    (work/'shim').mkdir()
    (work/'shim'/'windows.h').write_text(windows_shim)
    (work/'shim'/'winioctl.h').write_text(winioctl_shim)
    (work/'test.cpp').write_text(prefix + '\n'.join(map(function, signatures)) + suffix)
    sanitize = [] if sys.platform == 'win32' else ['-fsanitize=address,undefined']
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', *sanitize,
                    '-I', str(work/'shim'), '-I', str(ROOT/'include'), '-I', str(ROOT/'driver/src'),
                    str(work/'test.cpp'), str(ROOT/'driver/src/steam_controller.cpp'), '-o', str(work/'test')], check=True)
    raise SystemExit(subprocess.run([str(work/'test')]).returncode)
