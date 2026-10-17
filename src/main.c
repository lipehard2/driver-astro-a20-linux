// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Astro A20 wireless headset transmitter, game (Xbox GIP) mode.
 *
 * In game mode the transmitter (9886:0042) speaks the Xbox Gaming Input
 * Protocol instead of USB Audio Class, so no stock driver binds to it.
 * This driver performs the GIP handshake and exposes the headset as an
 * ALSA card with 48 kHz stereo playback and 16 kHz mono capture.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "astro_a20.h"

static struct usb_driver a20_driver;

static void a20_data_in_complete(struct urb *urb)
{
	struct a20 *a20 = urb->context;
	int err;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;
	default:
		goto resubmit;
	}

	if (urb->actual_length)
		a20_gip_process(a20, urb->transfer_buffer, urb->actual_length);

resubmit:
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err && err != -ENODEV && err != -EPERM)
		dev_err(&a20->intf_data->dev, "data in submit failed: %d\n",
			err);
}

/* select alt 1, start streaming and expose the card (process context) */
static void a20_start_work(struct work_struct *work)
{
	struct a20 *a20 = container_of(work, struct a20, start_work);
	struct device *dev = &a20->intf_data->dev;
	int err;

	if (a20->audio_started || a20->disconnected)
		return;

	/*
	 * The transmitter wedges its control endpoint if alt 1 is selected
	 * a second time, so this happens exactly once per plug.
	 */
	a20->audio_started = true;

	err = usb_set_interface(a20->udev, A20_INTF_AUDIO, 1);
	if (err) {
		dev_err(dev, "enabling audio failed: %d (replug the transmitter)\n",
			err);
		return;
	}

	err = a20_audio_start(a20);
	if (err) {
		dev_err(dev, "starting audio streams failed: %d\n", err);
		return;
	}

	err = a20_card_register(a20);
	if (err) {
		dev_err(dev, "registering sound card failed: %d\n", err);
		a20_audio_stop(a20);
		return;
	}

	dev_info(dev, "Astro A20 ready\n");
}

static int a20_init_data_urbs(struct a20 *a20)
{
	struct urb *urb;
	void *buf;
	int i;

	for (i = 0; i < A20_NUM_DATA_OUT_URBS; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;

		usb_anchor_urb(urb, &a20->data_out_idle);
		usb_free_urb(urb);

		buf = usb_alloc_coherent(a20->udev, A20_DATA_PKT_LEN,
					 GFP_KERNEL, &urb->transfer_dma);
		if (!buf)
			return -ENOMEM;

		usb_fill_int_urb(urb, a20->udev,
				 usb_sndintpipe(a20->udev,
						a20->ep_data_out->bEndpointAddress),
				 buf, A20_DATA_PKT_LEN, NULL, a20,
				 a20->ep_data_out->bInterval);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	a20->data_in_urb = urb;

	buf = usb_alloc_coherent(a20->udev, A20_DATA_PKT_LEN, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf)
		return -ENOMEM;

	usb_fill_int_urb(urb, a20->udev,
			 usb_rcvintpipe(a20->udev,
					a20->ep_data_in->bEndpointAddress),
			 buf, A20_DATA_PKT_LEN, a20_data_in_complete, a20,
			 a20->ep_data_in->bInterval);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	return 0;
}

static void a20_free_data_urbs(struct a20 *a20)
{
	struct urb *urb;

	if (a20->data_in_urb) {
		usb_free_coherent(a20->udev, A20_DATA_PKT_LEN,
				  a20->data_in_urb->transfer_buffer,
				  a20->data_in_urb->transfer_dma);
		usb_free_urb(a20->data_in_urb);
		a20->data_in_urb = NULL;
	}

	while ((urb = usb_get_from_anchor(&a20->data_out_idle))) {
		usb_free_coherent(a20->udev, A20_DATA_PKT_LEN,
				  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
	}
}

static void a20_free(struct a20 *a20)
{
	a20_audio_free(a20);
	a20_free_data_urbs(a20);
	kfree(a20->chunk_buf);
	kfree(a20);
}

static int a20_probe(struct usb_interface *intf,
		     const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct a20 *a20;
	int err;

	a20 = kzalloc(sizeof(*a20), GFP_KERNEL);
	if (!a20)
		return -ENOMEM;

	a20->udev = udev;
	a20->intf_data = intf;
	a20->data_seq = 1;
	a20->state = A20_ST_WAIT_ANNOUNCE;
	spin_lock_init(&a20->lock);
	init_usb_anchor(&a20->data_out_idle);
	init_usb_anchor(&a20->data_out_busy);
	INIT_DELAYED_WORK(&a20->state_work, a20_gip_state_work);
	INIT_WORK(&a20->start_work, a20_start_work);

	err = usb_find_common_endpoints(intf->cur_altsetting, NULL, NULL,
					&a20->ep_data_in, &a20->ep_data_out);
	if (err)
		goto err_free;

	a20->intf_audio = usb_ifnum_to_if(udev, A20_INTF_AUDIO);
	if (!a20->intf_audio) {
		err = -ENODEV;
		goto err_free;
	}

	err = a20_audio_init(a20);
	if (err)
		goto err_free;

	err = a20_init_data_urbs(a20);
	if (err)
		goto err_free;

	err = usb_driver_claim_interface(&a20_driver, a20->intf_audio, a20);
	if (err)
		goto err_free;

	usb_set_intfdata(intf, a20);

	err = usb_submit_urb(a20->data_in_urb, GFP_KERNEL);
	if (err)
		goto err_release;

	/* the transmitter announces itself only right after plugging in */
	schedule_delayed_work(&a20->state_work, msecs_to_jiffies(2000));

	return 0;

err_release:
	usb_set_intfdata(intf, NULL);
	usb_driver_release_interface(&a20_driver, a20->intf_audio);
err_free:
	a20_free(a20);
	return err;
}

static void a20_disconnect(struct usb_interface *intf)
{
	struct a20 *a20 = usb_get_intfdata(intf);
	unsigned long flags;

	if (!a20)
		return;

	/* the audio interface is unbound on its own, e.g. on unplug */
	if (intf != a20->intf_data) {
		a20_audio_stop(a20);
		usb_set_intfdata(intf, NULL);
		return;
	}

	spin_lock_irqsave(&a20->lock, flags);
	a20->disconnected = true;
	a20->state = A20_ST_FAILED;
	spin_unlock_irqrestore(&a20->lock, flags);

	cancel_delayed_work_sync(&a20->state_work);
	cancel_work_sync(&a20->start_work);

	a20_card_disconnect(a20);
	a20_audio_stop(a20);

	/* power the headset down before the core switches back to alt 0 */
	if (a20->audio_started && a20->udev->state != USB_STATE_NOTATTACHED) {
		spin_lock_irqsave(&a20->lock, flags);
		a20_gip_send_power(a20, GIP_PWR_SLEEP);
		spin_unlock_irqrestore(&a20->lock, flags);
		usb_wait_anchor_empty_timeout(&a20->data_out_busy, 100);
	}

	usb_kill_urb(a20->data_in_urb);
	usb_kill_anchored_urbs(&a20->data_out_busy);

	usb_set_intfdata(intf, NULL);
	if (usb_get_intfdata(a20->intf_audio))
		usb_driver_release_interface(&a20_driver, a20->intf_audio);

	a20_free(a20);
}

static const struct usb_device_id a20_id_table[] = {
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE |
			       USB_DEVICE_ID_MATCH_INT_INFO |
			       USB_DEVICE_ID_MATCH_INT_NUMBER,
		.idVendor = A20_VID,
		.idProduct = A20_PID_GAME,
		.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
		.bInterfaceSubClass = 0x47,
		.bInterfaceProtocol = 0xd0,
		.bInterfaceNumber = A20_INTF_DATA,
	},
	{ },
};
MODULE_DEVICE_TABLE(usb, a20_id_table);

static struct usb_driver a20_driver = {
	.name = "astro_a20",
	.probe = a20_probe,
	.disconnect = a20_disconnect,
	.id_table = a20_id_table,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(a20_driver);

MODULE_DESCRIPTION("Astro A20 wireless headset (game mode) driver");
MODULE_LICENSE("GPL");
