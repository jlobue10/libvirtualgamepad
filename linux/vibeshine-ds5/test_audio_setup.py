#!/usr/bin/env python3
"""Exercise production DualSense audio endpoint setup with injected USB failures.

Compiles the real enable/disable functions and SET_INTERFACE branch. Only USB
endpoint operations and EP0 acknowledgement are faked; no kernel is loaded.
--baseline reads HEAD.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATH = 'linux/vibeshine-ds5/vibeshine_ds5_gadget.c'
source = (subprocess.check_output(['git', 'show', 'HEAD:' + PATH], cwd=ROOT, text=True)
          if '--baseline' in sys.argv else (ROOT / PATH).read_text())


def block(marker):
    start = source.index(marker)
    end = source.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == '{') - (source[end] == '}')
    return source[start:end + 1]


code = r'''
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
#define GFP_ATOMIC 0
#define USB_REQ_SET_INTERFACE 11
#define USB_RECIP_INTERFACE 1
@@CONSTANTS@@
struct usb_ep { void *driver_data; const void *desc; bool enabled; int queued; };
struct usb_request { unsigned int length; unsigned char storage[DS5_ISO_OUT_MAXP]; void *buf; };
struct ds5_slot {
  struct usb_ep *ep_iso_out, *ep_iso_in;
  struct usb_request *req_iso_out[DS5_ISO_OUT_Q], *req_iso_in[DS5_ISO_IN_Q];
  bool iso_out_enabled, iso_in_enabled;
  u8 alt1, alt2;
};
static int ds5_ep_iso_out_desc, ds5_ep_iso_in_desc;
static int failure, queue_calls, enables, disables, acknowledgements;
static int usb_ep_enable(struct usb_ep *ep) {
  ++enables; if (failure==0) return -EIO;
  ep->enabled=true; return 0;
}
static int usb_ep_queue(struct usb_ep *ep, struct usb_request *req, int flags) {
  (void)req; (void)flags;
  if (++queue_calls==failure) return -ENOMEM;
  if (!ep->enabled) return -ESHUTDOWN;
  ++ep->queued; return 0;
}
static void usb_ep_disable(struct usb_ep *ep) {
  ++disables; ep->enabled=false; ep->queued=0;
  // The loopback UDC completes queued requests with -ESHUTDOWN on disable.
}
static void usb_ep_dequeue(struct usb_ep *ep, struct usb_request *req) { (void)ep; (void)req; }
static int ds5_queue_ep0(struct ds5_slot *slot, const void *buf, int length, int requested) {
  (void)slot; (void)buf; (void)length; (void)requested; ++acknowledgements; return 0;
}
@@METHODS@@
static int set_interface(struct ds5_slot *slot, u16 wIndex, u16 wValue) {
  int n; u8 req=USB_REQ_SET_INTERFACE, type=USB_RECIP_INTERFACE;
  @@SET_INTERFACE@@
  return -EINVAL;
}
static int checks, failures;
static void check(bool ok, const char *name) {
  ++checks; failures+=!ok; printf("%s %s\n",ok?"PASS":"FAIL",name);
}
int main(void) {
  for (int direction=1; direction<=2; ++direction) {
    int depth=direction==1 ? DS5_ISO_OUT_Q : DS5_ISO_IN_Q;
    for (int fail=0; fail<=depth; ++fail) {
      struct usb_ep out={}, in={};
      struct usb_request outs[DS5_ISO_OUT_Q]={}, ins[DS5_ISO_IN_Q]={};
      struct ds5_slot slot={.ep_iso_out=&out,.ep_iso_in=&in};
      for (int i=0;i<DS5_ISO_OUT_Q;++i) { outs[i].buf=outs[i].storage; slot.req_iso_out[i]=&outs[i]; }
      for (int i=0;i<DS5_ISO_IN_Q;++i) { ins[i].buf=ins[i].storage; slot.req_iso_in[i]=&ins[i]; }
      failure=fail; queue_calls=enables=disables=acknowledgements=0;
      int result=set_interface(&slot,direction,1);
      check(result==(fail==0 ? -EIO : -ENOMEM) && acknowledgements==0,
            "SET_INTERFACE returns the endpoint/queue error without acknowledging success");
      check(slot.alt1==0 && slot.alt2==0 && !slot.iso_out_enabled && !slot.iso_in_enabled,
            "failed setup does not publish an active alternate setting");
      check(!out.enabled && !in.enabled && out.queued==0 && in.queued==0,
            "failed setup disables its endpoint and retires any partially queued requests");
      failure=-1; queue_calls=0;
      result=set_interface(&slot,direction,1);
      struct usb_ep *ep=direction==1 ? &out : &in;
      check(result==0 && ep->enabled && ep->queued==depth && queue_calls==depth,
            "a retry after failure queues the complete audio pipeline");
      check((direction==1 ? slot.alt1 : slot.alt2)==1 &&
            (direction==1 ? slot.iso_out_enabled : slot.iso_in_enabled),
            "successful setup publishes its alternate setting");
      queue_calls=0; result=set_interface(&slot,direction,1);
      check(result==0 && queue_calls==0 && ep->queued==depth,
            "repeating a successful setup does not queue requests twice");
      result=set_interface(&slot,direction,0);
      check(result==0 && !ep->enabled && ep->queued==0 &&
            (direction==1 ? slot.alt1 : slot.alt2)==0,
            "alternate setting zero still disables and drains the audio endpoint");
    }
  }
  printf("%d checks; %d failures\n",checks,failures);
  return failures!=0;
}
'''
code = code.replace('@@CONSTANTS@@', '\n'.join(re.findall(r'^#define DS5_ISO_(?:OUT|IN)_(?:MAXP|Q) .+$', source, re.M)))
code = code.replace('@@METHODS@@', '\n'.join(block('static ' + name) for name in [
    'int ds5_enable_iso_out(', 'void ds5_disable_iso_out(', 'int ds5_enable_iso_in(', 'void ds5_disable_iso_in(']))
code = code.replace('@@SET_INTERFACE@@', block('\tif (req == USB_REQ_SET_INTERFACE'))
with tempfile.TemporaryDirectory() as temp:
    work = Path(temp)
    (work / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')], env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0')).returncode)
