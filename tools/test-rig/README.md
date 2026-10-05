# Steam Controller (2026) profile: test kit for a Windows 11 machine

What this tests: the fork's VHF gamepad driver with the `steam_controller` profile
(`docs/STEAM_CONTROLLER_PROFILE.md`, evidence so far in `docs/SC26_USB_COMPATIBILITY.md`).
On the Hyper-V rig the probe passed and Steam identified the virtual device as a Steam
Controller. This kit repeats that on a physical Windows 11 machine and adds the Moonlight stream.

## Kit contents

| Item | Source |
| --- | --- |
| `vhf-package\` (driver, catalog, LocalTest `.cer`, setup tool) | `test-signed-package.yml` artifact for commit e6611a2 |
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
   holds a live, animated virtual controller for 15 minutes (Enter stops). Open Steam → Settings →
   Controller: it should list a Steam Controller; note whether it offers a firmware update (it did
   not on the rig) and whether the input test shows the cycling A/B/X/Y presses and stick sweep.
   Steam's own view is in `%ProgramFiles(x86)%\Steam\logs\controller.txt` ("Steam controller
   device opened", HWID 74, FWTimestamp 0x6A4D85E3).
4. Vibepollo (once the fork installer exists): install it, set `gamepad = vhf_steam` in its
   config (web UI or `config\sunshine.conf`), restart Vibepollo. Its installer's own driver step
   is best-effort and leaves the test-signed driver in place. Stream from a Moonlight client that
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

1. Tag the driver fork: `git tag -a v0.1.0-beta.100 e6611a2 -m "..."; git push origin v0.1.0-beta.100`
   (`release-windows.yml` publishes the prerelease).
2. `python tools/capture/Update-VibepolloDriverPins.py --tag v0.1.0-beta.100` rewrites the
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
