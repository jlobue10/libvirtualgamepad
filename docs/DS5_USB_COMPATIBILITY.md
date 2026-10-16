# DualSense USB compatibility

The Windows VHF profile uses our portable `ds5_usb.h` contract. It has no
ViGEm dependency. Linux UHID DualSense uses the same firmware UpdateVersion,
USB descriptor, and sensor-enable command via Inputtino's `ds5_usb.hpp`. HD
haptics still need a DualSense USB audio function, which neither VHF nor UHID
provides.

Resonance: A Plague Tale Legacy identifies Sony 054c:0ce6, then classifies the
transport and initializes native scePad input. Matching VID/PID and sending
readable button reports does not satisfy that sequence.

## Verified native requirements

| Operation | USB requirement | Original defect |
| --- | --- | --- |
| Transport classification | Feature 85, page FF00, usage 2D, count 2 | Missing; classified as Bluetooth or skipped |
| Firmware descriptor | Feature 20, page FF00, usage 26, count 63 | Already correct |
| Firmware/calibration agreement | LE32 at offset 28 >= 0001003E selects calibration usage 33 | 00000100 selected legacy usage 23, absent from our descriptor |
| Firmware UpdateVersion | LE16 at offset 44 >= 0390 | 0000 made libScePad report a required DualSense firmware update and skip triggers/rumble |
| Improved rumble | Output valid_flag2 bit 2 (COMPATIBLE_VIBRATION2) | Only valid_flag0 bit 0 was decoded, so libScePad rumble v2 was ignored |
| Bluetooth output | Report 0x31, 78 bytes, common payload at offset 3 | USB-only 0x02 decode; libScePad sends 0x31 when OutputReportByteLength > 48 |
| Calibration descriptor | Feature 05, page FF00, usage 33, count 40 | Correct for the modern USB revision |
| Sensor initialization | SetFeature 08, 48 bytes, command byte 02, reserved bytes zero | Descriptor omitted and command rejected |
| Input / output / maximum feature sizes | 64 / 48 / 64 bytes including report ID | Already correct |
| Pairing descriptor | Feature 09, page FF00, usage 24, count 19 | Incorrect usage 34 |

The corrected descriptor exposes the complete USB feature inventory and restores
logical maximum 255 after the button fields. We implement the native sensor-enable
command precisely; unsupported factory/authentication operations still fail.
Firmware fields describe our supported protocol revision, not a claimed factory
build or driver-package version. The report still uses the hardware
`ReportFeatureInVersion` layout libScePad overlays: a printable build date/time,
nonzero `FwType`, `HardwareInfo`, and a 1.x `FirmwareVersion` (LE32 at offset 28
`0x0100003e`) so Sony's PC library does not treat the device as uninitialized.
Native transport classification depends on the descriptor for report 85; it does
not require a fabricated response to that factory operation. Pairing report 0x09
includes the hardware `08 25 00` bytes after the client MAC; libScePad reads
those as part of `ReportFeatureInMacAll`.

## Steam firmware notice

Steam has a separate update check from the native libScePad gate above. SDL
reads the LE16 UpdateVersion at report 0x20 offsets 44-45. In the Windows Steam
build with manifest version 1791918634, SteamUI.dll SHA-256
`1b9457943205dc3af0bd53463ba331cbd2a381388699705c02271aac7a2b1252`,
the decision at RVA `006BCBF0` uses PlayStation Accessories app 2053530's
`extended.version`, with fallback `0x0630` for the normal DualSense. A nonzero
version below that threshold schedules the notice when the other gates allow
it. This is independent of the printable date and offset-28 FirmwareVersion.

The virtual controller therefore advertises UpdateVersion `0x0630`, also the
2026-10-16 official Sony target 0004/000B version. This describes the reviewed
compatibility revision; it does not make this virtual device a flashable Sony
controller. Build date, HardwareInfo and FirmwareVersion remain unchanged.
A future remotely supplied Steam threshold can require a new compatibility
review. The portable test checks the actual feature reply and matches the
kernel gadget's full firmware report against the VHF/USB-IP report.

References: [SDL PS5 report parser](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c),
[Sony updater metadata](https://fwupdater.dl.playstation.net/fwupdater/info.json).
The offline native decision probe verified the current `0x0390` rejection,
`0x062f` rejection and `0x0630` boundary acceptance; live Windows Steam UI
validation remains separate.

The calibration report is 41 bytes with zero biases and interleaved gyro endpoints.
Gyro speed calibration is +/-512 degrees/s for +/-8192 counts, matching the
encoder's 16 counts per degree/s. The previous +/-8192 degrees/s magnitudes made
hosts interpret motion 16 times too large. Acceleration remains 8192 counts/g.
Pairing replies use a stable, distinct, locally administered unicast address per
slot in the host's expected byte order. Successful reads clear the caller's tail.

## Evidence and regression checks

Analyzed executable SHA-256:
`9F518C6D88D3A57F1EEAE4CE7708C666C0076331CBEFEDD3AD1FE4D219555195`.
Addresses below are RVAs, independent of ASLR:

- `25B43A0`: early DualSense USB marker check; enumeration branches on its result
  at `25B9130`.
- `25B4520`: reader transport classification using features 85 and 20.
- `25B676B`: firmware-dependent selection of modern versus legacy calibration
  feature usage. `25BAE69` reads the firmware word from report offset 28.
- `25B3010`: native USB sensor initialization; sets report 08, command 02 and
  calls HidD_SetFeature with length 48 at `25B317C`.
- `25B6926`: calibration read. `25B7460`: pairing read and wire-byte reversal.

`test_ds5_usb` validates parsed HID feature identities, lengths, buffer boundaries,
firmware/layout agreement, command validation, pairing identity, and host motion
calibration math. The profile regression test also calibrates an actual emitted
gyro report back to the original one-degree/s request.

`probe_ds5_usb` creates only slot 7 and selects its newly appearing VHF HID path,
then checks the native initialization sequence and receives a Cross input report.
It destroys its controller on completion; occupied slots fail without displacement.
The probe is manual, never an automatic CTest.

On 2026-09-13, the live 0.1.0.33 probe reproduced the missing USB marker, incorrect
pairing usage, missing/rejected sensor command, firmware/layout mismatch, gyro
scale, address, and stale-tail failures while ordinary button reads still worked.
All probe checks passed after installing locally signed driver 0.1.0.34. The DS4
live probe also passed. Both local source checkouts passed the MSVC test suites;
the driver built with WDK 26100 and passed INF, catalog, and signature verification.

A read-only snapshot of the running Resonance process showed no occupied native
scePad slots before the update. While our temporary neutral DualSense was held
open after the update, the game registered 054c:0ce6 and parsed firmware 0001003E
into its native controller record. This verifies native recognition; manual
gameplay and feedback behavior remain separate from that observation.

Protocol references:
- [Captured DualSense USB descriptor](https://github.com/nondebug/dualsense/blob/main/report-descriptor-usb.txt)
- [Linux hid-playstation calibration and firmware parser](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)
- Dying Light The Beast `libScePad.dll` 1.0.4.1 (SHA-256 `73d21bda527e53860b58c57de4a511507efaad4ddf3b4957ece4e49bc0791f84`) is DS4-only (VID 054C, PID 05C4/09CC). DualSense support lives in later Sony PC libraries; Resonance's statically linked scePad and duaLib's Stellar Blade-based `ReportFeatureInVersion` / `ReportFeatureInMacAll` overlays are the DualSense contract.
