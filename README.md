# libvirtualgamepad

`libvirtualgamepad` is Vibeshine's independently authored Windows virtual
gamepad driver and protocol. It uses Microsoft's in-box Virtual HID Framework
(VHF) from a UMDF2 source driver; it does not ship a custom kernel bus driver
and does not contain LizardByte/libvirtualhid source or assets.

## Design

```text
Vibeshine
  |  versioned, METHOD_BUFFERED control IOCTLs
  v
Vibeshine VHF UMDF source driver
  |  VhfUm.dll + Vhf.sys (inbox Windows components)
  v
HID child gamepad(s)
  |  HID input/output/feature reports
  v
Windows HID clients
```

The private source-device interface handles controller lifetime and normalized
input. VHF handles HID child enumeration. HID output reports are parsed in the
driver and returned to Vibeshine as bounded feedback events. A client closing
its control handle releases every controller it owns, so an application crash
cannot leave an owned virtual controller active.

The protocol lives in `include/libvirtualgamepad/protocol.h`. It is deliberately
small, fixed-size, versioned, and `METHOD_BUFFERED`; it is the only ABI shared
with Vibeshine.

## Profile contract

The target profile set is deliberately explicit:

| Profile | Contract |
| --- | --- |
| Generic HID | Protocol value reserved, but unavailable pending an accepted public VID/PID allocation. |
| Generic HID + PID | Protocol value reserved, but unavailable pending an accepted public VID/PID allocation. Its report encoder remains for private-test research. |
| Xbox 360 | Not reachable from VHF. A real Xbox 360 pad is an XUSB device on a USB bus, which needs a bus child that VHF cannot create. |
| Xbox One | Implemented. Native report shape by the same route as Xbox Series. |
| Xbox Series | Implemented. Native report shape plus the hardware ID that makes Windows attach its inbox XInput filter. |
| DualShock 4 | Implemented. Native report shape, touchpad, motion, battery, lightbar, and the calibration/pairing/firmware features. |
| DualSense | Implemented. As DualShock 4, plus adaptive triggers, player LEDs, and the microphone LED. |
| Switch Pro | Implemented. Native report set plus the USB handshake, subcommand replies, and emulated calibration flash a host reads before it will use the device. |
| Steam Controller (2026) | Implemented. Wired report set at hid-steam's offsets, the feature-report control channel Steam drives (settings, device info, keyed values, lizard mode), the 0x43 battery report, and Steam's haptic output reports forwarded to the host as `steam_haptic` events. See `docs/STEAM_CONTROLLER_PROFILE.md` and `docs/SC26_USB_COMPATIBILITY.md`. |

A profile becomes available only after its descriptor, input mapping, output
mapping, application behavior, and provenance have all been tested. The bar is
that the device behaves as the family it claims to be - not that it avoids the
family's identifiers. Shipping a generic pad wearing somebody else's VID/PID is
the failure this guards against, because the identity would promise
compatibility the reports cannot deliver.

Where an identity is a required part of a complete emulation, it is used. Xbox
Series is the worked example: Windows attaches its inbox `xinputhid.sys` filter
by hardware ID, so the identity is what puts the device on the XInput path at
all, and it ships together with the native report shape, trigger resolution,
hat encoding, and four-motor rumble payload rather than instead of them.

## Driver package and signing

Each architecture has its own production release package:

```text
driver/
  VibeshineVhfGamepad.inf
  VibeshineVhfGamepad.dll
  VibeshineVhfGamepad.cat
tools/
  VibeshineVhfGamepadDeviceSetup.exe
manifest.json
```

The producer builds this payload once. On an accepted release tag,
`tools/build-release-package.ps1` requires a clean tagged HEAD, orchestrates the
existing build, staging, `InfVerif`, `Inf2Cat`, and verification scripts, and
creates the unsigned-for-consumer-signing archive. The ZIP contains only the
five paths above. Its `.sha256`, `.release-lock.json`, and `-evidence.json`
records remain outside the ZIP so the archive never contains a circular hash
of itself.

Before pushing a release tag, the operator must enable repository immutable
releases and verify that prerequisite outside the workflow. The workflow uses
only its scoped `GITHUB_TOKEN`; it does not add an administration token solely
to read the repository setting. The tag-only publish job requires the tag to
remain a lightweight reference to the exact workflow revision both before
creating the draft and before publishing it. It uploads and verifies all four
files while the release is a private draft, then publishes the release and
requires GitHub to report that same release as immutable by both captured ID
and tag. Once publication is attempted, any failure preserves the release for
inspection because the API outcome may be ambiguous and GitHub does not allow
an immutable release's tag name to be reused.

Vibeshine and Vibepollo must download the same pinned producer asset and verify
its checksum, release lock, manifest, exact layout, source revision, DriverVer,
protocol, platform, `msi-request-signing` channel, and payload hashes. Consumers
must not compile or regenerate the driver, setup tool, INF, catalog, manifest,
or ZIP. Their own MSI SignPath request signs exactly
`driver/VibeshineVhfGamepad.cat` and
`tools/VibeshineVhfGamepadDeviceSetup.exe`; those two pre-signature manifest
hashes are expected to change, while the INF and DLL hashes remain immutable.
The consumer records its own local release lock after verification and signing;
that consumer lock is not inserted into the producer archive.

The producer's unsigned verification requires evidence written immediately
after successful `Inf2Cat` over the fresh package directory. That evidence binds
the exact INF, DLL, and CAT hashes; it is the unsigned catalog-membership basis,
because SignTool's catalog membership verification requires a signed catalog.
The catalog, driver DLL, and setup tool must each report Authenticode
`NotSigned` with no signer before the archive can be created.

`tools/prepare-driver-package.ps1` creates an unsigned staging directory and
refuses to overwrite an existing one. `tools/verify-driver-package.ps1`
verifies signed packages when signatures exist, or records the explicit
downstream-signing contract when called with `-UnsignedForMsiSigning`. The setup
tool is intentionally outside the catalog because it is not a driver payload.
Local test packages use a clearly separate local test certificate.

For a local test build, `tools/build-driver.ps1` follows the same package model
as Vibeshine's virtual-display driver: it builds the UMDF DLL and the owned
root-device setup tool, generates the final INF/catalog, creates or reuses
`CN=Vibeshine VHF Gamepad Test` in `CurrentUser\My`, signs the catalog and the
setup tool, and exports the public certificate as
`driver/VibeshineVhfGamepad.cer`. The private key never enters the package.
Trust that public certificate explicitly on the test host before installation:

```powershell
.\tools\build-driver.ps1 -Platform x64
.\tools\trust-test-certificate.ps1 -PackageDir .\artifacts\vhf-gamepad-x64-Release-<DriverVer>
.\tools\verify-driver-package.ps1 -PackageDir .\artifacts\vhf-gamepad-x64-Release-<DriverVer> -Platform x64 -DriverVer <DriverVer> -SourceRevision <git-sha>
```

The build script adapts to the installed toolchain: when Visual Studio is newer
than the WDK integration, or the Spectre-mitigated libraries are absent, it
warns and passes the matching MSBuild overrides for the driver project instead
of failing with an error that never mentions drivers. Pass `-SkipInfVerif` on a
host whose WDK does not ship `InfVerif.exe`; every warning it prints marks a
gate that a release package must still clear on a fully provisioned host.

On a clean Git worktree, the build script derives `DriverVer` from the latest
commit date and revision count. While iterating on uncommitted source, pass an
explicit monotonic newer `-DriverVer` so Windows cannot select an older staged
driver package.

The trust command must run elevated and installs the public certificate in
`LocalMachine\Root` and `LocalMachine\TrustedPublisher`. It does not install
the driver or create a virtual controller. A local test host might also need
Windows test-signing configuration; the script deliberately does not alter that
machine-wide setting. Use `tools/build-driver.ps1 -SigningMode Release` for a
release staging package. That mode deliberately omits the `.cer` and leaves the
final `.cat` and setup tool unsigned for their separate SignPath requests.
Never publish the local-test certificate as part of a SignPath release archive.

The public release workflow accepts only tags matching
`v0.1.0-beta.<positive-number>`. For example, `v0.1.0-beta.1` produces these
four release assets without signing them:

```text
libvirtualgamepad-0.1.0-beta.1-windows-x64.zip
libvirtualgamepad-0.1.0-beta.1-windows-x64.zip.sha256
libvirtualgamepad-0.1.0-beta.1-windows-x64.release-lock.json
libvirtualgamepad-0.1.0-beta.1-windows-x64-evidence.json
```

The setup tool creates or removes only `ROOT\VIBESHINEVIRTUALGAMEPAD`; it does
not enumerate physical HID devices or manage third-party virtual-controller
packages. Installing the INF into the Driver Store is not enough for this
root-enumerated source driver—the setup tool creates the source node and waits
for its private control interface before reporting success.

When Windows asks for a restart during installation, the setup tool first tries
to activate the exact owned source node in the current session: it
re-enumerates it, requests a PnP property-change restart, and then performs one
owned-node disable/enable cycle only if the private interface is still absent.
It never restarts shared UMDF hosts or unrelated HID devices. A reboot is
reported only when that bounded recovery cannot reload the owned node and
prove both the source interface and the selected driver version are live.

## Build baseline

The UMDF driver targets x64 and ARM64 Windows 10 or newer, uses UMDF 2.15 or
newer, and links the inbox VHF user-mode import library (`VhfUm.lib`). It must
not redistribute `VhfUm.dll` or `Vhf.sys`.

The generic report encoders remain as development seams for the VHF driver,
control protocol, controller cleanup, and generic HID feedback path. Their
protocol enum values are stable, but the public driver refuses both generic
profiles until this project receives an accepted VID/PID allocation. Adding or
enabling a profile is a driver change with descriptor and compatibility tests,
not a configuration-only branding change.

## Device identity

VHF leaves a HID child's VID/PID at zero unless the driver supplies them, which
gives Windows and applications nothing to match on. pid.codes reserves
`1209:0001` for private testing only, so the public driver does not define,
advertise, or create either generic profile with that identity. Their enum
values remain reserved for protocol compatibility until this project receives
an accepted public allocation. The DirectInput probes retain `1209:0001` only
as explicitly labelled private-test source references and are not release
profiles.

## XInput

`xinputhid.sys` is a filter driver, not a bus driver. `xinputhid.inf` attaches
it to any HID device whose hardware ID appears in its match list
(`HID\VID_045E&PID_xxxx&IG_00`), and `VHF_CONFIG` carries a `HardwareIDs`
field, so a VHF child can carry a matching ID and pick up the filter. That is
the whole mechanism: no bus driver, no kernel code in this project.

The IDs are supplied at runtime through `VHF_CONFIG`, so the signed INF and
catalog contain none of them. The Xbox Series profile offers the specific
Series identity first and the generic GIP software product ID behind it, so the
filter still attaches if the specific entry is ever retired.

Xbox 360 stays out of reach for a different reason. A real 360 pad is an XUSB
device on a USB bus; `xusb22.sys` binds to a bus child, which VHF cannot
create. No descriptor makes that profile honest, so `find_profile` refuses it.

## Force feedback

The reserved `profile::generic_pid` encoder publishes the DirectInput PID report
set alongside the game pad collection: Set Effect, Set Envelope, Set Condition,
Set Periodic, Set Constant Force, Set Ramp Force, Effect Operation, PID Block
Free, PID Device Control, and Device Gain as output reports, plus Create New
Effect, PID Block Load, and PID Pool as feature reports, and a PID State input
report. It remains useful for private-test research, but `find_profile()` refuses
it and the public driver does not advertise or create it pending an accepted
VID/PID allocation.

The driver reduces effects to the same rumble values the rest of the protocol
already carries. Constant force and ramps drive the low-frequency motor;
periodic effects drive the high-frequency motor at their magnitude, because a
rumble actuator cannot render a waveform but its amplitude is what the
application wants felt. Envelope attack and fade, per-effect gain, device gain,
start delay, duration, and loop count are honored, and finite effects stop
themselves so a host that never sends a stop cannot leave the motors running.
Condition effects (spring, damper, inertia, friction) depend on stick position
and have no rumble analogue, so they are accepted and produce nothing rather
than inventing vibration.

Feedback from a PID effect is reported as `feedback_type::generic_rumble`
rather than `generic_rumble_rgb`: a force says nothing about a light, and
forwarding a black LED would switch off the light on a client's real
controller.

A malformed report descriptor does not fail loudly - `VhfCreate` succeeds and
the HID child simply never starts. `driver/tests/test_pid_descriptor.cpp` walks
the descriptor the way HIDCLASS does and proves that every report's bit layout
matches the packed struct the driver copies into, so that class of error is
caught before it reaches a test machine:

```powershell
g++ -std=c++20 -I driver/src -I include `
  driver/tests/test_pid_descriptor.cpp driver/src/pid_ff.cpp -o pid_test
./pid_test
```
