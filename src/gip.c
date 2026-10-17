// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Astro A20: GIP (Xbox Gaming Input Protocol) control channel.
 *
 * Handshake, as observed on the transmitter:
 *   device: announce          -> host: identify
 *   device: identify (chunks) -> host: power sleep, audio format
 *   device: format accepted   -> host: auth hello (unmutes the microphone),
 *                                      power on (retried until a volume report)
 *   host: select alt 1 once and stream audio forever after
 */

#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "astro_a20.h"

#define GIP_HDR_MIN_LENGTH 3
#define GIP_PKT_MAX_LENGTH 58
#define GIP_CHUNK_MAX_LENGTH 4096

#define GIP_CMD_ACKNOWLEDGE 0x01
#define GIP_CMD_ANNOUNCE 0x02
#define GIP_CMD_STATUS 0x03
#define GIP_CMD_IDENTIFY 0x04
#define GIP_CMD_POWER 0x05
#define GIP_CMD_AUTHENTICATE 0x06
#define GIP_CMD_AUDIO_CONTROL 0x08

#define GIP_OPT_CLIENT_ID 0x0f
#define GIP_OPT_ACKNOWLEDGE 0x10
#define GIP_OPT_CHUNK_START 0x40
#define GIP_OPT_CHUNK 0x80

#define GIP_STATUS_CONNECTED 0x80

#define GIP_AUD_CTRL_VOLUME_CHAT 0x00
#define GIP_AUD_CTRL_FORMAT 0x02
#define GIP_AUD_CTRL_VOLUME 0x03

/* auth handshake header options */
#define GIP_AUTH_OPT_ACKNOWLEDGE 0x01
#define GIP_AUTH_OPT_REQUEST 0x02
#define GIP_AUTH_OPT_FROM_HOST 0x40
#define GIP_AUTH_CMD_HOST_HELLO 0x01
#define GIP_AUTH_CMD_CLIENT_HELLO 0x02
#define GIP_AUTH_CLIENT_HELLO_LEN 80

#define GIP_IDENTIFY_PREFIX_LEN 16
#define GIP_HEADSET_CLASS "Windows.Xbox.Input.Headset"

#define A20_ANNOUNCE_TIMEOUT msecs_to_jiffies(2000)
#define A20_POWER_RETRY_DELAY msecs_to_jiffies(500)
#define A20_POWER_MAX_TRIES 6

static int gip_encode_varint(u8 *buf, u32 val)
{
	int i;

	for (i = 0; i < sizeof(val); i++) {
		buf[i] = val & 0x7f;
		if (val > 0x7f)
			buf[i] |= 0x80;

		val >>= 7;
		if (!val)
			break;
	}

	return i + 1;
}

static int gip_decode_varint(const u8 *data, int len, u32 *val)
{
	int i;

	*val = 0;
	for (i = 0; i < sizeof(*val) && i < len; i++) {
		*val |= (u32)(data[i] & 0x7f) << (i * 7);
		if (!(data[i] & 0x80))
			break;
	}

	return i + 1;
}

static int gip_actual_header_len(u32 len)
{
	int n = GIP_HDR_MIN_LENGTH;

	do {
		n++;
		len >>= 7;
	} while (len);

	return n;
}

int a20_gip_header_len(u32 len)
{
	int n = gip_actual_header_len(len);

	/* header length must be even */
	return n + (n % 2);
}

/* chunked packets are never sent by this driver */
int a20_gip_encode_header(u8 *buf, u8 cmd, u8 options, u8 seq, u32 len)
{
	int n = 0;

	buf[n++] = cmd;
	buf[n++] = options;
	buf[n++] = seq;
	n += gip_encode_varint(buf + n, len);

	if (gip_actual_header_len(len) % 2) {
		buf[n - 1] |= 0x80;
		buf[n++] = 0;
	}

	return n;
}

int a20_gip_decode_header(const u8 *data, int len, u8 *cmd, u8 *options,
			  u32 *pkt_len)
{
	*cmd = data[0];
	*options = data[1];

	return GIP_HDR_MIN_LENGTH +
	       gip_decode_varint(data + GIP_HDR_MIN_LENGTH,
				 len - GIP_HDR_MIN_LENGTH, pkt_len);
}

static void a20_data_out_complete(struct urb *urb)
{
	struct a20 *a20 = urb->context;

	if (urb->status && urb->status != -ENOENT &&
	    urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
		dev_dbg(&a20->intf_data->dev, "data out status %d\n",
			urb->status);

	usb_anchor_urb(urb, &a20->data_out_idle);
}

/* safe to call from atomic context */
int a20_gip_send(struct a20 *a20, u8 cmd, u8 options, u8 seq,
		 const void *payload, u32 len)
{
	struct urb *urb;
	int n, err;

	if (len > GIP_PKT_MAX_LENGTH)
		return -EMSGSIZE;

	urb = usb_get_from_anchor(&a20->data_out_idle);
	if (!urb)
		return -ENOSPC;

	/* sequence numbers are never zero */
	if (!seq) {
		seq = a20->data_seq++;
		if (!a20->data_seq)
			a20->data_seq = 1;
	}

	n = a20_gip_encode_header(urb->transfer_buffer, cmd, options, seq,
				  len);
	if (len)
		memcpy(urb->transfer_buffer + n, payload, len);
	urb->transfer_buffer_length = n + len;
	urb->complete = a20_data_out_complete;
	urb->context = a20;

	usb_anchor_urb(urb, &a20->data_out_busy);
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		usb_unanchor_urb(urb);
		usb_anchor_urb(urb, &a20->data_out_idle);
		dev_dbg(&a20->intf_data->dev, "send cmd 0x%02x failed: %d\n",
			cmd, err);
	}

	/* drop the reference taken by usb_get_from_anchor */
	usb_free_urb(urb);

	return err;
}

int a20_gip_send_power(struct a20 *a20, u8 mode)
{
	return a20_gip_send(a20, GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, &mode,
			    sizeof(mode));
}

static void a20_gip_send_ack(struct a20 *a20, u8 cmd, u8 options, u8 seq,
			     u32 received, u32 total)
{
	u8 client = options & GIP_OPT_CLIENT_ID;
	u8 pkt[9] = {};

	pkt[1] = cmd;
	pkt[2] = client | GIP_OPT_INTERNAL;
	pkt[3] = received & 0xff;
	pkt[4] = received >> 8;
	if (total) {
		pkt[7] = (total - received) & 0xff;
		pkt[8] = (total - received) >> 8;
	}

	a20_gip_send(a20, GIP_CMD_ACKNOWLEDGE, client | GIP_OPT_INTERNAL, seq,
		     pkt, sizeof(pkt));
}

static void a20_gip_set_state(struct a20 *a20, enum a20_state state)
{
	dev_dbg(&a20->intf_data->dev, "state %d -> %d\n", a20->state, state);
	a20->state = state;
}

static void a20_gip_send_identify(struct a20 *a20)
{
	a20_gip_set_state(a20, A20_ST_WAIT_IDENTIFY);
	a20_gip_send(a20, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
}

/*
 * The microphone is muted with digital silence until the host starts the
 * authentication handshake. Completing it is not required, so only the
 * host hello and the client hello request are sent.
 */
static void a20_gip_send_auth_hello(struct a20 *a20)
{
	/*
	 * handshake header: context, options, error, command, be16 length,
	 * data header: command, version, be16 length,
	 * then 32 random bytes, 8 unknown bytes and an 8 byte trailer
	 */
	u8 pkt[58] = {
		0x00, GIP_AUTH_OPT_ACKNOWLEDGE | GIP_AUTH_OPT_FROM_HOST,
		0x00, GIP_AUTH_CMD_HOST_HELLO, 0x00, 44,
		GIP_AUTH_CMD_HOST_HELLO, 0x01, 0x00, 40,
	};

	get_random_bytes(pkt + 10, 32);
	a20->auth_hello_sent = true;
	a20_gip_send(a20, GIP_CMD_AUTHENTICATE,
		     GIP_OPT_INTERNAL | GIP_OPT_ACKNOWLEDGE, 0, pkt, sizeof(pkt));
}

static void a20_gip_request_client_hello(struct a20 *a20)
{
	u16 len = GIP_AUTH_CLIENT_HELLO_LEN + 4; /* includes the data header */
	u8 pkt[14] = {
		0x00, GIP_AUTH_OPT_REQUEST | GIP_AUTH_OPT_FROM_HOST,
		0x00, GIP_AUTH_CMD_CLIENT_HELLO, len >> 8, len & 0xff,
	};

	a20_gip_send(a20, GIP_CMD_AUTHENTICATE,
		     GIP_OPT_INTERNAL | GIP_OPT_ACKNOWLEDGE, 0, pkt, sizeof(pkt));
}

static void a20_gip_handle_auth(struct a20 *a20, const u8 *data, u32 len)
{
	if (len < 6)
		return;

	if (data[2]) {
		dev_warn(&a20->intf_data->dev, "auth error 0x%02x\n", data[2]);
		return;
	}

	/* device acknowledged our host hello */
	if ((data[1] & GIP_AUTH_OPT_ACKNOWLEDGE) &&
	    data[3] == GIP_AUTH_CMD_HOST_HELLO && a20->auth_hello_sent) {
		a20_gip_request_client_hello(a20);
		return;
	}

	if (data[3] == GIP_AUTH_CMD_CLIENT_HELLO)
		dev_dbg(&a20->intf_data->dev, "auth client hello received\n");
}

static bool a20_gip_has_headset_class(const u8 *d, u32 len, u16 off)
{
	size_t class_len = strlen(GIP_HEADSET_CLASS);
	u8 count;
	u16 sl;

	if (!off || off >= len)
		return false;

	count = d[off++];
	while (count--) {
		if (off + 2 > len)
			return false;
		sl = d[off] | d[off + 1] << 8;
		off += 2;
		if (off + sl > len)
			return false;
		if (sl == class_len && !memcmp(d + off, GIP_HEADSET_CLASS, sl))
			return true;
		off += sl;
	}

	return false;
}

static void a20_gip_handle_identify(struct a20 *a20, const u8 *data, u32 len)
{
	struct device *dev = &a20->intf_data->dev;
	const u8 *d = data + GIP_IDENTIFY_PREFIX_LEN;
	u16 off_formats, off_classes;
	u8 fmt_in, fmt_out;

	if (a20->state != A20_ST_WAIT_IDENTIFY)
		return;

	if (len < GIP_IDENTIFY_PREFIX_LEN + 16) {
		dev_err(dev, "identify descriptor too short (%u)\n", len);
		goto err_fail;
	}

	/* offsets are relative to the end of the unknown prefix */
	len -= GIP_IDENTIFY_PREFIX_LEN;
	off_formats = d[4] | d[5] << 8;
	off_classes = d[10] | d[11] << 8;

	if (!a20_gip_has_headset_class(d, len, off_classes)) {
		dev_err(dev, "not a GIP headset\n");
		goto err_fail;
	}

	if (!off_formats || off_formats + 3 > len || !d[off_formats]) {
		dev_err(dev, "no audio formats in identify descriptor\n");
		goto err_fail;
	}

	fmt_in = d[off_formats + 1];
	fmt_out = d[off_formats + 2];
	if (fmt_in != A20_GIP_FMT_IN || fmt_out != A20_GIP_FMT_OUT) {
		dev_err(dev, "unsupported audio formats 0x%02x/0x%02x\n",
			fmt_in, fmt_out);
		goto err_fail;
	}

	a20_gip_set_state(a20, A20_ST_WAIT_FORMAT);
	a20_gip_send_power(a20, GIP_PWR_SLEEP);
	{
		u8 pkt[3] = { GIP_AUD_CTRL_FORMAT, fmt_in, fmt_out };

		a20_gip_send(a20, GIP_CMD_AUDIO_CONTROL, GIP_OPT_INTERNAL, 0,
			     pkt, sizeof(pkt));
	}
	return;

err_fail:
	a20_gip_set_state(a20, A20_ST_FAILED);
}

static void a20_gip_handle_audio_control(struct a20 *a20, const u8 *data,
					 u32 len)
{
	if (!len)
		return;

	switch (data[0]) {
	case GIP_AUD_CTRL_FORMAT:
		if (a20->state != A20_ST_WAIT_FORMAT || len < 3)
			break;
		if (data[1] != A20_GIP_FMT_IN || data[2] != A20_GIP_FMT_OUT) {
			dev_err(&a20->intf_data->dev,
				"format rejected: 0x%02x/0x%02x\n",
				data[1], data[2]);
			a20_gip_set_state(a20, A20_ST_FAILED);
			break;
		}
		a20_gip_send_auth_hello(a20);
		a20_gip_set_state(a20, A20_ST_POWERING_ON);
		a20->power_tries = 0;
		mod_delayed_work(system_percpu_wq, &a20->state_work, 0);
		break;
	case GIP_AUD_CTRL_VOLUME_CHAT:
	case GIP_AUD_CTRL_VOLUME:
		a20->got_volume = true;
		break;
	}
}

static void a20_gip_dispatch(struct a20 *a20, u8 cmd, u8 options,
			     const u8 *data, u32 len)
{
	if (!(options & GIP_OPT_INTERNAL))
		return;

	switch (cmd) {
	case GIP_CMD_ANNOUNCE:
		/* repeated until the identify request arrives */
		if (a20->state <= A20_ST_WAIT_IDENTIFY && !a20->chunk_buf)
			a20_gip_send_identify(a20);
		break;
	case GIP_CMD_STATUS:
		if (len >= 1)
			dev_dbg(&a20->intf_data->dev, "headset %s\n",
				(data[0] & GIP_STATUS_CONNECTED) ?
				"connected" : "disconnected");
		break;
	case GIP_CMD_IDENTIFY:
		a20_gip_handle_identify(a20, data, len);
		break;
	case GIP_CMD_AUTHENTICATE:
		a20_gip_handle_auth(a20, data, len);
		break;
	case GIP_CMD_AUDIO_CONTROL:
		a20_gip_handle_audio_control(a20, data, len);
		break;
	}
}

static void a20_gip_process_chunk(struct a20 *a20, u8 cmd, u8 options,
				  u8 seq, u32 offset, const u8 *data, u32 len)
{
	u32 end = offset + len;

	if (options & GIP_OPT_CHUNK_START) {
		kfree(a20->chunk_buf);
		a20->chunk_buf = NULL;

		/* the first chunk carries the total length as its offset */
		if (!offset || offset > GIP_CHUNK_MAX_LENGTH)
			return;

		a20->chunk_buf = kzalloc(offset, GFP_ATOMIC);
		if (!a20->chunk_buf)
			return;

		a20->chunk_len = offset;
		a20->chunk_cmd = cmd;
		offset = 0;
		end = len;
	}

	if (!a20->chunk_buf || cmd != a20->chunk_cmd)
		return;

	if (len) {
		if (end > a20->chunk_len)
			return;

		if ((options & GIP_OPT_ACKNOWLEDGE) || end == a20->chunk_len)
			a20_gip_send_ack(a20, cmd, options, seq, end,
					 a20->chunk_len);

		memcpy(a20->chunk_buf + offset, data, len);
		return;
	}

	/* an empty chunk completes the transfer */
	a20_gip_dispatch(a20, cmd, options, a20->chunk_buf, a20->chunk_len);
	kfree(a20->chunk_buf);
	a20->chunk_buf = NULL;
}

/* called from the data in URB completion */
void a20_gip_process(struct a20 *a20, const u8 *data, int len)
{
	unsigned long flags;
	u8 cmd, options, seq;
	u32 pkt_len, offset;
	int n;

	spin_lock_irqsave(&a20->lock, flags);

	while (len > GIP_HDR_MIN_LENGTH) {
		n = a20_gip_decode_header(data, len, &cmd, &options, &pkt_len);
		seq = data[2];
		offset = 0;
		if (options & GIP_OPT_CHUNK)
			n += gip_decode_varint(data + n, len - n, &offset);

		if (n + pkt_len > len) {
			dev_dbg(&a20->intf_data->dev, "truncated packet\n");
			break;
		}

		if (options & GIP_OPT_CHUNK) {
			a20_gip_process_chunk(a20, cmd, options, seq, offset,
					      data + n, pkt_len);
		} else {
			if (options & GIP_OPT_ACKNOWLEDGE)
				a20_gip_send_ack(a20, cmd, options, seq,
						 pkt_len, 0);
			a20_gip_dispatch(a20, cmd, options, data + n, pkt_len);
		}

		data += n + pkt_len;
		len -= n + pkt_len;
	}

	spin_unlock_irqrestore(&a20->lock, flags);
}

/* announce timeout and power on retries */
void a20_gip_state_work(struct work_struct *work)
{
	struct a20 *a20 = container_of(to_delayed_work(work), struct a20,
				       state_work);
	unsigned long flags;

	spin_lock_irqsave(&a20->lock, flags);

	switch (a20->state) {
	case A20_ST_WAIT_ANNOUNCE:
		/* the transmitter only announces right after plugging in */
		dev_dbg(&a20->intf_data->dev, "no announce, requesting identify\n");
		a20_gip_send_identify(a20);
		break;
	case A20_ST_POWERING_ON:
		if (a20->got_volume || a20->power_tries >= A20_POWER_MAX_TRIES) {
			a20_gip_set_state(a20, A20_ST_STREAMING);
			schedule_work(&a20->start_work);
			break;
		}
		a20->power_tries++;
		a20_gip_send_power(a20, GIP_PWR_ON);
		mod_delayed_work(system_percpu_wq, &a20->state_work,
				 A20_POWER_RETRY_DELAY);
		break;
	default:
		break;
	}

	spin_unlock_irqrestore(&a20->lock, flags);
}
