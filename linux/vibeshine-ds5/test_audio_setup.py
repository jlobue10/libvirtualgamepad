#!/usr/bin/env python3
"""Exercise production DualSense audio endpoint setup with injected USB failures.

Compiles the real audio enable/disable functions and SET_INTERFACE/CONFIGURATION
branches. USB endpoint operations, HID lifecycle and EP0 acknowledgement are
boundary fakes; no kernel is loaded.
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
#define USB_REQ_SET_CONFIGURATION 9
#define USB_RECIP_INTERFACE 1
@@CONSTANTS@@
struct usb_request;
struct usb_ep { void *driver_data; const void *desc; bool enabled; int queued; struct usb_request *list[64]; };
struct usb_request {
  unsigned int length, actual; int status; bool linked;
  unsigned char storage[DS5_ISO_OUT_MAXP]; void *buf;
  void (*complete)(struct usb_ep *ep, struct usb_request *req);
};
struct ds5_slot {
  struct usb_ep *ep_iso_out, *ep_iso_in;
  struct usb_request *req_iso_out[DS5_ISO_OUT_Q], *req_iso_in[DS5_ISO_IN_Q];
  bool iso_out_enabled, iso_in_enabled;
  u8 alt1, alt2, config, ep0_pending_cs;
  bool hid_enabled;
};
static int ds5_ep_iso_out_desc, ds5_ep_iso_in_desc;
static int failure, queue_calls, enables, disables, acknowledgements, pcm_events, invalid_queues;
#define VIBESHINE_DS5_EVENT_PCM 2
static void ds5_push_event(struct ds5_slot *slot, u8 type, const void *data, unsigned int len) {
  (void)slot; (void)data; (void)len; if (type==VIBESHINE_DS5_EVENT_PCM) ++pcm_events;
}
// dummy_enable refuses an already-enabled endpoint; the production enable sets desc first.
static int usb_ep_enable(struct usb_ep *ep) {
  ++enables; if (ep->enabled || !ep->desc) { ++invalid_queues; return -EINVAL; }
  if (failure==0) return -EIO;
  ep->enabled=true; return 0;
}
// Modelled on vibeshine_ds5_udc.c: a linked request or a missing descriptor is -EINVAL, a
// disabled endpoint -ESHUTDOWN; the injected failure applies only to a request the real
// UDC would have accepted, so a double queue is always counted.
static int usb_ep_queue(struct usb_ep *ep, struct usb_request *req, int flags) {
  (void)flags;
  ++queue_calls;
  if (req->linked || !ep->desc) { ++invalid_queues; return -EINVAL; }
  if (!ep->enabled) return -ESHUTDOWN;
  if (queue_calls==failure) return -ENOMEM;
  req->linked=true; ep->list[ep->queued++]=req; return 0;
}
// Completing a request unlinks it first; the handler may requeue it (so the list is
// snapshotted before the completions run).
static void give_back_all(struct usb_ep *ep, int status) {
  struct usb_request *batch[64]; int n=ep->queued;
  memcpy(batch, ep->list, sizeof(batch[0])*(size_t)n); ep->queued=0;
  for (int i=0;i<n;++i) { batch[i]->linked=false; batch[i]->status=status; batch[i]->actual=0; if (batch[i]->complete) batch[i]->complete(ep, batch[i]); }
}
static void usb_ep_disable(struct usb_ep *ep) {
  ++disables; ep->enabled=false; ep->desc=NULL;   // dummy_disable clears desc too
  give_back_all(ep, -ESHUTDOWN);   // the real UDC nukes synchronously with -ESHUTDOWN
}
static void usb_ep_dequeue(struct usb_ep *ep, struct usb_request *req) {
  for (int i=0;i<ep->queued;++i) if (ep->list[i]==req) {
    for (int j=i+1;j<ep->queued;++j) ep->list[j-1]=ep->list[j];
    --ep->queued; req->linked=false; req->status=-ECONNRESET; req->actual=0;
    if (req->complete) req->complete(ep, req);   // the production handler requeues on this
    return;
  }
}
@@HANDLERS@@
static int ds5_queue_ep0(struct ds5_slot *slot, const void *buf, int length, int requested) {
  (void)slot; (void)buf; (void)length; (void)requested; ++acknowledgements; return 0;
}
static int hid_enables, hid_disables;
static int ds5_enable_hid(struct ds5_slot *slot) { ++hid_enables; slot->hid_enabled=true; return 0; }
static void ds5_disable_hid(struct ds5_slot *slot) { ++hid_disables; slot->hid_enabled=false; }
@@METHODS@@
static int set_interface(struct ds5_slot *slot, u16 wIndex, u16 wValue) {
  int n; u8 req=USB_REQ_SET_INTERFACE, type=USB_RECIP_INTERFACE; u16 wLength=0;
  @@SET_INTERFACE@@
  return -EINVAL;
}
static int set_configuration(struct ds5_slot *slot, u16 wValue) {
  int n; u8 req=USB_REQ_SET_CONFIGURATION, type=0; u16 wIndex=0, wLength=0;
  @@SET_CONFIGURATION@@
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
      struct ds5_slot slot={.ep_iso_out=&out,.ep_iso_in=&in,.config=1};
      for (int i=0;i<DS5_ISO_OUT_Q;++i) { outs[i].buf=outs[i].storage; outs[i].complete=ds5_iso_out_complete; slot.req_iso_out[i]=&outs[i]; }
      for (int i=0;i<DS5_ISO_IN_Q;++i) { ins[i].buf=ins[i].storage; ins[i].complete=ds5_iso_in_complete; slot.req_iso_in[i]=&ins[i]; }
      invalid_queues=0;
      failure=fail; queue_calls=enables=disables=acknowledgements=0;
      int result=set_interface(&slot,direction,1);
      check(result==(fail==0 ? -EIO : -ENOMEM) && acknowledgements==0,
            "SET_INTERFACE returns the endpoint/queue error without acknowledging success");
      check(slot.alt1==0 && slot.alt2==0 && !slot.iso_out_enabled && !slot.iso_in_enabled,
            "failed setup does not publish an active alternate setting");
      check(!out.enabled && !in.enabled && out.queued==0 && in.queued==0 && disables==(fail==0 ? 0 : 1),
            "failed setup disables its endpoint (once, and only when it was enabled) and retires any partially queued requests");
      bool none_linked=true;
      for (int i=0;i<DS5_ISO_OUT_Q;++i) none_linked&=!outs[i].linked;
      for (int i=0;i<DS5_ISO_IN_Q;++i) none_linked&=!ins[i].linked;
      check(none_linked, "no request stays linked after a failed setup (the handlers do not requeue on -ESHUTDOWN)");
      failure=-1; queue_calls=0;
      result=set_interface(&slot,direction,1);
      struct usb_ep *ep=direction==1 ? &out : &in;
      check(result==0 && ep->enabled && ep->queued==depth && queue_calls==depth && enables==2,
            "a retry after failure re-enables once and queues the complete audio pipeline");
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
      check(invalid_queues==0, "no request was ever queued while still linked");
    }
  }
  {
    struct usb_ep out={}, in={};
    struct usb_request outs[DS5_ISO_OUT_Q]={}, ins[DS5_ISO_IN_Q]={};
    struct ds5_slot slot={.ep_iso_out=&out,.ep_iso_in=&in};
    for(int i=0;i<DS5_ISO_OUT_Q;++i) { outs[i].buf=outs[i].storage; outs[i].complete=ds5_iso_out_complete; slot.req_iso_out[i]=&outs[i]; }
    for(int i=0;i<DS5_ISO_IN_Q;++i) { ins[i].buf=ins[i].storage; ins[i].complete=ds5_iso_in_complete; slot.req_iso_in[i]=&ins[i]; }
    failure=-1; invalid_queues=queue_calls=enables=disables=acknowledgements=0;
    check(set_interface(&slot,1,1)<0 && !out.enabled && acknowledgements==0,
          "SET_INTERFACE before configuration is rejected without enabling audio");
    ds5_disable_iso_out(&slot); slot.alt1=0;
    check(set_configuration(&slot,1)==0 && slot.config==1 && slot.hid_enabled,
          "the advertised configuration enables HID");
    check(set_interface(&slot,1,1)==0 && set_interface(&slot,2,1)==0 && out.queued==DS5_ISO_OUT_Q && in.queued==DS5_ISO_IN_Q,
          "configured playback and capture can both stream");
    for (unsigned int value=2; value<=3; ++value) {
      int before=acknowledgements;
      check(set_configuration(&slot,value)<0 && slot.config==1 && slot.alt1==1 && slot.alt2==1 && acknowledgements==before,
            "an unsupported configuration cannot change the active device");
    }
    slot.config=1;  // restore the baseline's invalid value to continue independent checks
    for (unsigned int intf=0; intf<4; ++intf) {
      int before=acknowledgements;
      check(set_interface(&slot,intf,2)<0 && acknowledgements==before,
            "an alternate setting absent from the descriptor is rejected");
    }
    slot.alt1=slot.alt2=1;
    int before=acknowledgements;
    check(set_interface(&slot,0x101,0)<0 && slot.alt1==1 && out.enabled && acknowledgements==before,
          "a high-byte interface alias cannot stop active playback");
    // Baseline may have stopped playback; re-enable it before testing reconfiguration.
    set_interface(&slot,1,1);
    check(set_configuration(&slot,1)==0 && slot.config==1 && slot.alt1==0 && slot.alt2==0 &&
          !out.enabled && !in.enabled && out.queued==0 && in.queued==0 && slot.hid_enabled,
          "reapplying the configuration retires both audio queues and resets alternate settings");
    check(set_interface(&slot,1,1)==0 && set_interface(&slot,2,1)==0,
          "audio can start again after reconfiguration");
    before=acknowledgements;
    check(set_configuration(&slot,0x100)<0 && slot.config==1 && out.enabled && in.enabled && acknowledgements==before,
          "a high-byte configuration alias cannot unconfigure the device");
    check(set_configuration(&slot,0)==0 && slot.config==0 && !slot.hid_enabled && !out.enabled && !in.enabled &&
          out.queued==0 && in.queued==0 && slot.alt1==0 && slot.alt2==0,
          "configuration zero cleanly stops every endpoint");
    check(invalid_queues==0, "configuration transitions never queue an already-linked audio request");
  }
  printf("%d checks; %d failures\n",checks,failures);
  return failures!=0;
}
'''
code = code.replace('@@CONSTANTS@@', '\n'.join(re.findall(r'^#define DS5_ISO_(?:OUT|IN)_(?:MAXP|Q) .+$', source, re.M)))
code = code.replace('@@METHODS@@', '\n'.join(block('static ' + name) for name in [
    'int ds5_enable_iso_out(', 'void ds5_disable_iso_out(', 'int ds5_enable_iso_in(', 'void ds5_disable_iso_in(']))
code = code.replace('@@SET_CONFIGURATION@@', block('\tif (req == USB_REQ_SET_CONFIGURATION'))
code = code.replace('@@SET_INTERFACE@@', block('\tif (req == USB_REQ_SET_INTERFACE'))
code = code.replace('@@HANDLERS@@', '\n'.join(block('static void ' + name) for name in ['ds5_iso_out_complete(', 'ds5_iso_in_complete(']))
with tempfile.TemporaryDirectory() as temp:
    work = Path(temp)
    (work / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')], env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0')).returncode)
