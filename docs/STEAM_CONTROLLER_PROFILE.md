# Steam Controller (2026) profile — plan and protocol notes

Goal: a `profile::steam_controller` that Steam recognises as a wired 2026 Steam Controller
(Valve "Ibex"), so a Moonlight client that reports `LI_CTYPE_STEAM` is presented to the host
as the real thing instead of a DualSense. Developed in the `jlobue10/libvirtualgamepad` fork,
tested on the author's host in test-signing mode, then offered upstream as PRs here and in
Vibepollo.

Status (2026-10-05, evening): **profile enabled.** The real wired unit was captured (§7.2/§7.3,
`captures/`), and `include/libvirtualgamepad/sc26_usb.h` now carries its 372-byte report descriptor,
bcdDevice 0x0307, the attribute values Steam read from it, the corrected string-attribute tags and
replies for every command Steam sent (`report_descriptor_is_provisional = false`, public mask 0x17C).
`driver/tests/test_sc26_usb.cpp` replays the captured Steam handshake against the responder. What the
unit presents and what Steam did with it is written up in `docs/SC26_USB_COMPATIBILITY.md`. Vibepollo
mapping: fork branch `jlobue10/Vibepollo` `feat/steam-controller-profile` (`vhf_steam`,
`LI_CTYPE_STEAM` → profile, touchpad index carried to the driver, docs/web UI), submodule pointed at
this branch.

**2026-10-07, first stream from the headset (fork installer from run 37490202610, `gamepad =
vhf_steam`):** Steam listed the virtual device as the user's Steam Controller and the stream worked.
Three defects were found in Steam's controller test and are fixed on this branch (see §8): both pads
landed on the left pad (left pad = its left half, right pad = its right half; the right pad never
registered a touch or a click), and the View and Menu buttons were swapped.

**Next (§7.10 has the exact state):** sideload client fork.26 (full-scale sticks), run Steam's
controller test over the stream (the left-stick circle step is the last open item; grips and stick
touch already light), then tag beta.107, bump the Vibepollo pins, merge the Vibepollo PRs,
`Collect-Evidence.ps1`, and the upstream PRs (driver first, then Vibepollo).

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
| Output reports | `driver.cpp` `evt_vhf_write_report` | new branch for ids 0x80–0x89 (`sc26_usb::is_output_report`) → feedback events for 0x80/0x81, accepted otherwise |
| Build | `driver/VibeshineVhfGamepad.vcxproj` ClCompile/ClInclude; `driver/tests/CMakeLists.txt` (`test_pid_descriptor` sources, new `test_sc26_usb`) | |
| Tests | `driver/tests/test_sc26_usb.cpp` (descriptor walk, sizes, literal-offset decode, malformed output), `test_pid_descriptor.cpp` (refusal/mask table), `probe_sc26_usb.cpp` (manual, Windows) | per `docs/PROFILE_CONTRACT.md` |
| Docs | `docs/SC26_USB_COMPATIBILITY.md` (what Steam checked, evidence), capability table, provenance | written 2026-10-05 from the captures |

Protocol version stays 2 unless the touch request needs a pad index (see §3 — it does not).

## 2. Wire format of the real controller (from permitted public sources)

Sources: Linux `drivers/hid/hid-steam.c` (GPL, Ibex support), SDL `SDL_hidapi_steam_triton.c` +
Valve's `controller_structs.h` / `controller_constants.h` (zlib), SDL `usb_ids.h`, Linux `hid-ids.h`.
Nothing below is taken from Valve firmware or Steam binaries.

### Identity
- VID `0x28DE`. Wired controller PID `0x1302`; BLE `0x1303`; puck dongles `0x1304` (Proteus) / `0x1305` (Nereid).
- The wired controller exposes **one unified HID interface** (class 03/00/00) whose 372-byte
  report descriptor has three top-level collections (captured 2026-10-05,
  `captures/sc26-report-descriptor-1.hex`): Generic Desktop **Mouse** (report 0x40, 6 bytes),
  Generic Desktop **Keyboard** (0x41, 9 bytes) for lizard mode, and the **vendor collection**
  `FF00/0001` Steam opens, which declares inputs 0x42 (54), 0x43 (15), 0x44 (6), 0x45 (46),
  0x79 (2), 0x7B (13), outputs 0x80 (10), 0x81 (8), 0x82 (4), 0x83 (10), 0x84 (9), 0x85 (4),
  0x86 (4), 0x87/0x88/0x89 (64) and features 0x01 and 0x02 (64). Windows binds them as three HID
  children (`Col01` mouhid, `Col02` kbdhid, `Col03` vendor). bcdDevice is 0x0307. The puck's
  pogo-pin interface is `FF00/0002`.

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

Confirmed on the wire (29 953 reports at ~250 Hz): the sequence byte steps by one, the
timestamp at 30 steps ~3.8 ms, accel Z at 38 reads ~16 384 at rest, and the quaternion at 46 is a
constant identity (32767, 0, 0, 0) on firmware 0x6A4D85E3, so the virtual device's identity
quaternion matches. BLE variant `0x45` is the same without the quaternion (46 bytes); `0x47` is a
timestamped variant.
The virtual device sends `0x42` only. Scale factors for accel/gyro: take from SDL
(`HIDAPI_DriverSteamTriton_HandleGenericState`, sensor section) during implementation.

Button bits (SDL `TritonButtons`):
`A 0x1, B 0x2, X 0x4, Y 0x8, QAM 0x10, R3 0x20, VIEW 0x40, R4 0x80, R5 0x100, R 0x200,
DPAD_DOWN 0x400, DPAD_RIGHT 0x800, DPAD_LEFT 0x1000, DPAD_UP 0x2000, MENU 0x4000, L3 0x8000,
STEAM 0x10000, L4 0x20000, L5 0x40000, L 0x80000, RSTICK_TOUCH 0x100000, RPAD_TOUCH 0x200000,
RPAD_CLICK 0x400000, RTRIGGER_CLICK 0x800000, LSTICK_TOUCH 0x1000000, LPAD_TOUCH 0x2000000,
LPAD_CLICK 0x4000000, LTRIGGER_CLICK 0x8000000, RGRIP_TOUCH 0x10000000, LGRIP_TOUCH 0x20000000`.

Mapping from `lvg::button_mask`: south→A, east→B, west→X, north→Y, dpad→dpad,
**start→VIEW (0x40), back→MENU (0x4000)**, home→STEAM, misc→QAM, LB/RB→L/R, LS/RS→L3/R3,
paddle_1..4→R4, L4, R5, L5 (the Moonlight client's order), touchpad button→the click bit of the
pad(s) currently touched (left only → LPAD_CLICK, right only → RPAD_CLICK, both → both, none →
RPAD_CLICK). Trigger full-pull bits set when trigger ≥ ~0xF0. Pad touch bits follow the touch
state. The start/back pair is deliberately the opposite of Valve's constant names: SDL's Triton
driver maps `TRITON_LBUTTON_VIEW` (0x40) to `SDL_GAMEPAD_BUTTON_START` and `TRITON_LBUTTON_MENU`
(0x4000) to `SDL_GAMEPAD_BUTTON_BACK`, the Moonlight client reading the real controller does the
same, and mapping by name made Steam show the two buttons swapped (first stream, 2026-10-07).

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
firmware. Build times and board revision are the author's unit's (firmware 0x6A4D85E3, bootloader
0x68D2F92E, board revision 0x4A), which Steam accepted without offering an update; serials are
synthetic. **What Steam sends on connect is now known** (`captures/sc26-steam-handshake.txt`,
summarised in `docs/SC26_USB_COMPATIBILITY.md`): GET_ATTRIBUTES (six tags, fixed order),
GET_STRING_ATTRIBUTE with tag 1 (unit serial) and tag 0 (board serial; the request is
`[0xAE][0x15 = length][tag]`, so the old "0x15 = unit serial" reading was wrong), CLEAR_DIGITAL_MAPPINGS,
SET_SETTINGS_VALUES for ids 7, 8, 9, 24, 34, 35, 45, 46, 48, 49, 50, 52, 53, 82, 84, 85, then
three commands absent from the public sources that Steam only writes (`0xC1` 16 bytes, `0xDC` `01 02`,
`0xE2` `01 20`), `0xF2` device info (sub ids 0..2, replies 41/34/9 bytes) and `0xED` keyed queries
(`esb/bond`, `esb/bond_2`, `user/wireless_transport`); on exit SET_DEFAULT_DIGITAL_MAPPINGS and
LOAD_DEFAULT_SETTINGS. The responder answers all of them in the captured shapes. Unknown commands are
still acknowledged and counted.

### Output reports — haptics (10 bytes incl. id)
`0x80 HAPTIC_RUMBLE {type u8, intensity u16, left{speed u16, gain s8}, right{speed u16, gain s8}}`,
`0x81 HAPTIC_PULSE {side u8, on_us u16, off_us u16, repeat u16}`, `0x82 HAPTIC_CMD`, `0x83 LFO`,
`0x84 LOG_SWEEP`, `0x85 SCRIPT`. The real descriptor sizes them 10 / 8 / 4 / 10 / 9 / 4 bytes
(plus 0x86: 4 and 0x87–0x89: 64). Steam re-sends 0x80 every ≤50 ms while rumbling (firmware safety
timeout). In the capture Steam's UI used only 0x81 (`81 <side> 90 01 00 00 01 00`: 400 µs on,
repeat 1, then an all-zero stop) and 0x82 (`82 <side> 02 F2` / `01 FD`); the unit answered each with
an input report 0x44. v1 maps 0x80 → `generic_rumble` (speed L/R → low/high) and 0x81 → a
`generic_rumble` magnitude from the duty cycle; 0x82–0x89 are accepted and dropped. The Moonlight
client turns rumble back into pad pulse trains. A dedicated `feedback_type::steam_haptic` can follow
once the basics work.

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

## 7. Continuing on Windows (handoff for the capture and test phase)

This section is self-contained on purpose: the Linux session's memory does not travel.

### 7.1 Machine setup
- Install Git, GitHub CLI (`winget install Git.Git GitHub.cli`), Wireshark **with USBPcap**
  (tick USBPcap in the Wireshark installer; reboot), and Claude Code. `gh auth login` as
  `jlobue10`.
- Clone the two forks side by side (Vibepollo expects `../libvirtualgamepad` or the submodule):
  ```powershell
  git clone -b feat/steam-controller-profile https://github.com/jlobue10/libvirtualgamepad
  git clone -b feat/steam-controller-profile --recurse-submodules https://github.com/jlobue10/Vibepollo
  cd libvirtualgamepad; git remote add upstream https://github.com/Nonary/libvirtualgamepad; gh repo set-default jlobue10/libvirtualgamepad
  ```
- Read this file top to bottom, then `git log --oneline -8` in both forks. Current state is §"Status"
  and §7.7 (what the first Windows session already did).
- Silent installs work: `winget install --source winget WiresharkFoundation.Wireshark` and
  `winget install --source winget desowin.USBPcap`. The silent Wireshark install skips Npcap; that
  is fine, the capture script below drives `USBPcapCMD.exe` directly and only needs `tshark.exe`
  to read the files. **Reboot after installing USBPcap**: the class upper filter is registered at
  install time but attaches to the root hubs only at boot, so `\\.\USBPcap1..N` do not exist
  until then (the capture script checks and says so).
- The Moonlight client side (Galaxy XR fork) lives in `jlobue10/moonlight-android`,
  `docs/HANDOFF.md`; it already reports `LI_CTYPE_STEAM` with two touchpads, motion and battery.

### 7.2 Capture 1: the controller's descriptors (plug-in capture)
Preferred: elevated PowerShell in the repo root, controller unplugged, then
`.\tools\capture\Capture-SteamController.ps1 -Phase Plugin` and plug the controller in when told.
It captures every root hub, keeps the one that saw the Valve device descriptor, and writes all the
files listed in steps 4–6 plus `captures/sc26-plugin-summary.txt` (idProduct, bcdDevice,
interface classes, descriptor lengths). Its descriptor extraction has not yet run against a real
capture (no controller was available in the session that wrote it); if a
`sc26-report-descriptor-<n>.hex` is missing or looks wrong, fall back to the manual steps below
on `captures/sc26-plugin.pcapng`, which it saves in any case.

Manual path:
1. Wireshark → Capture → pick the **USBPcap** interface the controller will land on (if unsure,
   start one capture per USBPcap root hub; the right one shows traffic when you plug in).
2. Start capturing, **then** plug the controller in over USB-C. Wait 10 s. Stop.
3. Filter `usb.idVendor == 0x28de`. Note `idProduct` (expect 0x1302) and `bcdDevice` in the
   GET DESCRIPTOR Response DEVICE; note interface count and each interface's class/subclass.
4. Find every `GET DESCRIPTOR Response HID Report` frame. For each: expand the HID Report
   Descriptor, right-click → Copy → ...as Hex Stream. Save them as
   `captures/sc26-report-descriptor-<n>.hex` (one per interface/collection; the one with a
   vendor usage page and feature report items is the controller interface).
5. Also run `tools/capture/Get-SteamControllerHid.ps1 | Tee-Object captures/steam-controller-hid.txt`
   (lists Windows' HID children with usage page/usage and `REV_xxxx` = bcdDevice).
6. File → Save As `captures/sc26-plugin.pcapng`. Then File → Export Packet Dissections → As JSON
   → `captures/sc26-plugin.json` (the JSON is what the next session parses; keep both).

### 7.3 Capture 2: Steam's handshake and traffic
Preferred: controller plugged in, Steam quit, then
`.\tools\capture\Capture-SteamController.ps1 -Phase Steam` (elevated). It reuses the hub from
phase 1, injects the already-connected device's descriptors so the controller can be filtered,
waits for Enter after you have exercised the controller and unplugged it, and writes the pcapng,
the JSON (zipped if it would exceed 90 MB), `captures/sc26-steam-control.tsv` (every
SET_REPORT/GET_REPORT with wValue, wIndex and payload), `captures/sc26-steam-interrupt.tsv`
(time, endpoint, payload of every interrupt transfer) and `captures/sc26-steam-summary.txt`
(counts per bRequest and per endpoint/report id), and `tools/capture/Decode-Sc26Handshake.py`
turns the control TSV into the ordered command/reply list (`captures/sc26-steam-handshake.txt`).
The TSVs are the easy parse target; the JSON is the complete one. Both phases also take
`-FromPcap <raw.pcap>` to re-run the post-processing on a raw hub capture a failed run left in
`%TEMP%\sc26-capture-*` (no elevation needed). Note for readers of the TSVs: Wireshark exposes
HID interrupt payloads as `usbhid.data` and HID class setup fields as `usbhid.setup.*`; the data of
a GET_REPORT response is exposed by no field at all, so the script reads it from the raw frame.

Manual path:
1. Quit Steam completely. Start a new USBPcap capture on the same interface, controller plugged in.
2. Start Steam → Settings → Controller (let it detect/identify the controller; if it offers a
   firmware update, **decline** for now and note it). Open Big Picture, navigate, press every
   button, touch both pads, move both sticks, pull triggers, click grips; trigger rumble (a
   controller test or any haptic feedback). Then disconnect the controller. Stop the capture.
3. Save `captures/sc26-steam.pcapng` and export JSON `captures/sc26-steam.json`.
4. Useful display filters: `usb.idVendor == 0x28de && usb.transfer_type == 0x02` shows the
   control transfers; `usb.setup.bRequest == 0x09` are SET_REPORT (commands Steam writes),
   `usb.setup.bRequest == 0x01` are GET_REPORT (replies Steam reads). Interrupt transfers
   (`usb.transfer_type == 0x01`) carry the 0x42/0x43 input reports and Steam's 0x80–0x85 haptic
   output reports.
5. What to extract (next session does this from the JSON): the ordered list of feature-report
   commands Steam sends and the controller's replies (attribute values, serial format, settings
   ids/values Steam writes, anything not in `sc26_usb::command`), the input report ids seen, the
   exact haptic output reports (rumble resend interval, pulse parameters).

Commit the `captures/` folder to the fork branch (`git add captures; git commit`) so it travels.

### 7.4 Enabling the profile from the captures
Steps 1 and 2 were done on 2026-10-05 (see the status at the top and `docs/SC26_USB_COMPATIBILITY.md`);
step 3 is next.
1. `include/libvirtualgamepad/sc26_usb.h`: replace `report_descriptor[]` with the controller
   interface's hex (comment: source = own hardware dump, date, firmware), set
   `report_descriptor_is_provisional = false`, set `version` = bcdDevice, fill `attributes`
   defaults (firmware/bootloader build times, board revision, capabilities) and the serial
   format from the replies, add any missing command ids to `set_feature`.
2. If the real descriptor's report sizes differ from the structs, fix the structs/sizes, the
   `offsetof` pins and `driver/tests/test_sc26_usb.cpp` together. Run the tests:
   `cmake -S driver/tests -B build/tests && cmake --build build/tests --config Release && ctest --test-dir build/tests -C Release`.
3. Push; `test-driver.yml` runs on push. Then `gh workflow run test-signed-package.yml --ref feat/steam-controller-profile`
   and download the artifact: `gh run download <run-id> -D artifacts/`.

### 7.5 Test rig
- Test signing: elevated `bcdedit /set testsigning on` + reboot. **Requires Secure Boot off**;
  some anti-cheat refuses to run while it is on. A Windows VM with Steam + Vibepollo is the safer
  first rig; a stream from it to the headset verifies detection, glyphs, pads, grips, motion, haptics.
- Install the test package (elevated PowerShell, inside the downloaded package folder):
  `.\tools\trust-test-certificate.ps1 -PackageDir .` then
  `.\tools\VibeshineVhfGamepadDeviceSetup.exe install --inf .\driver\VibeshineVhfGamepad.inf`
  (exit 3010 = reboot). `status` and `remove` exist too. An existing Vibepollo 2.0.0 driver is
  replaced; `remove` + reinstalling the 2.0.0 installer restores it.
- Vibepollo build with the new mapping: the fork's `ci-windows.yml` downloads a *released* driver
  package by tag with pinned hashes from `Nonary/libvirtualgamepad`. To build the fork: tag the
  driver fork `v0.1.0-beta.<N>` (N well above upstream, e.g. 100) → `release-windows.yml`
  publishes a prerelease with the ZIP, `.sha256`, `.release-lock.json`; then in Vibepollo's
  `ci-windows.yml` change both `-Repository` to `jlobue10/libvirtualgamepad` and the `VHF_TAG`,
  `VHF_ARCHIVE_SHA256`, `VHF_DRIVER_VER`, `VHF_SOURCE_REVISION` values (and
  `SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION` checked by
  `packaging/windows/virtual_gamepad_driver/tests/test_release_contract.cmake`), commit, and
  dispatch `ci-windows.yml` (it has `workflow_dispatch`). Install the resulting installer, set
  gamepad = `vhf_steam`, stream from the headset.
- Vibepollo logs: `%ProgramFiles%\Vibepollo\config\sunshine.log`; look for
  "will use the Vibepollo virtual gamepad driver" and the profile description
  "a Steam Controller (2026)". Unknown feature commands are counted in the driver
  (`feature_state.unknown_commands`) but not yet surfaced; add a `raw_hid_report_feedback`
  event if Steam misbehaves and the capture does not explain it.

### 7.6 Working notes
- `gh` in the driver clone targets Nonary's repo through the `upstream` remote unless
  `gh repo set-default jlobue10/libvirtualgamepad` was run.
- The driver tests build with plain MSVC/CMake (no WDK). The driver itself needs MSBuild + WDK
  10.0.26100; CI has them, so prefer CI builds over a local toolchain.
- Never publish the LocalTest certificate or package as a release input (README).
- Upstreaming order: driver PR first (profile + tests + evidence per PROFILE_CONTRACT.md §"Adding a
  profile", including the Windows enumeration evidence and Steam compatibility result), then the
  Vibepollo PR once the driver release carries the profile.

### 7.7 Windows session 2026-10-05: what is done, what is left
Done on the author's desktop (Windows 11 Pro 26300):
- Git 2.54, GitHub CLI 2.96 (`gh auth status` = jlobue10), Wireshark 4.6.8 and USBPcap 1.5.4 installed
  via winget; `upstream` remote added and `gh repo set-default jlobue10/libvirtualgamepad` run in the
  driver clone; `jlobue10/Vibepollo` `feat/steam-controller-profile` cloned with submodules next to it.
- `tools/capture/Get-SteamControllerHid.ps1` parse-checks clean and runs (it correctly reported
  "no VID_28DE device" with nothing plugged in).
- `tools/capture/Capture-SteamController.ps1` added (see §7.2/§7.3). Verified: parses, refuses to run
  non-elevated, refuses to run before the USBPcap filter is attached, and its Ctrl+C stop helper
  cleanly ends a child console process (tested against `ping -t`).
- Later the same day, after the reboot: both captures taken (`captures/`). Two live runs recorded but
  wrote no artifacts (the first because a rerun in the same window hit USBPcapCMD's refusal to
  overwrite its output file; the Steam run's reason was not recorded); both were recovered with the
  new `-FromPcap` mode. The tshark extraction was fixed against the real capture (`usbhid.data`,
  `usbhid.setup.*`, raw-frame GET_REPORT payloads) and verified.
- §7.4 steps 1–2 done: real descriptor, bcdDevice, attribute defaults, corrected string-attribute
  tags, replies for `0xC1/0xDC/0xE2/0xED/0xF2`, per-report output sizes (`report_size()`,
  `is_output_report()` 0x80–0x89), gate flipped, tests updated with a replay of the captured
  handshake. `test_sc26_usb`, `test_pid_descriptor` and `test_profile_identity` pass locally
  (g++ 16, MSYS2) with the mask at 0x17C.

- §7.4 step 3 done: `test-driver.yml` green on the push, `test-signed-package.yml` run 37344823201
  green, artifact downloaded to `artifacts/` (gitignored).
- Test-rig state of the author's desktop: Secure Boot **on**, test signing **off**, no VHF driver
  installed, Vibepollo not installed, Steam installed, Hyper-V running (256 GB RAM, no Windows ISO on
  disk). So the choice in §7.5 is: Secure Boot off + `bcdedit /set testsigning on` + reboot on the
  desktop, or a Hyper-V Windows 11 VM (Steam + Vibepollo in the VM, Moonlight client on the network).
- Vibepollo fork build prepared but not started: `tools/capture/Update-VibepolloDriverPins.py` rewrites
  the seven pins (ci-windows.yml ×3 blocks and the `-Repository` arguments, the CMake contract,
  install.ps1, the submodule gitlink) through the GitHub API from a release's `release-lock.json`;
  dry run verified against the fork branch. It needs the driver prerelease first.

Left:
1. Done 2026-10-05 22:05: `v0.1.0-beta.101` on ce39de4 published immutably (DriverVer `10/05/2026,0.1.0.68`,
   archive sha256 4a717c63…043f1). Lessons folded into the text above: lightweight, unsigned, one tag
   per commit, immutable releases on.
2. Done 22:50: `Update-VibepolloDriverPins.py --tag v0.1.0-beta.101` committed f78995a + 20ac844 (verifier
   scripts must also accept the fork as producer) on the Vibepollo fork; `ci.yml` run 37381766579 built
   `VibepolloSetup.exe` (artifact of that name, uploaded raw: fetch it through the artifacts API, not
   `gh run download`; sha256 in the `release-provenance` artifact). The Arch Linux job of that run fails
   for an unrelated reason. The installer is in the desktop's kit
   `C:\VMs\sc26-rig\kit\sc26-test-kit-e6611a2.zip` (41 MB) next to the driver package and the probe.
3. §7.5 test rig: a Hyper-V Windows 11 VM `SC26-RIG` exists on the author's desktop (built 2026-10-05
   under `C:\VMs\sc26-rig`: unattended install, test signing on, PowerShell Direct as `tester`; the
   scripts there provision it, update the driver from a downloaded test-signed package and run
   `probe_sc26_usb` in it). The probe passes on commit e6611a2 (`docs/SC26_USB_COMPATIBILITY.md`,
   evidence section). Steam (unlogged-in client) recognised the virtual controller as a Steam Controller and drove it
   (`docs/SC26_USB_COMPATIBILITY.md`, `captures/rig/`). The VM is shut down (restartable); the Steam-login check
   there was skipped in favour of a physical Windows 11 host using the `tools/test-rig/` kit, where the
   fork installer, `gamepad = vhf_steam` and the Moonlight stream will be exercised.
4. 2026-10-06: the kit ran on a physical Windows 11 host (test signing on, `Install-TestDriver.ps1
   -Probe` done). `VibepolloSetup.exe` from fork run 37381766579 installed Vibepollo 2.0.99 (exit code
   0) but reported "Virtual gamepad driver setup failed". Cause: the unsigned fork build bundles the
   beta.101 producer package, whose `.cat` and `VibeshineVhfGamepadDeviceSetup.exe` carry no Authenticode
   signature (SignPath signs them in upstream builds), so the MSI's `install.ps1` throws "Catalog or
   root-device setup tool has no intact Authenticode signature" before touching the device
   (`-AllowLocalTestCertificate:0`; the CMake local-test path `SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE`
   expects a manifest-bearing self-signed package, which the `test-signed-package.yml` artifact is not).
   Expected per the kit README. The kit-installed driver and `ROOT\VIBESHINEVIRTUALGAMEPAD 0` survive
   because the MSI's `cleanup.ps1` looks for `pnputil.exe` under the WOW64-redirected System32 from the
   32-bit custom action server and always fails ("PnPUtil is unavailable", also in the 2.0.0 uninstall
   log). That is an upstream bug; a fix (Sysnative-aware lookup like `install.ps1`) is committed on the
   Vibepollo fork branch `fix/vhf-gamepad-cleanup-pnputil-wow64` (929d5d5) off upstream `master`, not on
   the profile branch, because the current behaviour is what keeps the kit's driver installed. Remaining
   on the host: `gamepad = vhf_steam`, the "will use the Vibepollo virtual gamepad driver" log line, the
   Moonlight stream, `Collect-Evidence.ps1`.
5. 2026-10-06 (later): the user asked for a rebuilt installer. On the Vibepollo fork branch:
   merged the cleanup fix (5e641b7), added a `vhf_local_test_package` dispatch input to `ci.yml` /
   `ci-windows.yml` (760f093, 56306d3): the "Fetch pinned VHF producer release" job then checks out the
   `third-party/libvirtualgamepad` submodule (gitlink must equal the pinned revision), runs
   `tools/build-driver.ps1 -SigningMode LocalTest -DriverVer <pin>` plus `tools/verify-driver-package.ps1
   -AllowLocalTestCertificate` (manifest channel `self-signed-local-test`), and the build configures CMake
   with `SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE=ON`, so the MSI ships the `.cer` and runs
   `install.ps1` with `-AllowLocalTestCertificate 1`. First run 37486677594 failed in the CMake refresh
   target: `refresh_driver_package.ps1` passed `@($null)` as `signed_downstream` for a manifest without that
   list (upstream has the same code); fixed null-safe in 77fe6d5. Run 37490202610 built
   `VibepolloSetup.exe` (sha256 8bb84183...ac9936, matches `release-provenance`), copied into the kit folder
   and zip. Still outstanding on the host: install it, `gamepad = vhf_steam`, log line, Moonlight stream,
   `Collect-Evidence.ps1`. The local-test input is for testers only; the upstream PR keeps the production
   path (SignPath signs the catalog).

### 7.8 Linux session 2026-10-07: field fixes shipped, kit rebuild

Done from the garage box (Linux) after pulling the Windows session's commits:
- Driver fork branch `feat/steam-controller-profile` = 8c75150 (View/Menu bits, pad click on the
  touched pad, tests, docs §8). `test-driver.yml` run 37596677768 green.
- Driver prerelease **`v0.1.0-beta.102`** (lightweight tag on 8c75150, run 37596883215):
  https://github.com/jlobue10/libvirtualgamepad/releases/tag/v0.1.0-beta.102, DriverVer
  `10/07/2026,0.1.0.74`, archive sha256
  `11bec8577e4932723ba7b4af9f1c0c2263e6983e79ab315e72692a6c5758275f`.
- Test-signed driver package for the kit's `vhf-package\` and the probe: `test-signed-package.yml`
  run 37596882384, artifact `vhf-gamepad-test-signed-x64-8c75150ae93aacf62a197bdfc55854cc651185cd`
  (`gh run download 37596882384 -R jlobue10/libvirtualgamepad -D artifacts/`).
- Vibepollo fork branch `feat/steam-controller-profile`: 55383c1 (single-touchpad clients: the pad
  half selects the pad, see §8) + accb4f5 (pins → beta.102, written by
  `Update-VibepolloDriverPins.py`). Installer build: `ci.yml` run **37597106078**
  (`workflow_dispatch`, `vhf_local_test_package=true`): **Windows jobs green** (the run shows
  "failure" only because of the unrelated Arch Linux job, as before). Kit installer = artifact
  `VibepolloSetup.exe` (artifact id 11472796073, 43.6 MB), sha256
  `bc9a621c52b8723fce255994527adbae8e3afb7b56922741609a83f44f9ed75a` (from `release-provenance`).
  Fetch: `gh api repos/jlobue10/Vibepollo/actions/artifacts/11472796073/zip > VibepolloSetup.zip`
  (raw upload, so the zip holds `VibepolloSetup.exe`), then check the hash.
- Local clone notes: `~/GitHub/Vibepollo` origin fetch refspec was master-only and is now
  `+refs/heads/*`; the branch tracks `origin/feat/steam-controller-profile`. The driver tests build
  on Linux with the shim in the session scratchpad (`winshim/windows.h`, `winioctl.h`; recreate two
  empty headers if the scratchpad is gone) via
  `g++ -std=c++20 -I include -I driver/src -I <shim> driver/tests/test_pid_descriptor.cpp driver/src/{pid_ff,profile,report_pump,dualshock4,dualsense,switch_pro,steam_controller,xbox_one,xbox_series}.cpp`.

On the Windows host next:
1. Done 2026-10-07 (Windows session), refreshed again the same day for beta.104: installer from Vibepollo fork
   run 37619035635 (sha256 7465078f...535f), driver package from test-signed run 37619021508 (commit 2305ebf).
   Earlier that day: both artifacts are in `C:\VMs\sc26-rig\kit\sc26-test-kit-e6611a2\` on the
   desktop (folder name kept; `VibepolloSetup.provenance.json` and `vhf-package\SOURCE.txt` record run 37597106078 /
   37596882384, sha256 verified), zip regenerated. Copy it to the garage host.
2. Run the new `VibepolloSetup.exe` (it installs the beta.102 driver itself; test signing must
   still be on), keep `gamepad = vhf_steam`, restart Vibepollo.
3. Stream from the headset (Moonlight fork.16 or later, either trackpad setting; moonlight-android PRs #37
   "movable/recenter stereo screen" = fork.17 and #38 "PyroWave HDR10" = fork.18 were opened 2026-10-07 and are
   independent of this test). In Steam's
   controller test check: each pad spans its own full width, right-pad touch and click register,
   View and Menu are on the right buttons. Then `Collect-Evidence.ps1` and commit the zip under
   `captures/rig/`.
4. When clean: upstream PRs. Driver PR to Nonary/libvirtualgamepad from this branch (squash the
   docs noise; keep `captures/` out or trimmed per the profile contract's evidence rules), then the
   Vibepollo PR (`vhf_steam`, touchpad index + capability plumbing, the cleanup fix is already on
   `fix/vhf-gamepad-cleanup-pnputil-wow64`) once a Nonary driver release carries the profile.

### 7.9 Linux session 2026-10-08: circle-step burst fix, CI repaired

State when this session ended (branch `feat/steam-controller-profile` = 5f75c05; no new beta tag,
nothing merged upstream; last prerelease is still `v0.1.0-beta.105` = edb2d38, which predates
`fb21e41` "grips only from the client"):
- The 2026-10-08 Windows probe runs (commits aa64eb2..9355094, findings in the §8 row "Stick circle
  step: what Steam keys on") cleared the stream's stick shape and 66/s value cadence; the burst was
  the one property left. Commit **6f2ad3a**: `submit_motion_state` folds a Steam Controller motion
  sample into the state and returns; `evt_sc26_tick` (4 ms) carries it, so the wire is an even
  cadence instead of three reports in a millisecond per BLE packet.
- CI "Controller protocol tests" had been red since c2cb0a5 (probe only built with MinGW):
  f6dc6a9 `NOMINMAX`, 5f75c05 link `winmm`. Run 37877928931 green.
- **Test-signed driver package to install on the host:** `test-signed-package.yml` run
  37877814035, artifact `vhf-gamepad-test-signed-x64-6f2ad3a1de75e2243edc5e122460bf1f40ce3e9e`
  (id 11593181427, 1.5 MB): https://github.com/jlobue10/libvirtualgamepad/actions/runs/37877814035
  (or `gh run download 37877814035 -R jlobue10/libvirtualgamepad -D artifacts/`). Unzip into the
  kit's `vhf-package\`, run `Install-TestDriver.ps1` elevated as before. Vibepollo on the host
  needs no change (it only forwards; the driver pins in the fork installer still say beta.105,
  which the test package replaces).
- The owner's last test before this change: client fork.24 (no rim stretch; the stick path is
  passthrough plus a 5 % centre dead zone) on the host driver of 2026-10-08 (which exact package is
  unknown: beta.105 or the fb21e41 test package from run 37802277663) → circle step still stalls,
  grips never light.

Next on the Windows host, in order:
1. Install the 6f2ad3a test package, stream from the headset (fork.24), run Steam's controller
   test. Expected: the left-stick circle step completes. If it still stalls, run
   `probe_sc26_usb --rate 200 --burst 3` (the old burst pattern against the probe alone) and
   `--rate 200 --burst 1` to see whether the burst is really what Steam rejects; also
   `--monitor 60` during a stream to confirm the wire now shows no bursts (largest gap ~4 ms,
   ~250/s, value changes ~66/s).
2. Grips: get the client's stream log (Settings → Misc → Share stream log) with the controller
   held for 10 s+. Line `Steam Controller BLE: stick extents (raw): ...; raw buttons seen 0x……
   (grip touch L yes/no R yes/no)`. "yes" while held → the bits flow and the host side is at fault
   (check the driver is the fb21e41+ package, then `--monitor` shows the grip bits). "no" → the BLE
   firmware does not send them; add a client proxy in `SteamControllerBle.handleState` (e.g. grip =
   stick-touch bit 26/27 or any pad touch) behind a preference.
3. When both pass: `Collect-Evidence.ps1`, then the upstream PRs (§7.8 step 4).

### 7.10 Windows session 2026-10-09: grips and stick touch over the stream, keep-alive cadence, full-scale sticks (pick up here)

State when this session ended (branch `feat/steam-controller-profile` = 01ea7fc; prerelease
`v0.1.0-beta.106` = 4e60d70; the garage host runs the 0.1.0.101 test package = 4dc591d; Vibepollo PRs
#1 and #2 open, pinned to beta.106; Moonlight fork.25 released, fork.26 = PR #47 awaiting the
stream test; nothing merged upstream):
- **Grips and stick touch now light on Steam's test screen over the stream.** Grips were dark
  because Vibepollo's `supported_button_mask` (`vhf_gamepad_policy.h`) lacked the grip-touch bits and
  `make_input_state` ANDs the button word with it (Vibepollo PR #1, 875d176b). The BLE stick-touch
  bits (20/24) were never forwarded and the driver guessed touch from deflection: now
  `LI_CCAP_STICK_TOUCH` (0x400) with `LEFT/RIGHT_STICK_TOUCH_FLAG` (common-c 0b1d3ec, fork.25,
  Vibepollo PR #2, driver 4e60d70 `stick_touch_explicit`).
- Vibepollo CI guard: "Fetch pinned VHF producer release" throws unless the
  `third-party/libvirtualgamepad` gitlink equals the pinned `VHF_SOURCE_REVISION`. New driver
  headers need a driver release tag first, then `Update-VibepolloDriverPins.py --tag`.
- **Keep-alive cadence.** `probe_sc26_usb --monitor` during a stream showed ~115 reports/s with
  16..19 ms gaps on 0.1.0.98. A UMDF `WDFTIMER` fires on the system clock interrupt (15.6 ms)
  unless some process has raised the timer resolution; the probe does (`timeBeginPeriod(1)`), so
  every probe run ticked at 4 ms and no stream ever did. `WDF_TIMER_CONFIG::UseHighResolutionTimer`
  is KMDF-only: adef7b3 silently ran with no keep-alive at all (exactly the 67 BLE packets/s).
  4dc591d moves the keep-alive to a worker thread on a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`
  waitable timer: `--monitor` now shows ~245/s with an 8 ms worst gap.
- **Circle step, root cause found.** With the cadence fixed the stream still stalled, and the probe
  gained `--scale S` (magnification of the clipped shape) and `--clip N` (largest axis value).
  `--hold --circle 1 --rate 120 --update 66 --scale 1.15 --turn 0.5` (the stream's shape, update
  rate and turn speed) passes; the same run with `--clip 32766` stalls. Steam waits for the stick
  to sit at exact ±32767, and Moonlight's `reportControllerState` scaled sticks by `0x7FFE`, so a
  fully pushed stick arrived as 32766 on every build so far (fork.21's rim stretch included).
  Fix: moonlight-android PR #47 = fork.26 (`driverStickAxis`: round(v · 0x7FFF), clamped).

Next on the Windows host, in order:
1. Sideload fork.26, stream, run Steam's controller test: the left-stick circle step is expected to
   pass with the grips and stick touch lit. If it still stalls, `--monitor 60` during the step
   must now show `|x| 1.00 |y| 1.00` reached at exactly 32767 (add a raw-extreme column if needed).
2. Merge PR #47 and dispatch `release.yml` with `release_tag=v20.3.0-fork.26`. Tag
   `v0.1.0-beta.107` at 01ea7fc (keep-alive thread, probe `--scale`/`--clip`; the owner pushes the
   tag), `Update-VibepolloDriverPins.py --tag v0.1.0-beta.107`, Vibepollo CI with
   `vhf_local_test_package=true`, merge Vibepollo PR #1 then #2 into `feat/steam-controller-profile`.
3. `Collect-Evidence.ps1`, then the upstream PRs (driver first, then Vibepollo without the
   local-test input and fork pins; the Moonlight fork stays a fork).

## 8. Field fixes after the first stream (2026-10-07)

| Symptom in Steam's controller test | Cause | Fix |
|---|---|---|
| Left pad drives only the left half of the left pad; right pad drives the right half of the left pad; right pad never activates | The Galaxy XR client defaults to "Trackpads as DualShock touchpad halves" (it sends both pads as two fingers on touchpad 0, each confined to a half, without `LI_CCAP_DUAL_TOUCHPAD`). Vibepollo mapped `touchpadIndex` straight to the driver's contact, so everything became the left pad. | Vibepollo (`vhf_gamepad.cpp`) now keeps the client's `LI_CCAP_*` flags per slot: with `LI_CCAP_DUAL_TOUCHPAD` the touchpad index is the pad; without it the half the finger went down in selects the pad (tracked per pointer so releases land on the right pad) and the half is stretched back to the pad's full width. Either client setting now works. |
| Right pad click never registers | The protocol has one click flag; `sc26_buttons()` gave it to the right pad only when the left pad was untouched, and all touches were "left" (above). | `sc26_buttons()` clicks the touched pad(s): left only, right only, both when both are touched, right when none is. |
| View and Menu swapped | Mapped by Valve's constant names (`start→btn_menu 0x4000`, `back→btn_view 0x40`); SDL and the client put START on 0x40 and BACK on 0x4000. | `start→btn_view`, `back→btn_menu`; pinned by `test_pid_descriptor`. |

The pad-click resolution is the best the current protocol allows: a click while both pads are
touched clicks both. A per-pad click flag would need a protocol extension on both the Moonlight
and the driver side.

Second round (2026-10-07, beta.103), from the beta.102 test on the garage host:

| Symptom in Steam's controller test | Cause | Fix |
|---|---|---|
| Gyro test: the controller glyph does not move / does not match the motion | The real unit's `imu_timestamp` (offset 30) is a microsecond clock, ~3.8 ms per 250 Hz report, and Steam integrates the gyro over its deltas. The virtual device incremented it by 1 per IMU sample, so Steam integrated 1 us per sample | `sc26_tick()` stamps every report with the driver's performance-counter microseconds before encoding (`now_us()` in driver.cpp) |
| Grip sensors: no way to test | The protocol has no grip-sense event, so the capacitive grip bits (0x10000000 / 0x20000000) were never set | Derived: both grips read "held" while motion samples arrived within the last second (`k_sc26_grip_hold_us`); a client that streams motion is in someone's hands. Turn the client's motion off and the grips read released |
| Grip sensors, properly (beta.104) | Clients could not send grip sense at all | Moonlight extension `LI_CCAP_GRIP_SENSE` (0x200) with `LEFT/RIGHT_GRIP_TOUCH_FLAG` (0x400000/0x800000) on the common-c fork, forwarded by the Android BLE driver (fork.19) and by Vibepollo (`platf::LEFT/RIGHT_GRIP_TOUCH`, fork branch 277c651); `button_mask::left/right_grip_touch` in the driver sets the device's grip bits per side, and the first grip bit seen on a controller turns the motion heuristic off for good |
| Reports stop while the client is quiet (beta.105) | The pump sends on changes only; the real unit streams ~250 Hz regardless, and Steam paces gyro integration and smoothing on that cadence | `evt_sc26_tick` (4 ms WDF timer, started by the first input state, stopped when no Steam Controller slot is active) resends the last state when nothing went out for 3.5 ms. The input-path audit of 2026-10-07 found no other blocker: BLE link at a 15 ms interval (Android's HIGH priority), notifications parsed on the Bluetooth thread, common-c merges only same-button analog updates into the not-yet-sent packet, Vibepollo forwards each packet synchronously to the driver, VHF readiness gating adds no wait while a reader is pending |
| Left stick "move in a full circle" calibration step stalls | Not changed. The stick is passed through untouched (client `s16/32767`, host, driver int16); Steam's step wants the raw magnitude to reach the rim all the way round, which a physical stick's circular limit does not give on every diagonal. Still stalls on beta.104. Steam's controller test (as described by the tester 2026-10-07) runs in order: left trigger full pull, right trigger, finger across the whole left pad, whole right pad, left stick in circles ("multiple times until it registers"), right stick, every remaining button including stick and pad clicks, then A when the left haptic buzzes and A when the right one buzzes; the grips light blue whenever grip sense is detected. `probe_sc26_usb --hold` now drives exactly that sequence (whole-surface pad sweeps, three full-magnitude turns per stick, every button, automatic A on each haptic feedback event, grip flags toggling): if Steam's stick step passes with the probe, the driver is fine and the loss is upstream (the fork.20 client logs each stick's peak magnitude and angular coverage); if it stalls with the probe too, Steam wants something the virtual report lacks. RESULT 2026-10-07: the probe-driven test passed every step, so the driver and report format are cleared; over the stream the step still stalls (client log: BLE stick magnitude up to 1.18, i.e. the BLE stick is not shaped like the wired report). Client fork.21 rescales the BLE sticks to the rim and STILL stalls, although its output is a unit circle like the probe's (fork.21 log: raw axes clip at 1.00, compass peaks 1.06-1.18, i.e. the BLE values are ~1.2x over range and clipped per axis). So the shape is not what Steam objects to; `probe_sc26_usb --circle N` drives the circle step under the stream's other conditions one at a time (clipped shape, 60/s cadence, motion streaming, 100 ms gaps) and `--monitor` reads the stream's virtual controller from the host side | probe scenarios + host monitor |
| Stick circle step: what Steam keys on (2026-10-08, probe runs at the wired unit's 250 reports/s) | A perfect unit circle whose value creeps by a tiny step on every report stalls the step at any turn speed; the wired unit's rounded square (`--circle 1`, each axis parked at full deflection) passes; a unit circle held 50 ms between steps (`--update 20`) passes; `--circle 1 --update 66` (the stream's shape and update rate) passes. Rate, motion data and gaps are innocent. The client's fork.21 rim stretch produced exactly the shape that stalls and was retired (fork.23, off by default), yet fork.24 still stalls | The one stream property the probe had not staged is the burst: Vibepollo forwards each Moonlight packet as an input state plus two motion samples, and the driver submitted a full report for each, so Steam saw three reports within a millisecond and then ~12 ms of nothing, 66 times a second (`--monitor` counted ~248/s). Motion samples are now folded into the state without submitting; `evt_sc26_tick` carries them in the next 4 ms report, so the wire is a steady cadence with the stick changing once per BLE packet. `probe_sc26_usb --rate 200 --burst 3` reproduces the old burst against the probe for comparison. Also: the probe did not compile under MSVC since `--rate` (`windows.h` `min`/`max` macros vs `std::min`/`std::max`, CI "Controller protocol tests" red), fixed with `NOMINMAX` | driver: `submit_motion_state` returns after `apply_sc26_motion` for this profile; probe: `NOMINMAX` |
| Grips never light up on Steam's test screen (fork.24 client, 2026-10-08) | With the motion heuristic (beta.103/104 hosts) both grips read held for the whole stream, which Steam shows as not touched; with the client-only grips (fb21e41 test package) they stay released unless the client sends `LEFT/RIGHT_GRIP_TOUCH_FLAG`. SDL's Triton driver reads the grip bits (0x10000000 / 0x20000000) from the same button word on the BLE state report, so the client decodes the right bits; whether this firmware sets them over BLE is answered by the fork.22+ stream-log line `raw buttons seen 0x…… (grip touch L yes/no R yes/no)` while the controller is held | Pending the fork.24 stream log. If the BLE report never carries the bits, the client needs another source (e.g. stick or pad touch, or any input, as a grip proxy) | open |
| Grips lit in the client log (`grip touch L yes R yes`) but never on Steam's screen (2026-10-09, fork.25 + driver 0.1.0.98) | Vibepollo's `supported_button_mask` lacked `LEFT/RIGHT_GRIP_TOUCH`; `make_input_state` ANDs the client's button word with it, so the driver never saw the bits | Vibepollo PR #1 (875d176b) adds them to the mask; confirmed lit on Steam's test screen over the stream |
| Stick touch guessed from deflection (misses a resting thumb, reads a bumped stick as touched) | BLE button bits 20/24 (right/left stick touch) were never forwarded | `LI_CCAP_STICK_TOUCH` (0x400) + `LEFT/RIGHT_STICK_TOUCH_FLAG` on common-c 0b1d3ec, client fork.25, Vibepollo PR #2 (`platf::LEFT/RIGHT_STICK_TOUCH`), driver 4e60d70 (`button_mask::left/right_stick_touch`; the first bit seen turns the deflection heuristic off) |
| Wire ~115 reports/s with 16..19 ms gaps over a stream (`--monitor`, driver 0.1.0.98) while every probe run saw ~250/s | A UMDF `WDFTIMER` fires on the system clock interrupt, 15.6 ms unless a process raised the timer resolution; the probe's `timeBeginPeriod(1)` masked it. `UseHighResolutionTimer` is KMDF-only, so adef7b3 silently had no keep-alive (67/s) | 4dc591d: keep-alive worker thread on a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` waitable timer (plain waitable timer before Windows 10 1803), started by the first input state, joined by the stop paths; `--monitor` shows ~245/s, 8 ms worst gap |
| Left-stick circle step stalls over a stream although rate, update rate, shape, turn speed, grips and stick touch all match a passing probe run | Steam waits for the stick to sit at exact ±32767. Moonlight's `reportControllerState` scaled sticks by `0x7FFE`, so a fully pushed stick arrived as 32766 on every client build (`probe --circle 1 ... --clip 32766` stalls the step; 32767 passes) | moonlight-android PR #47 = fork.26: `driverStickAxis` scales by `0x7FFF`, rounds and clamps to ±32767 on the USB/BLE driver path |
