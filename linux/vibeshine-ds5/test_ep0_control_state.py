#!/usr/bin/env python3
"""Run production EP0 setup, queue helper and completion with USB boundary fakes.

Inject a rejected queue, then complete unrelated IN/HID requests on the reused
request object. No kernel/device is loaded. --baseline extracts HEAD.
"""
from pathlib import Path
import os
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
#include <linux/usb/ch9.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
#define GFP_ATOMIC 0
#define min_t(type,a,b) ((type)(a)<(type)(b)?(type)(a):(type)(b))
#define BUILD_BUG_ON(test) _Static_assert(!(test), "invalid fixture descriptor size")
#define DS5_HID_DESC_OFFSET 0
#define le16_to_cpu(x) (x)
struct usb_ep { int unused; };
struct usb_request {
  void *buf, *context; unsigned int length, actual; int status, zero;
  void (*complete)(struct usb_ep *,struct usb_request *);
};
struct ds5_slot;
struct usb_gadget { struct usb_ep *ep0; struct ds5_slot *slot; };
struct ds5_slot {
  struct usb_gadget *gadget; struct usb_request *ep0_req;
  u8 config, alt1, alt2, mute, ep0_pending_cs; s16 volume;
};
static struct ds5_slot *gadget_to_slot(struct usb_gadget *g) { return g->slot; }
static const u8 ds5_device_desc[18]={18,1}, ds5_config_desc[32]={9,2}, ds5_report_desc[8]={1};
static void ds5_disable_iso_out(struct ds5_slot *s) { (void)s; }
static void ds5_disable_iso_in(struct ds5_slot *s) { (void)s; }
static void ds5_disable_hid(struct ds5_slot *s) { (void)s; }
static int ds5_enable_iso_out(struct ds5_slot *s) { (void)s; return 0; }
static int ds5_enable_iso_in(struct ds5_slot *s) { (void)s; return 0; }
static int ds5_enable_hid(struct ds5_slot *s) { (void)s; return 0; }
static int ds5_get_feature(struct ds5_slot *s,u8 report,u8 *buf,unsigned int cap) {
  (void)s; (void)report; (void)cap; buf[0]=42; return 1;
}
static u16 get_unaligned_le16(const void *p) { const u8 *b=p; return b[0]|((u16)b[1]<<8); }
static void put_unaligned_le16(u16 v,void *p) { u8 *b=p; b[0]=v; b[1]=v>>8; }
static int queue_error;
static struct usb_request *queued;
// A queue error does not transfer ownership and has no completion callback.
static int usb_ep_queue(struct usb_ep *ep,struct usb_request *req,int flags) {
  (void)ep; (void)flags;
  if(queue_error)return queue_error;
  if(queued)return -EINVAL;  // a UDC refuses a request that is still linked
  req->status=-EINPROGRESS; req->actual=0; queued=req; return 0;
}
@@METHODS@@
static int checks, failures;
static void check(bool ok,const char *name) {
  ++checks; failures+=!ok; printf("%s %s\n",ok?"PASS":"FAIL",name);
}
static void complete(struct usb_ep *ep,int status,unsigned int actual) {
  struct usb_request *r=queued; queued=NULL;
  if(!r){ ++checks; ++failures; printf("FAIL completion with nothing queued\n"); return; }
  r->status=status; r->actual=actual; r->complete(ep,r);
}
static int setup(struct usb_gadget *g,u8 type,u8 request,u16 value,u16 index,u16 length) {
  struct usb_ctrlrequest c={.bRequestType=type,.bRequest=request,.wValue=value,.wIndex=index,.wLength=length};
  return ds5_setup(g,&c);
}
int main(void) {
  struct usb_ep ep={}; u8 data[512]={};
  struct usb_request req={.buf=data,.complete=ds5_ep0_complete};
  struct usb_gadget gadget={.ep0=&ep};
  struct ds5_slot slot={.gadget=&gadget,.ep0_req=&req,.config=1,.volume=-512};
  gadget.slot=&slot; req.context=&slot;
  for(int selector=1;selector<=2;++selector) {
    slot.mute=0; slot.volume=-512; queue_error=-ESHUTDOWN;
    check(setup(&gadget,0x21,1,selector<<8,0,selector)==-ESHUTDOWN && !queued,
          "failed SET_CUR does not transfer request ownership");
    check(slot.ep0_pending_cs==0,"failed SET_CUR leaves no pending audio selector");
    queue_error=0;
    check(setup(&gadget,0xa1,0x82,0x200,0,2)==0,"next volume GET_MIN can use the request");
    complete(&ep,0,2);
    check(slot.mute==0 && slot.volume==-512,"an IN response cannot apply a failed SET_CUR");
    slot.mute=0; slot.volume=-512;
    queue_error=-ESHUTDOWN;
    setup(&gadget,0x21,1,selector<<8,0,selector);
    queue_error=0;
    check(setup(&gadget,0x21,9,0x200,3,2)==0,"HID SET_REPORT can follow a failed audio request");
    data[0]=1; data[1]=0; complete(&ep,0,2);
    check(slot.mute==0 && slot.volume==-512,"HID output bytes cannot mutate audio controls");
    slot.ep0_pending_cs=0;
  }
  check(setup(&gadget,0x21,1,0x200,0,2)==0,"valid volume SET_CUR queues normally");
  put_unaligned_le16((u16)-1024,data); complete(&ep,0,2);
  check(slot.volume==-1024 && slot.ep0_pending_cs==0,"successful volume completion applies exactly once");
  check(setup(&gadget,0xa1,0x81,0x200,0,2)==0 && (s16)get_unaligned_le16(data)==-1024,
        "GET_CUR returns the newly accepted volume");
  complete(&ep,0,2);
  setup(&gadget,0x21,1,0x100,0,1); data[0]=1; complete(&ep,0,1);
  check(slot.mute==1 && slot.ep0_pending_cs==0,"successful mute completion still applies");
  setup(&gadget,0x21,1,0x200,0,2); put_unaligned_le16(0,data); complete(&ep,-ECONNRESET,2);
  check(slot.volume==-1024 && slot.ep0_pending_cs==0,"cancelled completion clears state without applying bytes");
  setup(&gadget,0x21,1,0x200,0,2); complete(&ep,0,1);
  check(slot.volume==-1024 && slot.ep0_pending_cs==0,"short volume completion cannot change the control");
  // #37: both OUT data stages stall when the ep0 request is gone (pre-#37 dereferenced NULL).
  slot.ep0_req=NULL; queued=NULL;
  check(setup(&gadget,0x21,1,0x100,0,1)==-ESHUTDOWN && !queued && slot.ep0_pending_cs==0,
        "a UAC SET_CUR without an ep0 request stalls and arms nothing");
  check(setup(&gadget,0x21,9,0x205,3,64)==-ESHUTDOWN && !queued,"a HID SET_REPORT without an ep0 request stalls");
  slot.ep0_req=&req;
  // D31-02: the selector belongs to the request on the wire, not to the slot's history.
  slot.ep0_pending_cs=2;
  check(setup(&gadget,0xa1,0x81,0x200,0,2)==0 && slot.ep0_pending_cs==0,"an IN reply clears a selector no completion retired");
  put_unaligned_le16((u16)-2048,data); complete(&ep,0,2);
  check(slot.volume==-1024,"...and its bytes are not applied as volume");
  slot.ep0_pending_cs=1; slot.mute=0;
  check(setup(&gadget,0x21,9,0x205,3,2)==0 && slot.ep0_pending_cs==0,"a HID SET_REPORT clears a stale audio selector");
  data[0]=1; complete(&ep,0,2);
  check(slot.mute==0,"...and its bytes are not applied as mute");
  printf("%d checks; %d failures\n",checks,failures);
  return failures!=0;
}
'''
code = code.replace('@@METHODS@@', '\n'.join(block(x) for x in [
    'static int ds5_queue_ep0(', 'static void ds5_ep0_complete(', 'static int ds5_utf16(', 'static int ds5_setup(']))
with tempfile.TemporaryDirectory() as temp:
    work = Path(temp)
    (work / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')], env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0')).returncode)
