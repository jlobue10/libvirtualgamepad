# Steam Controller (2026) profile: test kit for a Windows 11 machine

What this tests: the fork's VHF gamepad driver with the `steam_controller` profile
(`docs/STEAM_CONTROLLER_PROFILE.md`, evidence so far in `docs/SC26_USB_COMPATIBILITY.md`).
On the Hyper-V rig the probe passed and Steam identified the virtual device as a Steam
Controller. This kit repeats that on a physical Windows 11 machine and adds the Moonlight stream.

## Kit contents

| Item | Source |
| --- | --- |
| `vhf-package\` (driver, catalog, LocalTest `.cer`, setup tool) | `test-signed-package.yml` artifact; current = run 37596882384 for commit 8c75150 (beta.102 code) |
| `probe_sc26_usb.exe` | `driver/tests/probe_sc26_usb.cpp`, static MinGW build |
| `Install-TestDriver.ps1` | this folder |
| `Collect-Evidence.ps1` | this folder |
| Vibepollo fork installer | **not in the kit yet**: built by the Vibepollo fork CI after the driver prerelease tag exists (see "Vibepollo" below) |

## Requirements on the machine

- Windows 11, you are an administrator, Steam installed.
- **Secure Boot off** in the firmware. The driver is test-signed and only loads with test signing
  on, and Windows refuses test signing while Secure Boot is on. Some anti-cheat refuses to run
  while test signing is on; use a machine where that does not matter.
- Nothing from Vibepollo 2.0.0 needs to be removed first. The test driver replaces an existing
  Vibepollo driver; `tools\VibeshineVhfGamepadDeviceSetup.exe remove` plus the 2.0.0 installer
  restores it.

## Steps

All in an elevated PowerShell, from the kit folder.

1. Check and enable test signing:
   ```powershell
   .\Install-TestDriver.ps1 -EnableTestSigning
   ```
   It stops with a clear message if Secure Boot is still on. Reboot after it says to.
2. Install the driver and run the probe:
   ```powershell
   .\Install-TestDriver.ps1 -Probe
   ```
   Expected tail: `PROBE PASSED`. The probe creates a virtual Steam Controller on slot 7, checks
   the three HID collections (mouse, keyboard, vendor FF00/0001 with VID 28DE PID 1302 version
   0307), the attribute, serial and device-info replies, the 0x42 state report and the
   pulse-to-rumble feedback, then releases the controller. Exit code 3010 from the install means
   reboot and run step 2 again.
3. Steam check, with a logged-in Steam client:
   ```powershell
   .\probe_sc26_usb.exe --hold 900
   ```
   holds a live virtual controller for 15 minutes (Enter stops) and drives Steam's controller test
   in its own order, repeating: left trigger (full pull), right trigger, a finger across the whole
   left pad, the whole right pad, the left stick in circles, the right stick, every remaining
   button (stick and pad clicks included), then a 5 s quiet window in which each haptic pulse
   Steam sends is answered with an A press (the "press A when the left/right haptic buzzes"
   steps). The grip touch flags toggle every 2 s throughout, so the grips should light blue on
   Steam's screen. Open Steam → Settings → Controller: it should list a Steam Controller; note
   whether it offers a firmware update (it did not on the rig), then start the controller test
   and watch which step is the first that does not complete. The console prints each phase and
   button as it is driven, and each feedback event as it arrives.
   `--circle N` (0..5) changes how the two stick-circle steps are driven, to find what Steam's
   step objects to in a stream: 0 perfect unit circle at 20 reports/s (passes); 1 the shape the
   controller sends over BLE (axes clipped at full scale, magnitude 1.17 on diagonals); 2 unit
   circle at 60/s; 3 as 2 with gyro/accel streaming; 4 as 2 with a 100 ms gap every second (a
   jump, as a Wi-Fi hiccup leaves); 5 all of it. Run Steam's test once per mode and note which
   modes complete the left stick step. `--rate N` (20..250, steps of 20) sets how many reports per second
   the stick phases submit instead of the mode's 20 or 60; the turn stays 3 s long. `--rate 250` is the
   wired unit's cadence, `--rate 40` halves the fast modes. The hold now runs with 1 ms timer resolution,
   so these cadences are real (earlier builds slept 16-31 ms where 16 was asked).
   While a stream is running (the controller Vibepollo created, not the probe's own):
   ```powershell
   .\probe_sc26_usb.exe --monitor 120
   ```
   creates nothing and reads the state reports of the virtual Steam Controller that already
   exists, printing once a second what Steam sees: reports per second and the largest gap
   between them, each stick's magnitude range, per-axis peaks, 16-sector coverage, largest
   angular jump between consecutive reports and the peak at each compass point, plus the grip
   and stick touch bits seen. Circle a stick during it and compare the numbers with the
   client's `stick extents (raw)` log line to see where the rim is lost.
   Steam's own view is in `%ProgramFiles(x86)%\Steam\logs\controller.txt` ("Steam controller
   device opened", HWID 74, FWTimestamp 0x6A4D85E3).
4. Vibepollo: run `VibepolloSetup.exe` from the kit, set `gamepad = vhf_steam` in its
   config (web UI or `config\sunshine.conf`), restart Vibepollo. The kit installer is a
   *local-test* build (fork CI `vhf_local_test_package=true`; current = run 37597106078 for the
   beta.102 pins, the earlier one was 37490202610 for beta.101): it carries the
   driver signed with a throwaway certificate created on the CI runner, trusts that certificate
   in Root and TrustedPublisher, and installs the driver and root device itself. Test signing
   must still be on. It replaces the device from step 2 (same driver code, DriverVer
   10/07/2026,0.1.0.74 for beta.102), so the probe still passes afterwards. It must NOT report "Virtual
   gamepad driver setup failed" any more; if it does, keep the warning report it offers. The
   earlier kit installer (run 37381766579) was a plain unsigned build whose driver step always
   failed on that message, because its bundled catalog had no Authenticode signature; with that
   build the step-2 driver stayed in place and Vibepollo used it. Stream from a Moonlight client that
   reports a Steam Controller (`LI_CTYPE_STEAM`); the host log should say "will use the Vibepollo
   virtual gamepad driver" and describe "a Steam Controller (2026)". Steam on the host should see
   the controller while the stream runs.
5. Collect everything:
   ```powershell
   .\Collect-Evidence.ps1
   ```
   produces `evidence-<machine>-<time>.zip` (driver state, HID children, Steam controller logs
   and bindings, Vibepollo log lines). Commit it under `captures/rig/` in the driver fork.

## Vibepollo fork build (done from the development machine, not here)

1. Tag the driver fork: `git tag --no-sign v0.1.0-beta.<N> origin/feat/steam-controller-profile; git push origin v0.1.0-beta.<N>` (102 is taken)
   (a **lightweight** tag; `publish-release.ps1` refuses annotated ones and needs the repository's immutable-releases setting on. `release-windows.yml` publishes the prerelease).
2. `python tools/capture/Update-VibepolloDriverPins.py --tag v0.1.0-beta.<N>` rewrites the
   Vibepollo fork's seven driver pins and submodule gitlink through the GitHub API.
3. `gh workflow run ci.yml -R jlobue10/Vibepollo --ref feat/steam-controller-profile`, then
   download the `unsigned-installer-Windows` artifact and copy it into this kit.

## Removal

```powershell
.\vhf-package\tools\VibeshineVhfGamepadDeviceSetup.exe remove
certutil -delstore Root 01493403A9039F3F406AB9F94766CA303928F428
certutil -delstore TrustedPublisher 01493403A9039F3F406AB9F94766CA303928F428
bcdedit /set testsigning off
```
then reboot and re-enable Secure Boot.
