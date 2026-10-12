#!/usr/bin/env python3
"""Text gate on ds5_setup's class-request routing (until a compiled control harness exists):
HID SET_REPORT/SET_CUR/SET_IDLE accept OUT requests only, a HID-interface class request that
matches no branch stalls instead of reaching the audio-control block, and the audio-control
block answers interfaces 0-2 with the interface recipient only. --baseline reads HEAD.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PATH = 'linux/vibeshine-ds5/vibeshine_ds5_gadget.c'
source = (subprocess.check_output(['git', 'show', 'HEAD:' + PATH], cwd=ROOT, text=True, encoding='utf-8')
          if '--baseline' in sys.argv else (ROOT / PATH).read_text(encoding='utf-8'))


def block(marker):
    start = source.index(marker)
    end = source.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == '{') - (source[end] == '}')
    return source[start:end + 1]


setup = block('static int ds5_setup(')
hid = block('\tif ((wIndex & 0xff) == 3 && (type & USB_TYPE_MASK) == USB_TYPE_CLASS')
get_interface = block('\tif (req == USB_REQ_GET_INTERFACE')
hid_head = setup[setup.index('\tif ((wIndex & 0xff) == 3 && (type & USB_TYPE_MASK) == USB_TYPE_CLASS'):]
hid_head = hid_head[:hid_head.index('{')]
uac_start = setup.index('/* UAC SET_CUR / GET_CUR')
uac_head = setup[uac_start:setup.index('{', uac_start)]
failures = 0
for ok, name in (
    ('req == 0x09 && !(type & USB_DIR_IN)' in hid, 'HID SET_REPORT queues an OUT data stage only'),
    ('req == 0x0a && !(type & USB_DIR_IN)' in hid, 'HID SET_IDLE is acknowledged for OUT requests only'),
    (hid.rstrip()[:-1].rstrip().endswith('return -EOPNOTSUPP;'), 'an unmatched HID-interface class request stalls instead of falling through'),
    ('req == 0x01 && !(type & USB_DIR_IN)' in setup[uac_start:], 'UAC SET_CUR consumes an OUT data stage only'),
    ('(type & USB_RECIP_MASK) == USB_RECIP_INTERFACE' in uac_head and '(wIndex & 0xff) <= 2' in uac_head,
     'the audio-control block answers the audio interfaces 0-2 only'),
    ('(type & USB_RECIP_MASK) == USB_RECIP_INTERFACE' in hid_head, 'the HID block answers the interface recipient only'),
    ('slot->config != 1 || wIndex > 3' in get_interface, 'GET_INTERFACE stalls when unconfigured or for an interface the descriptor lacks'),
    ('slot->ep0_pending_cs = cs;' in setup and 'slot->ep0_pending_cs == 0x02' in source, 'a UAC SET_CUR data stage is applied to mute/volume on completion'),
):
    print(('PASS: ' if ok else 'FAIL: ') + name)
    failures += not ok
sys.exit(1 if failures else 0)
