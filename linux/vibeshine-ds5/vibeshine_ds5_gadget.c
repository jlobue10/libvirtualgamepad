// SPDX-License-Identifier: GPL-2.0-only
/*
 * DualSense composite gadget: HID (IF 3) plus UAC1 4-channel playback
 * (IF 0-2), matching a captured 054c:0ce6 USB device.
 */
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/wait.h>

#include "vibeshine_ds5.h"

#define DS5_ISO_OUT_MAXP 392
#define DS5_ISO_IN_MAXP 196
#define DS5_INT_MAXP 64
#define DS5_ISO_OUT_Q 8
#define DS5_ISO_IN_Q 4
/* Offset of the HID class descriptor (09 21 ...) inside ds5_config_desc: interface 3 at
 * 0xc3, its HID descriptor at 0xcc, endpoints at 0xd5/0xdc. Checked at module init. */
#define DS5_HID_DESC_OFFSET 0xcc
#define DS5_EVENT_FIFO 16

static const u8 ds5_report_desc[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
	0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06,
	0x00, 0xff, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
	0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x05,
	0x09, 0x19, 0x01, 0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02, 0x06,
	0x00, 0xff, 0x09, 0x21, 0x75, 0x01, 0x95, 0x0d, 0x81, 0x02, 0x09, 0x22, 0x15, 0x00, 0x26, 0xff,
	0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02, 0x85, 0x02, 0x09, 0x23, 0x95, 0x2f, 0x91, 0x02, 0x85,
	0x05, 0x09, 0x33, 0x95, 0x28, 0xb1, 0x02, 0x85, 0x08, 0x09, 0x34, 0x95, 0x2f, 0xb1, 0x02, 0x85,
	0x09, 0x09, 0x24, 0x95, 0x13, 0xb1, 0x02, 0x85, 0x0a, 0x09, 0x25, 0x95, 0x1a, 0xb1, 0x02, 0x85,
	0x20, 0x09, 0x26, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xb1, 0x02, 0x85,
	0x22, 0x09, 0x40, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x80, 0x09, 0x28, 0x95, 0x3f, 0xb1, 0x02, 0x85,
	0x81, 0x09, 0x29, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x82, 0x09, 0x2a, 0x95, 0x09, 0xb1, 0x02, 0x85,
	0x83, 0x09, 0x2b, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x84, 0x09, 0x2c, 0x95, 0x3f, 0xb1, 0x02, 0x85,
	0x85, 0x09, 0x2d, 0x95, 0x02, 0xb1, 0x02, 0x85, 0xa0, 0x09, 0x2e, 0x95, 0x01, 0xb1, 0x02, 0x85,
	0xe0, 0x09, 0x2f, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf0, 0x09, 0x30, 0x95, 0x3f, 0xb1, 0x02, 0x85,
	0xf1, 0x09, 0x31, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf2, 0x09, 0x32, 0x95, 0x0f, 0xb1, 0x02, 0x85,
	0xf4, 0x09, 0x35, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf5, 0x09, 0x36, 0x95, 0x03, 0xb1, 0x02, 0xc0,
};

static const u8 ds5_config_desc[] = {
	0x09, 0x02, 0xe3, 0x00, 0x04, 0x01, 0x00, 0xc0, 0xfa, 0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01,
	0x00, 0x00, 0x0a, 0x24, 0x01, 0x00, 0x01, 0x49, 0x00, 0x02, 0x01, 0x02, 0x0c, 0x24, 0x02, 0x01,
	0x01, 0x01, 0x06, 0x04, 0x33, 0x00, 0x00, 0x00, 0x0c, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x09, 0x24, 0x03, 0x03, 0x01, 0x03, 0x04, 0x02, 0x00, 0x0c, 0x24, 0x02,
	0x04, 0x02, 0x04, 0x03, 0x02, 0x03, 0x00, 0x00, 0x00, 0x09, 0x24, 0x06, 0x05, 0x04, 0x01, 0x03,
	0x00, 0x00, 0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x01, 0x05, 0x00, 0x09, 0x04, 0x01, 0x00, 0x00,
	0x01, 0x02, 0x00, 0x00, 0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00, 0x07, 0x24, 0x01,
	0x01, 0x01, 0x01, 0x00, 0x0b, 0x24, 0x02, 0x01, 0x04, 0x02, 0x10, 0x01, 0x80, 0xbb, 0x00, 0x09,
	0x05, 0x01, 0x09, 0x88, 0x01, 0x04, 0x00, 0x00, 0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09,
	0x04, 0x02, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x09, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00,
	0x00, 0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00, 0x0b, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01,
	0x80, 0xbb, 0x00, 0x09, 0x05, 0x82, 0x05, 0xc4, 0x00, 0x04, 0x00, 0x00, 0x07, 0x25, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x09, 0x04, 0x03, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x09, 0x21, 0x11, 0x01,
	0x00, 0x01, 0x22, sizeof(ds5_report_desc) & 0xff, sizeof(ds5_report_desc) >> 8,
	0x07, 0x05, 0x84, 0x03, 0x40, 0x00, 0x06, 0x07, 0x05, 0x03, 0x03, 0x40, 0x00, 0x06,
};

static const u8 ds5_calibration[] = {
	5, 0, 0, 0, 0, 0, 0,
	0, 0x20, 0, 0xe0, 0, 0x20, 0, 0xe0, 0, 0x20, 0, 0xe0,
	0, 2, 0, 2,
	0, 0x20, 0, 0xe0, 0, 0x20, 0, 0xe0, 0, 0x20, 0, 0xe0,
	0, 0, 0, 0, 0, 0
};

static const u8 ds5_firmware[] = {
	0x20,
	'J', 'u', 'n', ' ', '1', '9', ' ', '2', '0', '2', '3',
	'1', '4', ':', '4', '7', ':', '3', '4',
	0x03, 0x00, 0x44, 0x00, 0x08, 0x02, 0x00, 0x01,
	0x3e, 0x00, 0x00, 0x01,
	0xc1, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* UpdateVersion >= 0x0390 is required by 007 First Light's libScePad. */
	0x90, 0x03, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
	0x0b, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static struct usb_device_descriptor ds5_device_desc = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = cpu_to_le16(0x0200),
	.bMaxPacketSize0 = 64,
	.idVendor = cpu_to_le16(0x054c),
	.idProduct = cpu_to_le16(0x0ce6),
	.bcdDevice = cpu_to_le16(0x0100),
	.iManufacturer = 1,
	.iProduct = 2,
	.bNumConfigurations = 1,
};

static struct usb_endpoint_descriptor ds5_ep_int_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x84,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(DS5_INT_MAXP),
	.bInterval = 6,
};

static struct usb_endpoint_descriptor ds5_ep_int_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x03,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(DS5_INT_MAXP),
	.bInterval = 6,
};

static struct usb_endpoint_descriptor ds5_ep_iso_out_desc = {
	.bLength = USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x01,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ADAPTIVE,
	.wMaxPacketSize = cpu_to_le16(DS5_ISO_OUT_MAXP),
	.bInterval = 4,
};

static struct usb_endpoint_descriptor ds5_ep_iso_in_desc = {
	.bLength = USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x82,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	.wMaxPacketSize = cpu_to_le16(DS5_ISO_IN_MAXP),
	.bInterval = 4,
};

struct ds5_slot {
	struct mutex lock;
	struct usb_gadget *gadget;
	struct usb_request *ep0_req;
	struct usb_ep *ep_int_in;
	struct usb_ep *ep_int_out;
	struct usb_ep *ep_iso_out;
	struct usb_ep *ep_iso_in;
	struct usb_request *req_int_in;
	struct usb_request *req_int_out;
	struct usb_request *req_iso_out[DS5_ISO_OUT_Q];
	struct usb_request *req_iso_in[DS5_ISO_IN_Q];
	wait_queue_head_t wait;
	spinlock_t evlock;
	DECLARE_KFIFO(events, struct vibeshine_ds5_event, DS5_EVENT_FIFO);
	u8 mac[6];
	u8 input[VIBESHINE_DS5_INPUT_SIZE];
	u8 config;
	u8 alt1;
	u8 alt2;
	bool created;
	/* The fd that CREATEd this slot: only it may read, write or destroy it, and
	 * a reader blocked on the slot's wait queue is released when it goes away. */
	struct file *owner;
	u32 generation;
	bool hid_enabled;
	bool iso_out_enabled;
	bool iso_in_enabled;
	bool int_in_busy;
	s16 volume;
	u8 mute;
};

/* Serialize lifecycle and I/O admission on each open file. Slot locks alone do
 * not serialize concurrent CREATEs targeting different slots on the same fd. */
struct ds5_file {
	struct mutex lock;
	struct ds5_slot *slot;
};

static struct ds5_slot ds5_slots[VIBESHINE_DS5_SLOTS];
static char ds5_udc_names[VIBESHINE_DS5_SLOTS][32];
static char ds5_driver_names[VIBESHINE_DS5_SLOTS][32];
static struct usb_gadget_driver ds5_drivers[VIBESHINE_DS5_SLOTS];

static struct ds5_slot *gadget_to_slot(struct usb_gadget *gadget)
{
	int i;

	for (i = 0; i < VIBESHINE_DS5_SLOTS; i++) {
		if (ds5_slots[i].gadget == gadget)
			return &ds5_slots[i];
	}
	return NULL;
}

static void ds5_push_event(struct ds5_slot *slot, u32 type, const void *data, u32 size)
{
	struct vibeshine_ds5_event ev = {};
	unsigned long flags;

	if (size > sizeof(ev.data))
		size = sizeof(ev.data);
	ev.type = type;
	ev.size = size;
	if (data && size)
		memcpy(ev.data, data, size);
	spin_lock_irqsave(&slot->evlock, flags);
	kfifo_put(&slot->events, ev);
	spin_unlock_irqrestore(&slot->evlock, flags);
	wake_up_interruptible(&slot->wait);
}

static int ds5_queue_ep0(struct ds5_slot *slot, const void *data, unsigned int len, u16 wLength)
{
	struct usb_request *req = slot->ep0_req;

	if (!req)
		return -ESHUTDOWN;
	if (len > wLength)
		len = wLength;
	if (len && data)
		memcpy(req->buf, data, len);
	req->length = len;
	req->zero = len < wLength;
	req->status = 0;
	return usb_ep_queue(slot->gadget->ep0, req, GFP_ATOMIC);
}

static void ds5_fill_pairing(u8 *buf, const u8 *mac)
{
	int i;

	memset(buf, 0, 20);
	buf[0] = 0x09;
	/* The HID pairing report stores the Bluetooth address least byte first. */
	for (i = 0; i < 6; i++)
		buf[i + 1] = mac[5 - i];
	buf[7] = 0x08;
	buf[8] = 0x25;
	buf[9] = 0x00;
}

static int ds5_get_feature(struct ds5_slot *slot, u8 id, u8 *buf, unsigned int cap)
{
	unsigned int len = 0;

	switch (id) {
	case 0x05:
		len = sizeof(ds5_calibration);
		if (cap < len)
			return 0;
		memcpy(buf, ds5_calibration, len);
		return len;
	case 0x09:
		len = 20;
		if (cap < len)
			return 0;
		ds5_fill_pairing(buf, slot->mac);
		return len;
	case 0x20:
		len = sizeof(ds5_firmware);
		if (cap < len)
			return 0;
		memcpy(buf, ds5_firmware, len);
		return len;
	default:
		return 0;
	}
}

static struct usb_ep *ds5_find_ep(struct usb_gadget *gadget, const char *name)
{
	struct usb_ep *ep;

	list_for_each_entry(ep, &gadget->ep_list, ep_list) {
		if (ep->name && strcmp(ep->name, name) == 0)
			return ep;
	}
	return NULL;
}

static void ds5_int_in_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct ds5_slot *slot = ep->driver_data;

	if (!slot)
		return;
	/* Userspace publishes at 100 Hz. Let the next write queue the next
	 * report instead of recursively requeueing from an inline completion.
	 */
	smp_store_release(&slot->int_in_busy, false);
}

static void ds5_int_out_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct ds5_slot *slot = ep->driver_data;

	if (!slot || req->status == -ESHUTDOWN)
		return;
	if (req->status == 0 && req->actual)
		ds5_push_event(slot, VIBESHINE_DS5_EVENT_OUTPUT, req->buf, req->actual);
	req->length = DS5_INT_MAXP;
	usb_ep_queue(ep, req, GFP_ATOMIC);
}

static void ds5_iso_out_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct ds5_slot *slot = ep->driver_data;

	if (!slot || req->status == -ESHUTDOWN)
		return;
	if (req->status == 0 && req->actual)
		ds5_push_event(slot, VIBESHINE_DS5_EVENT_PCM, req->buf, req->actual);
	req->length = DS5_ISO_OUT_MAXP;
	usb_ep_queue(ep, req, GFP_ATOMIC);
}

static void ds5_iso_in_complete(struct usb_ep *ep, struct usb_request *req)
{
	if (req->status == -ESHUTDOWN)
		return;
	memset(req->buf, 0, DS5_ISO_IN_MAXP);
	req->length = DS5_ISO_IN_MAXP;
	usb_ep_queue(ep, req, GFP_ATOMIC);
}

static void ds5_ep0_complete(struct usb_ep *ep, struct usb_request *req)
{
}

static int ds5_enable_hid(struct ds5_slot *slot)
{
	int ret;

	if (slot->hid_enabled)
		return 0;
	/* A slot whose bind did not complete (or that is being unbound) has no requests. */
	if (!slot->req_int_in || !slot->req_int_out || !slot->ep_int_in || !slot->ep_int_out)
		return -ESHUTDOWN;
	slot->ep_int_in->driver_data = slot;
	slot->ep_int_out->driver_data = slot;
	slot->ep_int_in->desc = &ds5_ep_int_in_desc;
	slot->ep_int_out->desc = &ds5_ep_int_out_desc;
	ret = usb_ep_enable(slot->ep_int_in);
	if (ret)
		return ret;
	ret = usb_ep_enable(slot->ep_int_out);
	if (ret) {
		usb_ep_disable(slot->ep_int_in);
		return ret;
	}
	slot->req_int_out->length = DS5_INT_MAXP;
	ret = usb_ep_queue(slot->ep_int_out, slot->req_int_out, GFP_ATOMIC);
	if (ret) {
		usb_ep_disable(slot->ep_int_out);
		usb_ep_disable(slot->ep_int_in);
		return ret;
	}
	/* The first userspace report starts IN traffic after configuration. */
	smp_store_release(&slot->hid_enabled, true);
	return 0;
}

static void ds5_disable_hid(struct ds5_slot *slot)
{
	if (!slot->hid_enabled)
		return;
	/* Dequeue/disable can invoke completion synchronously as well. */
	slot->hid_enabled = false;
	usb_ep_dequeue(slot->ep_int_in, slot->req_int_in);
	usb_ep_dequeue(slot->ep_int_out, slot->req_int_out);
	usb_ep_disable(slot->ep_int_in);
	usb_ep_disable(slot->ep_int_out);
	slot->int_in_busy = false;
}

static int ds5_enable_iso_out(struct ds5_slot *slot)
{
	int i, ret;

	if (slot->iso_out_enabled)
		return 0;
	slot->ep_iso_out->driver_data = slot;
	slot->ep_iso_out->desc = &ds5_ep_iso_out_desc;
	ret = usb_ep_enable(slot->ep_iso_out);
	if (ret)
		return ret;
	for (i = 0; i < DS5_ISO_OUT_Q; i++) {
		slot->req_iso_out[i]->length = DS5_ISO_OUT_MAXP;
		ret = usb_ep_queue(slot->ep_iso_out, slot->req_iso_out[i], GFP_ATOMIC);
		if (ret) {
			/* Disable completes any earlier requests with -ESHUTDOWN. Leave
			 * setup unpublished so SET_INTERFACE can retry the whole queue. */
			usb_ep_disable(slot->ep_iso_out);
			return ret;
		}
	}
	slot->iso_out_enabled = true;
	return 0;
}

static void ds5_disable_iso_out(struct ds5_slot *slot)
{
	int i;

	if (!slot->iso_out_enabled)
		return;
	for (i = 0; i < DS5_ISO_OUT_Q; i++)
		usb_ep_dequeue(slot->ep_iso_out, slot->req_iso_out[i]);
	usb_ep_disable(slot->ep_iso_out);
	slot->iso_out_enabled = false;
}

static int ds5_enable_iso_in(struct ds5_slot *slot)
{
	int i, ret;

	if (slot->iso_in_enabled)
		return 0;
	slot->ep_iso_in->driver_data = slot;
	slot->ep_iso_in->desc = &ds5_ep_iso_in_desc;
	ret = usb_ep_enable(slot->ep_iso_in);
	if (ret)
		return ret;
	for (i = 0; i < DS5_ISO_IN_Q; i++) {
		memset(slot->req_iso_in[i]->buf, 0, DS5_ISO_IN_MAXP);
		slot->req_iso_in[i]->length = DS5_ISO_IN_MAXP;
		ret = usb_ep_queue(slot->ep_iso_in, slot->req_iso_in[i], GFP_ATOMIC);
		if (ret) {
			usb_ep_disable(slot->ep_iso_in);
			return ret;
		}
	}
	slot->iso_in_enabled = true;
	return 0;
}

static void ds5_disable_iso_in(struct ds5_slot *slot)
{
	int i;

	if (!slot->iso_in_enabled)
		return;
	for (i = 0; i < DS5_ISO_IN_Q; i++)
		usb_ep_dequeue(slot->ep_iso_in, slot->req_iso_in[i]);
	usb_ep_disable(slot->ep_iso_in);
	slot->iso_in_enabled = false;
}

static int ds5_utf16(const char *s, u8 *buf, unsigned int cap)
{
	unsigned int n = 0;
	unsigned int i;

	if (cap < 2)
		return 0;
	buf[1] = USB_DT_STRING;
	for (i = 0; s[i] && 2 + n + 2 <= cap; i++) {
		buf[2 + n] = s[i];
		buf[3 + n] = 0;
		n += 2;
	}
	buf[0] = 2 + n;
	return buf[0];
}

static int ds5_setup(struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
	struct ds5_slot *slot = gadget_to_slot(gadget);
	u16 wValue = le16_to_cpu(ctrl->wValue);
	u16 wIndex = le16_to_cpu(ctrl->wIndex);
	u16 wLength = le16_to_cpu(ctrl->wLength);
	u8 type = ctrl->bRequestType;
	u8 req = ctrl->bRequest;
	u8 buf[256];
	int n;

	if (!slot)
		return -ENODEV;

	if (req == USB_REQ_GET_DESCRIPTOR && (type & USB_DIR_IN)) {
		switch (wValue >> 8) {
		case USB_DT_DEVICE:
			return ds5_queue_ep0(slot, &ds5_device_desc, sizeof(ds5_device_desc), wLength);
		case USB_DT_CONFIG:
			return ds5_queue_ep0(slot, ds5_config_desc, sizeof(ds5_config_desc), wLength);
		case USB_DT_STRING:
			if ((wValue & 0xff) == 0) {
				buf[0] = 4;
				buf[1] = USB_DT_STRING;
				buf[2] = 0x09;
				buf[3] = 0x04;
				return ds5_queue_ep0(slot, buf, 4, wLength);
			}
			if ((wValue & 0xff) == 1)
				n = ds5_utf16("Sony Interactive Entertainment", buf, sizeof(buf));
			else if ((wValue & 0xff) == 2)
				n = ds5_utf16("DualSense Wireless Controller", buf, sizeof(buf));
			else
				return -EINVAL;
			return ds5_queue_ep0(slot, buf, n, wLength);
		case 0x22:
			if ((wIndex & 0xff) == 3)
				return ds5_queue_ep0(slot, ds5_report_desc, sizeof(ds5_report_desc), wLength);
			return -EINVAL;
		case 0x21:
			BUILD_BUG_ON(sizeof(ds5_config_desc) < DS5_HID_DESC_OFFSET + 9);
			return ds5_queue_ep0(slot, ds5_config_desc + DS5_HID_DESC_OFFSET, 9, wLength);
		default:
			return -EINVAL;
		}
	}

	if (req == USB_REQ_SET_CONFIGURATION && type == 0) {
		/* Only configuration 1 is advertised. Reject reserved/high bits before
		 * changing any endpoint state. Reapplying it resets every interface. */
		if (wValue > 1 || wIndex || wLength)
			return -EINVAL;
		ds5_disable_iso_out(slot);
		ds5_disable_iso_in(slot);
		ds5_disable_hid(slot);
		slot->alt1 = slot->alt2 = 0;
		slot->config = 0;
		if (wValue) {
			n = ds5_enable_hid(slot);
			if (n)
				return n;
			slot->config = 1;
		}
		return ds5_queue_ep0(slot, NULL, 0, 0);
	}

	if (req == USB_REQ_GET_CONFIGURATION && (type & USB_DIR_IN))
		return ds5_queue_ep0(slot, &slot->config, 1, wLength);

	if (req == USB_REQ_SET_INTERFACE && type == USB_RECIP_INTERFACE) {
		u16 alt = wValue;
		u16 intf = wIndex;

		/* Audio interfaces have alternates 0/1; control and HID only have 0. */
		if (slot->config != 1 || wLength || intf > 3 ||
		    alt > ((intf == 1 || intf == 2) ? 1 : 0))
			return -EINVAL;

		if (intf == 1) {
			if (alt) {
				n = ds5_enable_iso_out(slot);
				if (n)
					return n;
			} else {
				ds5_disable_iso_out(slot);
			}
			slot->alt1 = alt;
		} else if (intf == 2) {
			if (alt) {
				n = ds5_enable_iso_in(slot);
				if (n)
					return n;
			} else {
				ds5_disable_iso_in(slot);
			}
			slot->alt2 = alt;
		} else if (intf != 0 && intf != 3) {
			return -EINVAL;
		}
		return ds5_queue_ep0(slot, NULL, 0, 0);
	}

	if (req == USB_REQ_GET_INTERFACE && (type & USB_DIR_IN)) {
		u8 alt = 0;

		if ((wIndex & 0xff) == 1)
			alt = slot->alt1;
		else if ((wIndex & 0xff) == 2)
			alt = slot->alt2;
		return ds5_queue_ep0(slot, &alt, 1, wLength);
	}

	/* HID class GET/SET_REPORT on interface 3 */
	if ((wIndex & 0xff) == 3 && (type & USB_TYPE_MASK) == USB_TYPE_CLASS) {
		u8 report = wValue & 0xff;

		if (req == 0x01 && (type & USB_DIR_IN)) { /* GET_REPORT */
			n = ds5_get_feature(slot, report, buf, sizeof(buf));
			if (!n)
				return -EINVAL;
			return ds5_queue_ep0(slot, buf, n, wLength);
		}
		if (req == 0x09 && !(type & USB_DIR_IN)) { /* SET_REPORT: data stage then status */
			/* An IN request with these values must stall, not be answered with
			 * whatever ep0_req->buf last carried. */
			slot->ep0_req->length = min_t(u16, wLength, 64);
			slot->ep0_req->zero = 0;
			slot->ep0_req->complete = ds5_ep0_complete;
			return usb_ep_queue(gadget->ep0, slot->ep0_req, GFP_ATOMIC);
		}
		if (req == 0x0a && !(type & USB_DIR_IN)) /* SET_IDLE */
			return ds5_queue_ep0(slot, NULL, 0, 0);
		/* Any other HID-interface class request stalls here: it must not fall
		 * through to the audio-control branch below. */
		return -EOPNOTSUPP;
	}

	/* UAC SET_CUR / GET_CUR for mute and volume (audio interfaces 0-2 only) */
	if ((type & USB_TYPE_MASK) == USB_TYPE_CLASS && (type & USB_RECIP_MASK) == USB_RECIP_INTERFACE &&
	    (wIndex & 0xff) <= 2) {
		u8 cs = wValue >> 8;

		if ((type & USB_DIR_IN) && (req == 0x81 || req == 0x82 || req == 0x83 || req == 0x84)) {
			if (cs == 0x01) {
				buf[0] = (req == 0x81) ? slot->mute : 0;
				return ds5_queue_ep0(slot, buf, 1, wLength);
			}
			if (cs == 0x02) {
				s16 value = slot->volume;

				if (req == 0x82)
					value = -32768;
				else if (req == 0x83)
					value = 0;
				else if (req == 0x84)
					value = 256;
				put_unaligned_le16((__u16)value, buf);
				return ds5_queue_ep0(slot, buf, 2, wLength);
			}
		}
		if (req == 0x01 && !(type & USB_DIR_IN)) { /* SET_CUR: OUT data stage only */
			slot->ep0_req->length = min_t(u16, wLength, 8);
			return usb_ep_queue(gadget->ep0, slot->ep0_req, GFP_ATOMIC);
		}
	}

	return -EOPNOTSUPP;
}

static struct usb_request *ds5_alloc_req(struct usb_ep *ep, unsigned int size, void (*complete)(struct usb_ep *, struct usb_request *))
{
	struct usb_request *req = usb_ep_alloc_request(ep, GFP_KERNEL);

	if (!req)
		return NULL;
	req->buf = kmalloc(size, GFP_KERNEL);
	if (!req->buf) {
		usb_ep_free_request(ep, req);
		return NULL;
	}
	req->complete = complete;
	req->length = size;
	return req;
}

static void ds5_free_req(struct usb_ep *ep, struct usb_request *req)
{
	if (!req)
		return;
	kfree(req->buf);
	usb_ep_free_request(ep, req);
}

static int ds5_bind(struct usb_gadget *gadget, struct usb_gadget_driver *driver)
{
	struct ds5_slot *slot = NULL;
	int i, ret;

	for (i = 0; i < VIBESHINE_DS5_SLOTS; i++) {
		if (!ds5_slots[i].gadget) {
			slot = &ds5_slots[i];
			break;
		}
	}
	if (!slot)
		return -ENOSPC;

	/* The gadget pointer is what CREATE and the I/O paths test for a usable slot:
	 * publish it last, under the slot lock, and only when every request exists.
	 * The core does not call unbind for a failed bind, so free on failure here. */
	set_gadget_data(gadget, slot);
	usb_ep_autoconfig_reset(gadget);
	slot->ep_iso_out = ds5_find_ep(gadget, "ep1out-iso");
	slot->ep_iso_in = ds5_find_ep(gadget, "ep2in-iso");
	slot->ep_int_in = ds5_find_ep(gadget, "ep5in-int");
	slot->ep_int_out = ds5_find_ep(gadget, "ep-aout");
	if (!slot->ep_iso_out || !slot->ep_iso_in || !slot->ep_int_in || !slot->ep_int_out) {
		ret = -ENODEV;
		goto fail;
	}

	slot->ep0_req = ds5_alloc_req(gadget->ep0, 512, ds5_ep0_complete);
	slot->req_int_in = ds5_alloc_req(slot->ep_int_in, DS5_INT_MAXP, ds5_int_in_complete);
	slot->req_int_out = ds5_alloc_req(slot->ep_int_out, DS5_INT_MAXP, ds5_int_out_complete);
	if (!slot->ep0_req || !slot->req_int_in || !slot->req_int_out) {
		ret = -ENOMEM;
		goto fail;
	}
	for (i = 0; i < DS5_ISO_OUT_Q; i++) {
		slot->req_iso_out[i] = ds5_alloc_req(slot->ep_iso_out, DS5_ISO_OUT_MAXP, ds5_iso_out_complete);
		if (!slot->req_iso_out[i]) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	for (i = 0; i < DS5_ISO_IN_Q; i++) {
		slot->req_iso_in[i] = ds5_alloc_req(slot->ep_iso_in, DS5_ISO_IN_MAXP, ds5_iso_in_complete);
		if (!slot->req_iso_in[i]) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	slot->input[0] = 0x01;
	slot->input[1] = 0x80;
	slot->input[2] = 0x80;
	slot->input[3] = 0x80;
	slot->input[4] = 0x80;
	gadget->is_selfpowered = 1;
	usb_gadget_set_selfpowered(gadget);
	usb_gadget_disconnect(gadget);
	mutex_lock(&slot->lock);
	slot->gadget = gadget;
	mutex_unlock(&slot->lock);
	return 0;

fail:
	ds5_free_req(gadget->ep0, slot->ep0_req);
	ds5_free_req(slot->ep_int_in, slot->req_int_in);
	ds5_free_req(slot->ep_int_out, slot->req_int_out);
	for (i = 0; i < DS5_ISO_OUT_Q; i++)
		ds5_free_req(slot->ep_iso_out, slot->req_iso_out[i]);
	for (i = 0; i < DS5_ISO_IN_Q; i++)
		ds5_free_req(slot->ep_iso_in, slot->req_iso_in[i]);
	slot->ep0_req = NULL;
	slot->req_int_in = NULL;
	slot->req_int_out = NULL;
	memset(slot->req_iso_out, 0, sizeof(slot->req_iso_out));
	memset(slot->req_iso_in, 0, sizeof(slot->req_iso_in));
	set_gadget_data(gadget, NULL);
	return ret;
}

static void ds5_unbind(struct usb_gadget *gadget)
{
	struct ds5_slot *slot = gadget_to_slot(gadget);
	int i;

	if (!slot)
		return;
	/* Under the slot lock: a write() past its hid_enabled test must not queue a request
	 * this function is freeing. The slot is no longer created afterwards, so its owner
	 * sees -ENODEV and a rebind does not strand it. */
	mutex_lock(&slot->lock);
	ds5_disable_iso_out(slot);
	ds5_disable_iso_in(slot);
	ds5_disable_hid(slot);
	ds5_free_req(gadget->ep0, slot->ep0_req);
	ds5_free_req(slot->ep_int_in, slot->req_int_in);
	ds5_free_req(slot->ep_int_out, slot->req_int_out);
	for (i = 0; i < DS5_ISO_OUT_Q; i++)
		ds5_free_req(slot->ep_iso_out, slot->req_iso_out[i]);
	for (i = 0; i < DS5_ISO_IN_Q; i++)
		ds5_free_req(slot->ep_iso_in, slot->req_iso_in[i]);
	slot->ep0_req = NULL;
	slot->req_int_in = NULL;
	slot->req_int_out = NULL;
	slot->gadget = NULL;
	slot->created = false;
	WRITE_ONCE(slot->owner, NULL);
	mutex_unlock(&slot->lock);
	wake_up_interruptible_all(&slot->wait);
}

static void ds5_disconnect(struct usb_gadget *gadget)
{
	struct ds5_slot *slot = gadget_to_slot(gadget);

	if (!slot)
		return;
	ds5_disable_iso_out(slot);
	ds5_disable_iso_in(slot);
	ds5_disable_hid(slot);
	slot->config = 0;
	slot->alt1 = slot->alt2 = 0;
}

static const struct usb_gadget_driver ds5_driver_template = {
	.function = "DualSense",
	.max_speed = USB_SPEED_HIGH,
	.bind = ds5_bind,
	.unbind = ds5_unbind,
	.setup = ds5_setup,
	.reset = ds5_disconnect,
	.disconnect = ds5_disconnect,
	.match_existing_only = 1,
	.driver = {
		.owner = THIS_MODULE,
		.name = "vibeshine_ds5",
		/* No sysfs bind/unbind: a slot may be owned by an open fd. */
		.suppress_bind_attrs = true,
	},
};

static int ds5_open(struct inode *inode, struct file *file)
{
	struct ds5_file *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	mutex_init(&ctx->lock);
	file->private_data = ctx;
	return 0;
}

static int ds5_release(struct inode *inode, struct file *file)
{
	struct ds5_file *ctx = file->private_data;
	struct ds5_slot *slot;

	mutex_lock(&ctx->lock);
	slot = ctx->slot;
	if (slot) {
		mutex_lock(&slot->lock);
		/* Unbind may already have retired this ownership and rebind may have
		 * handed the slot to another fd. Never retire the replacement. */
		if (slot->owner == file) {
			if (slot->created && slot->gadget)
				usb_gadget_disconnect(slot->gadget);
			slot->created = false;
			WRITE_ONCE(slot->owner, NULL);
		}
		ctx->slot = NULL;
		mutex_unlock(&slot->lock);
		wake_up_interruptible_all(&slot->wait);
	}
	mutex_unlock(&ctx->lock);
	file->private_data = NULL;
	kfree(ctx);
	return 0;
}

static long ds5_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ds5_file *ctx = file->private_data;
	struct vibeshine_ds5_create create;
	struct ds5_slot *slot;
	unsigned long flags;
	int ret = 0;

	switch (cmd) {
	case VIBESHINE_DS5_CREATE:
		if (copy_from_user(&create, (void __user *)arg, sizeof(create)))
			return -EFAULT;
		if (create.slot >= VIBESHINE_DS5_SLOTS)
			return -EINVAL;
		mutex_lock(&ctx->lock);
		if (ctx->slot) {
			mutex_unlock(&ctx->lock);
			return -EBUSY;
		}
		slot = &ds5_slots[create.slot];
		mutex_lock(&slot->lock);
		if (slot->created) {
			ret = -EBUSY;
			goto unlock;
		}
		if (!slot->gadget) {
			ret = -ENODEV;
			goto unlock;
		}
		memcpy(slot->mac, create.mac, 6);
		spin_lock_irqsave(&slot->evlock, flags);
		INIT_KFIFO(slot->events);
		spin_unlock_irqrestore(&slot->evlock, flags);
		WRITE_ONCE(slot->generation, slot->generation + 1);
		slot->created = true;
		WRITE_ONCE(slot->owner, file);
		ctx->slot = slot;
		usb_gadget_connect(slot->gadget);
		break;
	case VIBESHINE_DS5_DESTROY:
		mutex_lock(&ctx->lock);
		slot = ctx->slot;
		if (!slot) {
			mutex_unlock(&ctx->lock);
			return -ENODEV;
		}
		mutex_lock(&slot->lock);
		if (slot->owner == file) {
			if (slot->gadget)
				usb_gadget_disconnect(slot->gadget);
			slot->created = false;
			WRITE_ONCE(slot->owner, NULL);
		} else {
			ret = -ENODEV;
		}
		ctx->slot = NULL;
		/* Also wake a read whose fd immediately CREATEs the slot again. */
		wake_up_interruptible_all(&slot->wait);
		break;
	default:
		return -ENOTTY;
	}
unlock:
	mutex_unlock(&slot->lock);
	mutex_unlock(&ctx->lock);
	return ret;
}

static ssize_t ds5_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct ds5_file *ctx = file->private_data;
	struct ds5_slot *slot;
	u8 report[VIBESHINE_DS5_INPUT_SIZE];
	ssize_t result;

	if (count < 1 || count > VIBESHINE_DS5_INPUT_SIZE)
		return -EINVAL;
	mutex_lock(&ctx->lock);
	slot = ctx->slot;
	if (!slot) {
		result = -ENODEV;
		goto unlock_file;
	}
	if (copy_from_user(report, buf, count)) {
		result = -EFAULT;
		goto unlock_file;
	}
	mutex_lock(&slot->lock);
	if (!slot->created || slot->owner != file || !slot->gadget) {
		result = -ENODEV;
		goto unlock_slot;
	}
	memset(slot->input, 0, sizeof(slot->input));
	memcpy(slot->input, report, count);
	if (smp_load_acquire(&slot->hid_enabled) &&
	    !smp_load_acquire(&slot->int_in_busy)) {
		int ret;

		memcpy(slot->req_int_in->buf, slot->input, VIBESHINE_DS5_INPUT_SIZE);
		slot->req_int_in->length = VIBESHINE_DS5_INPUT_SIZE;
		/* Completion can run inside queue(): never set busy afterwards. */
		WRITE_ONCE(slot->int_in_busy, true);
		ret = usb_ep_queue(slot->ep_int_in, slot->req_int_in, GFP_KERNEL);
		if (ret) {
			smp_store_release(&slot->int_in_busy, false);
			result = ret;
			goto unlock_slot;
		}
	}
	result = count;
unlock_slot:
	mutex_unlock(&slot->lock);
unlock_file:
	mutex_unlock(&ctx->lock);
	return result;
}

static ssize_t ds5_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct ds5_file *ctx = file->private_data;
	struct ds5_slot *slot;
	struct vibeshine_ds5_event ev;
	unsigned long flags;
	u32 generation;
	int ret;

	if (count < sizeof(ev))
		return -EINVAL;
	mutex_lock(&ctx->lock);
	slot = ctx->slot;
	if (!slot) {
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}
	mutex_lock(&slot->lock);
	if (!slot->created || slot->owner != file) {
		mutex_unlock(&slot->lock);
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}
	generation = slot->generation;
	mutex_unlock(&slot->lock);
	mutex_unlock(&ctx->lock);

	for (;;) {
		/* Never hold either mutex while sleeping: DESTROY must be able to wake us.
		 * The owner pointer alone cannot detect DESTROY/CREATE by the same fd. */
		if (!(file->f_flags & O_NONBLOCK)) {
			ret = wait_event_interruptible(slot->wait, kfifo_len(&slot->events) > 0 ||
						       READ_ONCE(slot->owner) != file ||
						       READ_ONCE(slot->generation) != generation);
			if (ret)
				return ret;
		}

		mutex_lock(&ctx->lock);
		mutex_lock(&slot->lock);
		/* Check and dequeue under the lifecycle locks for blocking AND nonblocking
		 * reads. A slot must not change ownership between this check and kfifo_get. */
		if (ctx->slot != slot || !slot->created || slot->owner != file ||
		    slot->generation != generation) {
			ret = -ENODEV;
		} else {
			spin_lock_irqsave(&slot->evlock, flags);
			ret = kfifo_get(&slot->events, &ev) ? 0 : -EAGAIN;
			spin_unlock_irqrestore(&slot->evlock, flags);
		}
		mutex_unlock(&slot->lock);
		mutex_unlock(&ctx->lock);
		/* A sibling reader on this fd took the event between the (non-exclusive)
		 * wake and the re-lock: a blocking read waits for the next one. */
		if (ret != -EAGAIN || (file->f_flags & O_NONBLOCK))
			break;
	}
	if (ret)
		return ret;
	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;
	return sizeof(ev);
}

static __poll_t ds5_poll(struct file *file, poll_table *wait)
{
	struct ds5_file *ctx = file->private_data;
	struct ds5_slot *slot;
	__poll_t mask = POLLOUT | POLLWRNORM;

	mutex_lock(&ctx->lock);
	slot = ctx->slot;
	if (!slot) {
		mutex_unlock(&ctx->lock);
		return POLLERR;
	}
	mutex_lock(&slot->lock);
	poll_wait(file, &slot->wait, wait);
	if (!slot->created || slot->owner != file)
		mask = POLLERR | POLLHUP;
	else if (kfifo_len(&slot->events))
		mask |= POLLIN | POLLRDNORM;
	mutex_unlock(&slot->lock);
	mutex_unlock(&ctx->lock);
	return mask;
}

static const struct file_operations ds5_fops = {
	.owner = THIS_MODULE,
	.open = ds5_open,
	.release = ds5_release,
	.unlocked_ioctl = ds5_ioctl,
	.compat_ioctl = ds5_ioctl,
	.read = ds5_read,
	.write = ds5_write,
	.poll = ds5_poll,
	.llseek = noop_llseek,
};

static struct miscdevice ds5_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vibeshine-ds5",
	.fops = &ds5_fops,
	.mode = 0660,
};

int vibeshine_ds5_gadget_init(void)
{
	int i, ret;

	if (ds5_config_desc[DS5_HID_DESC_OFFSET] != 0x09 ||
	    ds5_config_desc[DS5_HID_DESC_OFFSET + 1] != 0x21 /* HID class descriptor */) {
		pr_err("vibeshine_ds5: HID class descriptor is not at config offset %#x\n", DS5_HID_DESC_OFFSET);
		return -EINVAL;
	}

	for (i = 0; i < VIBESHINE_DS5_SLOTS; i++) {
		mutex_init(&ds5_slots[i].lock);
		init_waitqueue_head(&ds5_slots[i].wait);
		spin_lock_init(&ds5_slots[i].evlock);
		INIT_KFIFO(ds5_slots[i].events);
		ds5_drivers[i] = ds5_driver_template;
		snprintf(ds5_udc_names[i], sizeof(ds5_udc_names[i]),
			 "vibeshine_ds5_udc.%d", i);
		snprintf(ds5_driver_names[i], sizeof(ds5_driver_names[i]),
			 "vibeshine_ds5.%d", i);
		ds5_drivers[i].udc_name = ds5_udc_names[i];
		ds5_drivers[i].driver.name = ds5_driver_names[i];
		ret = usb_gadget_register_driver(&ds5_drivers[i]);
		if (ret)
			goto err;
		if (ds5_slots[i].gadget)
			usb_gadget_disconnect(ds5_slots[i].gadget);
	}
	ret = misc_register(&ds5_misc);
	if (ret)
		goto err;
	return 0;
err:
	while (--i >= 0)
		usb_gadget_unregister_driver(&ds5_drivers[i]);
	return ret;
}

void vibeshine_ds5_gadget_exit(void)
{
	int i;

	misc_deregister(&ds5_misc);
	for (i = 0; i < VIBESHINE_DS5_SLOTS; i++)
		usb_gadget_unregister_driver(&ds5_drivers[i]);
}
