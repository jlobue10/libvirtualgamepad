#!/usr/bin/env python3
"""Run production gadget file operations against fake USB/FIFO boundaries.

Exercises overlapping CREATE calls, a blocked read across same-fd slot reuse,
and stale operations after unbind/rebind. No kernel module or USB hardware is
loaded. --baseline extracts production operations from HEAD.
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


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vibeshine_ds5_uapi.h"
#define __user
#define GFP_KERNEL 0
#define POLLWRNORM 0x100
#define POLLRDNORM 0x040
#include <poll.h>
typedef unsigned int __poll_t;
typedef int poll_table;
typedef uint8_t u8;
typedef uint32_t u32;
struct inode { int unused; };
struct file { void *private_data; int f_flags; };
struct mutex { pthread_mutex_t native; };
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->native, NULL)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->native)); }
struct usb_gadget { int disconnects; };
struct usb_request { void *buf; unsigned int length; };
#define FAKE_FIFO_DEPTH 16
struct fifo { struct vibeshine_ds5_event event[FAKE_FIFO_DEPTH]; unsigned head, tail; };
struct ds5_slot {
  struct mutex lock;
  struct usb_gadget *gadget;
  struct file *owner;
  u32 generation;
  bool created, hid_enabled, int_in_busy;
  struct usb_request *req_int_in;
  void *ep_int_in;
  int wait, evlock;
  struct fifo events;
  struct vibeshine_ds5_event output;
  bool output_valid;
  u32 pcm_dropped;
  u8 input[VIBESHINE_DS5_INPUT_SIZE], mac[6];
};
static struct ds5_slot ds5_slots[VIBESHINE_DS5_SLOTS];
static struct usb_gadget gadgets[VIBESHINE_DS5_SLOTS];
static pthread_mutex_t scheduling = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool pause_first_create, at_first_slot, resume_first;
static void mutex_lock(struct mutex *m) {
  pthread_mutex_lock(&scheduling);
  if (pause_first_create && m == &ds5_slots[0].lock) {
    pause_first_create = false; at_first_slot = true; pthread_cond_broadcast(&changed);
    while (!resume_first) pthread_cond_wait(&changed, &scheduling);
  }
  pthread_mutex_unlock(&scheduling);
  assert(!pthread_mutex_lock(&m->native));
}
static void *kzalloc(size_t size, int flags) { return calloc(1, size); }
#define kfree free
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define smp_load_acquire(x) (*(x))
#define smp_store_release(x, value) (*(x) = (value))
#define spin_lock_irqsave(lock, flags) ((void)(lock), (void)(flags))
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define INIT_KFIFO(f) ((f).head = (f).tail = 0)
#define kfifo_len(f) ((f)->tail - (f)->head)
static bool kfifo_get(struct fifo *f, struct vibeshine_ds5_event *event) {
  if (f->head == f->tail) return false;
  *event = f->event[f->head++ % FAKE_FIFO_DEPTH]; return true;
}
// A full kfifo refuses the offered element (drop-newest), as the kernel's does.
static bool kfifo_put(struct fifo *f, struct vibeshine_ds5_event ev) {
  if (f->tail - f->head == FAKE_FIFO_DEPTH) return false;
  f->event[f->tail++ % FAKE_FIFO_DEPTH] = ev; return true;
}
#define wake_up_interruptible(q) ((void)(q))
static int copy_from_user(void *dst, const void *src, size_t size) { memcpy(dst, src, size); return 0; }
#define copy_to_user copy_from_user
static void usb_gadget_connect(struct usb_gadget *g) { (void)g; }
static void usb_gadget_disconnect(struct usb_gadget *g) { ++g->disconnects; }
static int usb_ep_queue(void *ep, struct usb_request *r, int flags) { return 0; }
#define wake_up_interruptible_all(q) ((void)(q))
#define poll_wait(file, queue, wait) ((void)(file), (void)(queue), (void)(wait))
static void (*wait_hook)(void);
static int fake_wait(bool ready) { if (!ready && wait_hook) wait_hook(); return 0; }
#define wait_event_interruptible(queue, condition) fake_wait(condition)
'''
suffix = r'''
static int checks, failures;
static void check(bool ok, const char *name) {
  ++checks; failures += !ok; printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static int create(struct file *file, unsigned int slot) {
  struct vibeshine_ds5_create request = {.slot = slot};
  return ds5_ioctl(file, VIBESHINE_DS5_CREATE, (unsigned long)&request);
}
static void event(unsigned int slot, u8 marker) {
  struct vibeshine_ds5_event ev = {.type = VIBESHINE_DS5_EVENT_PCM, .size = 1};
  ev.data[0] = marker;
  assert(kfifo_put(&ds5_slots[slot].events, ev));
}
static struct file *reused_file;
static void reuse_same_fd(void) {
  assert(!ds5_ioctl(reused_file, VIBESHINE_DS5_DESTROY, 0));
  assert(!create(reused_file, 0));
  event(0, 99);
}
// First wake: nothing queued (a sibling reader on the same fd drained it); second wake: an event.
static int sleeps;
static void spurious_then_event(void) {
  if (++sleeps == 2) event(0, 42);
}
struct create_job { struct file *file; unsigned int slot; int result; bool done; };
static void *create_thread(void *arg) {
  struct create_job *job = arg;
  job->result = create(job->file, job->slot);
  pthread_mutex_lock(&scheduling); job->done = true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&scheduling);
  return NULL;
}
int main(void) {
  for (int i = 0; i < VIBESHINE_DS5_SLOTS; ++i) {
    mutex_init(&ds5_slots[i].lock); ds5_slots[i].gadget = &gadgets[i];
  }
  struct file file = {}, other = {};
  assert(!ds5_open(NULL, &file)); assert(!ds5_open(NULL, &other));
  assert(!create(&file, 0));
  struct vibeshine_ds5_event received = {};
  u8 input[VIBESHINE_DS5_INPUT_SIZE] = {1, 2, 3};
  check(ds5_write(&file, (const char *)input, sizeof(input), NULL) == sizeof(input) &&
        !memcmp(ds5_slots[0].input, input, sizeof(input)), "the current owner can still submit input");
  check(create(&file, 1) == -EBUSY, "a second sequential CREATE on one fd is rejected");
  event(0, 7);
  check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == sizeof(received) && received.data[0] == 7,
        "the current owner receives its queued feedback");
  file.f_flags = O_NONBLOCK;
  check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == -EAGAIN, "an empty nonblocking read returns EAGAIN");
  // D32-01: audio pressure never evicts a HID output report, and refused PCM packets are counted.
  {
    u8 pcm[4] = {0}, report[4] = {2, 0xaa};
    for (int i = 0; i < FAKE_FIFO_DEPTH + 1; ++i) { pcm[0] = (u8)i; ds5_push_event(&ds5_slots[0], VIBESHINE_DS5_EVENT_PCM, pcm, sizeof(pcm)); }
    ds5_push_event(&ds5_slots[0], VIBESHINE_DS5_EVENT_OUTPUT, report, sizeof(report));
    check(ds5_slots[0].pcm_dropped == 1 && ds5_slots[0].output_valid, "a full PCM FIFO refuses the packet and counts it, the output report is kept");
    check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == sizeof(received) &&
          received.type == VIBESHINE_DS5_EVENT_OUTPUT && received.data[1] == 0xaa,
          "the output report is read ahead of the queued PCM packets");
    int drained = 0;
    while (ds5_read(&file, (char *)&received, sizeof(received), NULL) == sizeof(received)) { ++drained; }
    check(drained == FAKE_FIFO_DEPTH && received.type == VIBESHINE_DS5_EVENT_PCM, "the PCM packets that fit are read in order afterwards");
    ds5_push_event(&ds5_slots[0], VIBESHINE_DS5_EVENT_OUTPUT, report, sizeof(report));
    report[1] = 0xbb;
    ds5_push_event(&ds5_slots[0], VIBESHINE_DS5_EVENT_OUTPUT, report, sizeof(report));
    check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == sizeof(received) && received.data[1] == 0xbb &&
          ds5_read(&file, (char *)&received, sizeof(received), NULL) == -EAGAIN,
          "a newer output report replaces an unread one (latest wins)");
  }
  file.f_flags = 0;
  reused_file = &file; wait_hook = reuse_same_fd;
  check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == -ENODEV,
        "a blocked read cannot become a reader of a new generation on the same fd");
  wait_hook = NULL;
  file.f_flags = O_NONBLOCK;
  check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == sizeof(received) && received.data[0] == 99,
        "the replacement generation keeps its feedback for a new read");
  // D25-03: a blocking read woken without an event goes back to sleep instead of
  // returning EAGAIN (or spinning); a nonblocking read reports it in one pass.
  file.f_flags = 0; sleeps = 0; wait_hook = spurious_then_event;
  check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == sizeof(received) && received.data[0] == 42 && sleeps == 2,
        "a blocking read drained by a sibling waits for the next event");
  wait_hook = NULL;
  file.f_flags = O_NONBLOCK;
  check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == -EAGAIN, "a nonblocking read drained by a sibling returns EAGAIN at once");
  // D25-04: a slot whose gadget bind failed (or has not happened) cannot be created.
  struct file unbound = {};
  assert(!ds5_open(NULL, &unbound));
  ds5_slots[2].gadget = NULL;
  check(create(&unbound, 2) == -ENODEV && !ds5_slots[2].created && ((struct ds5_file *)unbound.private_data)->slot == NULL,
        "CREATE on a slot without a bound gadget is refused without reserving it");
  ds5_slots[2].gadget = &gadgets[2];
  ds5_release(NULL, &unbound);

  // Unbind retires a slot without changing the old file's private state. Rebind
  // can then give the same slot to another file before the old file closes.
  ds5_slots[0].owner = NULL; ds5_slots[0].created = false;
  assert(!create(&other, 0)); event(0, 55);
  check(ds5_read(&file, (char *)&received, sizeof(received), NULL) == -ENODEV,
        "a stale nonblocking fd cannot consume another owner's feedback");
  check(ds5_write(&file, (const char *)input, sizeof(input), NULL) == -ENODEV,
        "a stale fd cannot submit input to the replacement owner");
  check(ds5_poll(&file, NULL) & POLLHUP, "a stale fd reports a hangup");
  ds5_release(NULL, &file);
  check(ds5_slots[0].created && ds5_slots[0].owner == &other,
        "closing an unbound fd cannot disconnect the replacement owner");
  ds5_release(NULL, &other);

  struct file destroyer = {}, replacement = {};
  assert(!ds5_open(NULL, &destroyer)); assert(!ds5_open(NULL, &replacement));
  assert(!create(&destroyer, 0));
  ds5_slots[0].owner = NULL; ds5_slots[0].created = false;
  assert(!create(&replacement, 0));
  check(ds5_ioctl(&destroyer, VIBESHINE_DS5_DESTROY, 0) == -ENODEV &&
        ds5_slots[0].created && ds5_slots[0].owner == &replacement,
        "DESTROY on an unbound fd cannot destroy the replacement owner's slot");
  ds5_release(NULL, &destroyer); ds5_release(NULL, &replacement);

  struct file shared = {};
  assert(!ds5_open(NULL, &shared));
  struct create_job first = {.file=&shared, .slot=0}, second = {.file=&shared, .slot=1};
  pause_first_create = true;
  pthread_t a, b;
  pthread_create(&a, NULL, create_thread, &first);
  pthread_mutex_lock(&scheduling);
  while (!at_first_slot) pthread_cond_wait(&changed, &scheduling);
  pthread_mutex_unlock(&scheduling);
  pthread_create(&b, NULL, create_thread, &second);
  pthread_mutex_lock(&scheduling);
  struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 1;
  while (!second.done && pthread_cond_timedwait(&changed, &scheduling, &deadline) != ETIMEDOUT) {}
  resume_first = true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&scheduling);
  pthread_join(a, NULL); pthread_join(b, NULL);
  check((first.result == 0 && second.result == -EBUSY) || (second.result == 0 && first.result == -EBUSY),
        "concurrent CREATEs on one fd cannot both reserve a slot");
  ds5_release(NULL, &shared);
  check(!ds5_slots[0].created && !ds5_slots[1].created, "close leaves no stranded slot from a concurrent CREATE");
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
'''
code = prefix
if 'struct ds5_file {' in source:
    code += block('struct ds5_file {') + ';\n'
for marker in ['static void ds5_push_event(', 'static int ds5_open(', 'static int ds5_release(', 'static long ds5_ioctl(',
               'static ssize_t ds5_write(', 'static ssize_t ds5_read(', 'static __poll_t ds5_poll(']:
    code += block(marker) + '\n'
code += suffix
with tempfile.TemporaryDirectory(prefix='ds5-lifecycle-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-pthread', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(ROOT / 'linux/vibeshine-ds5'),
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')], timeout=15,
                                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0')).returncode)
