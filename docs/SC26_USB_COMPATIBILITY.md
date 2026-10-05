# Steam Controller (2026) USB compatibility

The Windows VHF profile uses the portable `sc26_usb.h` contract. Its descriptor,
identity, attribute values and the shape of every control-channel reply come
from the author's own wired unit, captured with USBPcap on 2026-10-05
(`captures/`, see `docs/STEAM_CONTROLLER_PROFILE.md` §7). The protocol semantics
(report layouts, button bits, command and setting ids, scaling) come from the
public Linux `hid-steam` driver and SDL's Triton HIDAPI driver. Nothing is taken
from Valve firmware or Steam binaries.

Steam opens the controller through HIDAPI, so what it sees is the vendor
collection of the HID device: the report descriptor, the VID/PID/bcdDevice, the
feature-report control channel and the interrupt reports. This document records
what the real unit presents and what Steam did with it, which is what the
virtual device reproduces.

## Identity and enumeration (real unit)

| Item | Value | Evidence |
| --- | --- | --- |
| USB identity | VID 28DE, PID 1302, bcdDevice 0307 | `captures/sc26-plugin-summary.txt` |
| Interfaces | one HID interface, class 03 / 00 / 00 | same |
| Report descriptor | 372 bytes, three top-level collections | `captures/sc26-report-descriptor-1.hex` |
| Collection 1 | Generic Desktop / Mouse, report 0x40 (6 bytes) | descriptor |
| Collection 2 | Generic Desktop / Keyboard, report 0x41 (9 bytes) | descriptor |
| Collection 3 | Vendor page FF00 usage 1: inputs 0x42 (54), 0x43 (15), 0x44 (6), 0x45 (46), 0x79 (2), 0x7B (13); outputs 0x80 (10), 0x81 (8), 0x82 (4), 0x83 (10), 0x84 (9), 0x85 (4), 0x86 (4), 0x87/0x88/0x89 (64); features 0x01 and 0x02 (64) | descriptor |
| Windows HID children | `HID\VID_28DE&PID_1302&REV_0307&Col01` mouse (mouhid), `Col02` keyboard (kbdhid), `Col03` vendor FF00/0001 (no service) | `captures/steam-controller-hid.txt` |

The virtual device compiles in that descriptor byte for byte, reports the same
VID/PID/bcdDevice and declares `HID\VID_28DE&PID_1302` as its PnP hardware ID,
so Windows produces the same three collections. Steam's HIDAPI enumeration
matches on the vendor collection (usage page 0xFF00).

## What Steam does on connect (real unit, 2 minutes with Steam open)

`captures/sc26-steam-handshake.txt` lists every transfer. Summary:

| Step | Command (feature report 1, `[type][len][payload]`) | Reply Steam reads |
| --- | --- | --- |
| 1 | `83 00` GET_ATTRIBUTES_VALUES | 30 bytes: six `(tag, u32)` pairs in this order: product id 1 = 0x1302, capabilities 2 = 0, bootloader build time 10 = 0x68D2F92E, firmware build time 4 = 0x6A4D85E3, board revision 9 = 0x4A, connection interval 11 = 4000 |
| 2 | `AE 15 01` GET_STRING_ATTRIBUTE, tag 1 | 20 bytes: `[01]` + unit serial (13 chars, NUL padded) |
| 3 | `87 03 32 84 03`, `87 03 09 00 00` SET_SETTINGS_VALUES | none (write only) |
| 4 | `AE 15 00` GET_STRING_ATTRIBUTE, tag 0 | 20 bytes: `[00]` + board serial |
| 5 | `81 00` CLEAR_DIGITAL_MAPPINGS, then `87` with settings 48 = 0x18 (IMU accel+gyro), 7 = 7, 8 = 7, 49 = 2, 82 = 3, 24 = 0, 46 = 0, 52 = 0xFFFF, 53 = 0xFFFF | none |
| 6 | `F2 01 00`, `F2 01 01`, `F2 01 02` (device info, sub id 0..2) | 41, 34 and 9 bytes; sub id 0 repeats firmware build time, board revision, a 12-character build id and the unit serial |
| 7 | `87 03 2D 64 00` (setting 45 = 100) | none |
| 8 | on entering the controller UI: `C1 10 ff ff ff ff 03 09 05 ff..`, `DC 02 01 02`, `E2 02 01 20`, settings 34 = 100, 35 = 80, 84 = 0, 85 = 0 | none |
| 9 | `ED <len> "esb/bond"`, `"esb/bond_2"`, `"user/wireless_transport"` | 1 byte (`00`), 24 bytes, 0 bytes |
| 10 | periodically: `83 00` and `AE 15 01` again; on exit `85 00` SET_DEFAULT_DIGITAL_MAPPINGS and `8E 00` LOAD_DEFAULT_SETTINGS | as above |

Totals: 165 SET_REPORT and 25 GET_REPORT feature transfers; no other control
transfers once enumerated. Steam did not offer a firmware update for this unit
(firmware build 0x6A4D85E3), which is why the virtual device returns that build
time.

Interrupt traffic in the same capture:

| Report | Direction | Count | Notes |
| --- | --- | --- | --- |
| 0x42 state | in | 29 953 | 54 bytes, ~250 Hz, sequence byte increments by 1; buttons u32 at offset 2 used all 30 bits; triggers 0..32767 at 6/8; sticks at 10..16; pads at 18..28; microsecond timestamp at 30 (3.8 ms steps); accel at 34/36/38 with 1 g on Z at rest; gyro at 40/42/44; quaternion at 46 constant identity (32767, 0, 0, 0) |
| 0x43 battery | in | 34 | every ~3.5 s: `04 64` (done, 100 %), 4122 mV cell, 4160 mV system, 4980 mV input, 157 mA, 239 mA, temperature 0x76ED |
| 0x44 | in | 42 | 6 bytes, one after every haptic output report (`04 02 00..` / `03 02 00..`) |
| 0x40 / 0x41 | in | 3 each | lizard-mode mouse and keyboard, all zero |
| 0x81 pulse | out | 96 | 8 bytes. Steam's UI click is `81 <side> 90 01 00 00 01 00` (400 us on, repeat 1) followed by all-zero stop reports; pairs 0.7 ms apart, bursts every 11 to 20 ms |
| 0x82 command | out | 66 | 4 bytes: `82 <side> 02 F2` or `82 <side> 01 FD` |
| 0x80 rumble | out | 0 | not used by Steam's UI; games may |

## What the virtual device does

| Operation | Behaviour | Where |
| --- | --- | --- |
| Descriptor, identity | the captured 372-byte descriptor, VID 28DE PID 1302 bcdDevice 0307 | `sc26_usb.h`, `profile.cpp` |
| GET_ATTRIBUTES_VALUES | the six captured tags in the captured order; build times and board revision default to the captured unit's | `sc26_usb::set_feature` |
| GET_STRING_ATTRIBUTE | tag 0 board serial, tag 1 unit serial, fixed 20-byte reply; serials are synthetic (`LVGSC26…`), never a real unit's | same |
| SET_SETTINGS_VALUES and the GET_SETTINGS family | stored and read back as `(id, u16)` triples; lizard mode and IMU mode tracked | same |
| GET_DEVICE_INFO 0xF2 | 41 / 34 / 9-byte replies shaped like the unit's, carrying the state's build time, board revision and serial | same |
| 0xED keyed values | `esb/bond` answers `00` (no bonded puck); other keys answer empty | same |
| 0xC1, 0xDC, 0xE2 | acknowledged, no reply payload, not counted as unknown | same |
| Unknown commands | acknowledged with an empty reply and counted (`feature_state.unknown_commands`) | same |
| Output 0x80 rumble, 0x81 pulse | decoded into a `generic_rumble` feedback event (pulse duty cycle becomes a magnitude) | `apply_sc26_output` |
| Output 0x82..0x89 | accepted (STATUS_SUCCESS), nothing rendered | `driver.cpp` write path |
| Input 0x42 | 54 bytes at hid-steam's Ibex offsets, sequence byte, identity quaternion | `sc26_usb::encode_input` |
| Input 0x43 | battery figures shaped like the unit's | `sc26_usb::encode_battery` |
| Input 0x40, 0x41, 0x44, 0x45, 0x79, 0x7B | never sent (lizard mode off, no BLE, no haptic acks) | n/a |

## Capability table

| Capability | Virtual device | Notes |
| --- | --- | --- |
| Buttons | all 30 bits of `TritonButtons` | A/B/X/Y, D-pad, Menu/View/Steam/QAM, L/R, L3/R3, L4/L5/R4/R5, pad clicks, trigger clicks, stick/pad/grip touch |
| Sticks | two, s16 ±32767, positive up | deflection beyond ~10 % sets the stick-touch bit when the client has no capacitive touch |
| Triggers | two, 0..32767 | click bits at ≥ 0xF0 of the client's 0..255 |
| Touch pads | two, single contact each, x/y ±32767, pressure 0..32767 | protocol contact index 0 = left, 1 = right |
| Motion | accelerometer and gyroscope at SDL scaling, device axes | quaternion fixed at identity, as on the real unit |
| Battery | report 0x43 with level and charge state | emitted on battery updates |
| LEDs | none | the controller has no host-controlled LED |
| Rumble | 0x80 rumble and 0x81 pulse → `generic_rumble` | 0x82..0x89 accepted, not rendered |
| Trigger rumble | none | not a feature of the device |
| Feature reports | report 1 control channel as above; report 2 declared, never used by Steam | the driver refuses Get/SetFeature on report 2 |

## Privacy and provenance

- The descriptor is the author's own unit's and contains no per-unit data.
- The attribute values the virtual device returns (firmware/bootloader build
  times, board revision) identify a firmware build, not a unit. Steam saw this
  build without asking for an update.
- Per-unit values seen in the capture (the unit serial, the board serial, the
  12-character build id, the bonded-device record) are not copied into the
  driver. The virtual device returns synthetic serials and an all-zero build id.
- The captures themselves, committed under `captures/`, contain the author's
  unit serial and board serial. They are the evidence for this document and are
  committed knowingly.
- No Valve firmware or Steam binary was read; the behaviour was observed on the
  wire.

## Evidence and regression checks

- `driver/tests/test_sc26_usb.cpp`: walks the compiled-in descriptor and pins
  every report size to `report_size()`, checks the three collections, pins the
  0x42 offsets, replays the captured Steam command sequence and asserts every
  reply length matches the real unit's, and checks the haptic decode including
  the captured click pulse. `test_pid_descriptor.cpp` checks the profile is
  now served and the advertised mask is 0x17C; `test_profile_identity.cpp`
  checks the PnP ID agrees with the HID VID/PID.
- Real Windows HID enumeration evidence for the real unit:
  `captures/steam-controller-hid.txt`.
- Virtual device on the test rig (Hyper-V Windows 11 Pro 26300 VM, test signing
  on, test-signed package from commit e6611a2, 2026-10-05): `probe_sc26_usb`
  passed every check. Windows produced the same three HID children as the real
  unit, `HID\VID_28DE&PID_1302&COL01..03` with version 0x0307: Mouse (input 6),
  Keyboard (input 9) and the vendor collection FF00/0001 with input 54, output
  64, feature 64 report lengths, opened read/write. GET_ATTRIBUTES_VALUES
  returned `1=0x1302 2=0 10=0x68D2F92E 4=0x6A4D85E3 9=0x4A 11=0xFA0`,
  GET_STRING_ATTRIBUTE tag 1 returned `LVGSC260007` (slot 7) in the 20-byte
  frame, GET_DEVICE_INFO 0 returned 41 bytes, the 0x42 report arrived at 54
  bytes with the submitted buttons, stick and trigger, the 0x81 pulse became a
  `generic_rumble` event with left = 65535, and 0x82 was accepted. The first
  rig run exposed and fixed a real defect: `feature_state::reset()` preserved
  the zeroed WDF memory, so attributes and serial were all zero (commit
  e6611a2). One known difference: VHF has no way to set the HID serial-number
  string, so `HidD_GetSerialNumberString` returns "1.0" where the real unit
  returns its USB serial; Steam reads the serial through the feature report.
- Steam's reaction on the rig (Steam client 1788652215, 2026-10-05, before any
  account login; `captures/rig/steam-controller-logs.txt`): `controller.txt`
  logs "Local Device Found type: 28de 1302" on the Col03 path, "Controller uses
  V1 HID protocol via USB", "Steam controller device opened for index 0",
  "Steam Controller reserving XInput slot 0", "Controller Info: HWID: 74,
  FWTimestamp: 0x6A4D85E3" and the serial `LVGSC260007` read through the
  feature report; `controller_ui.txt` reports Type 10, ProductID 4866,
  Capabilities 00000000416dbfff, Firmware Version 1783465443 and loads
  `controller_base/basicui_neptune.vdf` for it. Steam created its "Steam
  Virtual Gamepad" binding (`config.vdf`) and began sending 0x81 haptic pulse
  pairs to the device every ~3 s, which the driver turned into `generic_rumble`
  events (142 in four minutes, `probe --hold`). No firmware-update prompt
  appeared in the logs. Steam logs the HID strings as "Manufacturer: Microsoft,
  Product: HID VHF Driver, serial 1.0", which are VHF's and not configurable;
  it identified the controller regardless. Settings → Controller with a
  logged-in account was not exercised in the VM; it moves to the physical test
  host (`tools/test-rig/`), together with the Vibepollo stream.
- Virtual device enumeration on the rig: `captures/rig/virtual-device-hid-children.txt`
  (Windows lists the three collections plus VHF's parent node); probe output:
  `captures/rig/probe_sc26_usb-output.txt`.
