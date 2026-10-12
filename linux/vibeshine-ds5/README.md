# Vibeshine DualSense USB driver

Out-of-tree Linux driver that publishes a **virtual USB DualSense**
(`054c:0ce6`) with the real composite layout:

- interface 0: UAC1 audio control
- interface 1: 4-channel 48 kHz S16 playback (speaker + haptics)
- interface 2: 2-channel 48 kHz capture (headset)
- interface 3: HID gamepad, report descriptor and USB features from `ds5_usb.h`

The USB bus is a software loopback (a renamed `dummy_hcd` with isochronous
transfers enabled). Nothing physical is required. Proton sees a USB parent
shared by HID and `snd-usb-audio`, which is what Wwise DualSense haptics
match on via container ID.

Userspace talks to `/dev/vibeshine-ds5`:

1. `VIBESHINE_DS5_CREATE` with a slot (0–3) and MAC
2. `write()` 64-byte USB input reports
3. `read()` `vibeshine_ds5_event` records: HID output reports and haptic PCM

Build against the running kernel:

```sh
./build-module "$(uname -r)"
sudo insmod vibeshine_ds5.ko
```

HID feature reports (calibration 0x05, pairing 0x09, firmware 0x20, sensor
enable 0x08) are answered in the kernel so `libScePad` can initialize without
a round-trip.

File operations serialize lifecycle changes per open fd. Blocking reads bind to
one slot generation, so DESTROY followed by CREATE on the same fd cannot feed a
retired read the new controller's feedback. An unbound fd cannot read or destroy
a replacement owner's slot.

Run the portable ownership/concurrency regression without loading a module:

```sh
python3 linux/vibeshine-ds5/test_slot_lifecycle.py
python3 linux/vibeshine-ds5/test_audio_setup.py
```

The harnesses run production file operations and audio endpoint setup with fake
USB/FIFO boundaries under ASan/UBSan. Audio checks inject failure at endpoint
enable and each queue position, then verify error propagation, partial-queue
cleanup and a successful retry. Kernel builds and live USB behavior require
separate validation.
