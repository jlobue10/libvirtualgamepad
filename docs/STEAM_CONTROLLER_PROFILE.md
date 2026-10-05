# Steam Controller (2026) profile — plan and protocol notes

Goal: a `profile::steam_controller` that Steam recognises as a wired 2026 Steam Controller
(Valve "Ibex"), so a Moonlight client that reports `LI_CTYPE_STEAM` is presented to the host
as the real thing instead of a DualSense. Developed in the `jlobue10/libvirtualgamepad` fork,
tested on the author's host in test-signing mode, then offered upstream as PRs here and in
Vibepollo.

Status: **planning / protocol research**. Nothing in `driver/` implements it yet.

## 1. What has to exist (driver side)

Mirrors the DualSense profile (`driver/src/dualsense.{h,cpp}`, `include/libvirtualgamepad/ds5_usb.h`):

| Piece | Where | Notes |
|---|---|---|
| `profile::steam_controller = 9` | `include/libvirtualgamepad/protocol.h` | next free value; mask bit 0x100; update `k_public_profile_mask` in `driver/tests/test_pid_descriptor.cpp` (0x7C → 0x17C) |
| `include/libvirtualgamepad/sc26_usb.h` | new, portable | report descriptor of the **real wired controller** (from the owner's device dump, see §4), report ids, sizes, feature-report framing, `offsetof` pins |
| `driver/src/steam_controller.{h,cpp}` | new | input encoder, touch/motion/battery folding, feature-report responder, output-report decoder |
| `profile_definition` | `driver/src/profile.cpp` | VID 0x28DE, PID 0x1302 (`USB_DEVICE_ID_STEAM_CONTROLLER_IBEX`), version from the dump, `force_feedback=false`, `hardware_ids=null`; add to `find_profile()` and `k_candidates[]` |
| Slot state | `driver/src/driver.cpp` `controller_slot` | `steam_controller_state sc;` + reset in `create_controller` |
| Feature callbacks | `driver.cpp` ~407 | condition becomes `force_feedback \|\| is_playstation(id) \|\| id == profile::steam_controller` |
| Input submit | `driver.cpp` `submit_input_state` | new branch → `encode_sc26_input()` → `pump_report(..., 0x42, kind)` |
| Touch / motion / battery | `driver.cpp` `begin_state_update`, `submit_touch_state`, `submit_motion_state`, `submit_battery_state`, `submit_profile_report` | new branches; battery also emits report 0x43 |
| GetInputReport | `driver.cpp` `evt_vhf_get_input_report` | new case |
| Output reports | `driver.cpp` `evt_vhf_write_report` | new branch for ids 0x80–0x85 → feedback events |
| Build | `driver/VibeshineVhfGamepad.vcxproj` ClCompile/ClInclude; `driver/tests/CMakeLists.txt` (`test_pid_descriptor` sources, new `test_sc26_usb`) | |
| Tests | `driver/tests/test_sc26_usb.cpp` (descriptor walk, sizes, literal-offset decode, malformed output), `test_pid_descriptor.cpp` (refusal/mask table), `probe_sc26_usb.cpp` (manual, Windows) | per `docs/PROFILE_CONTRACT.md` |
| Docs | `docs/SC26_USB_COMPATIBILITY.md` (what Steam checked, evidence), capability table, provenance | |

Protocol version stays 2 unless the touch request needs a pad index (see §3 — it does not).

## 2. Wire format of the real controller (from permitted public sources)

Sources: Linux `drivers/hid/hid-steam.c` (GPL, Ibex support), SDL `SDL_hidapi_steam_triton.c` +
Valve's `controller_structs.h` / `controller_constants.h` (zlib), SDL `usb_ids.h`, Linux `hid-ids.h`.
Nothing below is taken from Valve firmware or Steam binaries.

### Identity
- VID `0x28DE`. Wired controller PID `0x1302`; BLE `0x1303`; puck dongles `0x1304` (Proteus) / `0x1305` (Nereid).
- The wired controller exposes **one unified HID interface**. Windows splits its top-level
  collections into separate HID devices; Steam opens the vendor collection. Which collections
  exist (vendor + keyboard + mouse for lizard mode?) and their usages are **unknown until the
  descriptor dump** (§4). The puck's pogo-pin interface is `FF00/0002`; the controller collection
  is something else.

### Input report `0x42` (54 bytes including the id) — `TritonMTUFull_t`
| offset | field |
|---|---|
| 0 | report id 0x42 |
| 1 | seq_num (u8) |
| 2..5 | buttons (u32 LE), bits below |
| 6, 8 | trigger L, R (s16, 0..32767) |
| 10,12,14,16 | left stick X,Y; right stick X,Y (s16, ±32767; Y positive = up) |
| 18,20,22 | left pad X, Y (s16 ±32767), pressure (u16) |
| 24,26,28 | right pad X, Y, pressure |
| 30..33 | IMU timestamp (u32) |
| 34,36,38 | accel X, Y, Z (s16) — kernel maps X→X, Y→−Z, Z→Y |
| 40,42,44 | gyro X, Y, Z (s16) |
| 46..53 | gyro quaternion W,X,Y,Z (s16) |

BLE variant `0x45` is the same without the quaternion (46 bytes); `0x47` is a timestamped variant.
The virtual device sends `0x42` only. Scale factors for accel/gyro: take from SDL
(`HIDAPI_DriverSteamTriton_HandleGenericState`, sensor section) during implementation.

Button bits (SDL `TritonButtons`):
`A 0x1, B 0x2, X 0x4, Y 0x8, QAM 0x10, R3 0x20, VIEW 0x40, R4 0x80, R5 0x100, R 0x200,
DPAD_DOWN 0x400, DPAD_RIGHT 0x800, DPAD_LEFT 0x1000, DPAD_UP 0x2000, MENU 0x4000, L3 0x8000,
STEAM 0x10000, L4 0x20000, L5 0x40000, L 0x80000, RSTICK_TOUCH 0x100000, RPAD_TOUCH 0x200000,
RPAD_CLICK 0x400000, RTRIGGER_CLICK 0x800000, LSTICK_TOUCH 0x1000000, LPAD_TOUCH 0x2000000,
LPAD_CLICK 0x4000000, LTRIGGER_CLICK 0x8000000, RGRIP_TOUCH 0x10000000, LGRIP_TOUCH 0x20000000`.

Mapping from `lvg::button_mask`: south→A, east→B, west→X, north→Y, dpad→dpad, start→MENU,
back→VIEW, home→STEAM, misc→QAM, LB/RB→L/R, LS/RS→L3/R3, paddle_1..4→L4,R4,L5,R5 (order to be
confirmed against the Moonlight client), touchpad button→LPAD_CLICK+RPAD_CLICK? (decide; the
Moonlight client already sends pad clicks as separate buttons). Trigger full-pull bits set when
trigger ≥ ~0xF0. Pad touch bits follow the touch state.

### Battery report `0x43` (15 bytes)
`charge_state u8 (1 discharging, 2 charging, 4 done), level u8 (0..100), battery_voltage u16,
system_voltage u16, input_voltage u16, current u16, input_current u16, temperature u16`.

### Feature reports — the control channel (report id 1, 64 bytes + id)
Host writes a command with SetFeature, then reads the reply with GetFeature. Framing:
`[type u8][length u8][payload...]`. Commands seen in the kernel and SDL:
`0x81 CLEAR_DIGITAL_MAPPINGS, 0x83 GET_ATTRIBUTES_VALUES, 0x85 SET_DEFAULT_DIGITAL_MAPPINGS,
0x87 SET_SETTINGS_VALUES (triples id u8, value u16), 0x89 GET_SETTINGS_VALUES, 0x8B GET_SETTINGS_MAXS,
0x8C GET_SETTINGS_DEFAULTS, 0x8E LOAD_DEFAULT_SETTINGS, 0xAE GET_STRING_ATTRIBUTE (0x15=unit serial)`.
Attributes (`tag u8, value u32`): `UNIQUE_ID 0, PRODUCT_ID 1, CAPABILITIES 2, FIRMWARE_VERSION 3,
FIRMWARE_BUILD_TIME 4, RADIO_FIRMWARE_BUILD_TIME 5, RADIO_DEVICE_ID0/1 6/7, DONGLE_FIRMWARE_BUILD_TIME 8,
BOARD_REVISION 9, BOOTLOADER_BUILD_TIME 10, CONNECTION_INTERVAL_IN_US 11, ...`.
The responder keeps the last command and answers the next GetFeature with its reply, like the
firmware. Values (build times, board revision, serial) are copied from the owner's controller
(§4) so Steam does not offer a firmware update. **What Steam actually sends on connect is the
main unknown and needs the USB capture in §4.** Unknown commands are acknowledged and surfaced
to the host as `raw_hid_report_feedback` so the capture gaps can be closed iteratively.

### Output reports — haptics (10 bytes incl. id)
`0x80 HAPTIC_RUMBLE {type u8, intensity u16, left{speed u16, gain s8}, right{speed u16, gain s8}}`,
`0x81 HAPTIC_PULSE {side u8, on_us u16, off_us u16, repeat u16}`, `0x82 HAPTIC_CMD`, `0x83 LFO`,
`0x84 LOG_SWEEP`, `0x85 SCRIPT`. Steam re-sends 0x80 every ≤50 ms while rumbling (firmware safety
timeout). v1 maps 0x80 → `generic_rumble` (speed L/R → low/high) and 0x81 → a short
`generic_rumble` burst; the Moonlight client turns rumble back into pad pulse trains. A dedicated
`feedback_type::steam_haptic` can follow once the basics work.

## 3. Vibepollo side (fork `jlobue10/Vibepollo`, branch `fork/2.0.0`)
- `vhf_profile_e::steam_controller`; `vhf_gamepad::offers(client, lvg::profile::steam_controller)`.
- `vhf_desired_profile()`: `metadata.type == LI_CTYPE_STEAM` → steam_controller (guard with
  `#ifndef LI_CTYPE_STEAM #define LI_CTYPE_STEAM 0x04` because the pinned common-c lacks it).
  New config value `vhf_steam` for an explicit choice.
- Touch: `SS_CONTROLLER_TOUCH_PACKET.touchpadIndex` is currently dropped by `passthrough()`;
  carry it into `platf::gamepad_touch_t` and, for this profile, send `contact_index = touchpadIndex`
  (0 = left pad, 1 = right pad; each pad is single-touch so no protocol change is needed).
- `has_motion()` includes the new profile; battery already generic.
- Submodule `third-party/libvirtualgamepad` → the fork commit; `ci-windows.yml` pins
  (`VHF_TAG`, `VHF_ARCHIVE_SHA256`, `VHF_SOURCE_REVISION`, `VHF_DRIVER_VER`) → the fork's release.

## 4. Owner-side research tasks (blocking the first build)
1. **Report descriptor + USB descriptors of the wired controller.** Easiest on Linux
   (plug the controller into the CachyOS box via USB-C):
   `lsusb -v -d 28de:` and `for d in /sys/bus/usb/devices/*; do grep -qs 28de $d/idVendor && find $d -name report_descriptor -exec sh -c 'echo {}; xxd -p {}' \;; done`.
   On Windows: a USBPcap/Wireshark capture of plugging the controller in contains the
   `GET DESCRIPTOR Response HID Report`.
2. **Steam ↔ controller handshake capture** on the Windows host: USBPcap + Wireshark, filter
   `usb.idVendor == 0x28de`, start capture, plug in the controller, open Steam → Settings →
   Controller, then Big Picture, press buttons, let it rumble, unplug. Save as `.pcapng`.
   This yields every SetFeature/GetFeature pair (the commands and the *replies*: attributes,
   serial, settings tables), the input report ids actually used, and the haptic output reports.
3. **Test-signing on the host**: `bcdedit /set testsigning on` needs **Secure Boot off** and some
   anti-cheat systems refuse to run with test signing active. Alternative dev rig: a Windows VM
   (Secure Boot off) with Steam + Vibepollo + the test driver, streamed to the headset to verify
   detection, glyphs, trackpads and haptics before anything touches the gaming PC.

## 5. Build / install plan for the fork
- Driver: fork CI `release-windows.yml` builds an unsigned package on tag `v0.1.0-beta.N`; add a
  `test-signed-package` workflow (LocalTest signing in the runner, ship the `.cer`) so the host
  needs no Visual Studio/WDK. Install: `trust-test-certificate.ps1` then
  `VibeshineVhfGamepadDeviceSetup.exe install --inf ...` (elevated; exit 3010 = reboot).
- Vibepollo: fork `ci-windows.yml` with the pins pointed at the fork's driver release; the
  resulting installer replaces 2.0.0 on the host (pairing state survives).

## 6. Risks
Steam firmware-update prompts or refusals (mitigated by echoing the real controller's build
times), Steam updates changing the handshake, no documentation of Steam's checks (only the
capture), test signing vs. Secure Boot/anti-cheat, and upstream acceptance (profile contract §35–48).
