// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Astro A20: isochronous audio streaming and ALSA card.
 *
 * The transmitter only accepts selecting alt 1 once per plug, so both
 * isochronous streams run continuously from the end of the handshake until
 * the device goes away. Playback sends silence while no substream is
 * running. Each completed URB (8 ms) advances the ALSA pointers, so the
 * USB frame clock drives the PCM.
 */

#include <linux/slab.h>
#include <linux/unaligned.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

#include "astro_a20.h"

#define A20_CARD_NAME "Astro A20"

#define A20_PERIOD_BYTES_OUT (A20_OUT_FRAG * A20_NUM_AUDIO_PKTS)
#define A20_PERIOD_BYTES_IN (A20_IN_FRAG * A20_NUM_AUDIO_PKTS)

static const struct snd_pcm_hardware a20_pcm_hw_playback = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = A20_OUT_RATE,
	.rate_max = A20_OUT_RATE,
	.channels_min = A20_OUT_CHANNELS,
	.channels_max = A20_OUT_CHANNELS,
	.buffer_bytes_max = A20_PERIOD_BYTES_OUT * 128,
	.period_bytes_min = A20_PERIOD_BYTES_OUT,
	.period_bytes_max = A20_PERIOD_BYTES_OUT * 64,
	.periods_min = 2,
	.periods_max = 128,
};

static const struct snd_pcm_hardware a20_pcm_hw_capture = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_16000,
	.rate_min = A20_IN_RATE,
	.rate_max = A20_IN_RATE,
	.channels_min = A20_IN_CHANNELS,
	.channels_max = A20_IN_CHANNELS,
	.buffer_bytes_max = A20_PERIOD_BYTES_IN * 128,
	.period_bytes_min = A20_PERIOD_BYTES_IN,
	.period_bytes_max = A20_PERIOD_BYTES_IN * 64,
	.periods_min = 2,
	.periods_max = 128,
};

static struct a20_stream *a20_get_stream(struct snd_pcm_substream *sub)
{
	struct a20 *a20 = snd_pcm_substream_chip(sub);

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &a20->playback;

	return &a20->capture;
}

/* returns true when a period boundary was crossed */
static bool a20_stream_advance(struct a20_stream *stream,
			       struct snd_pcm_runtime *runtime, int len)
{
	unsigned int buf_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	unsigned int period_bytes = frames_to_bytes(runtime,
						    runtime->period_size);

	stream->pos += len;
	if (stream->pos >= buf_bytes)
		stream->pos -= buf_bytes;

	stream->period_pos += len;
	if (stream->period_pos >= period_bytes) {
		stream->period_pos %= period_bytes;
		return true;
	}

	return false;
}

/* copy len bytes of playback data out of the ring buffer */
static bool a20_copy_playback(struct a20_stream *stream,
			      struct snd_pcm_runtime *runtime, u8 *dest,
			      int len)
{
	unsigned int buf_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	unsigned int part = min_t(unsigned int, len, buf_bytes - stream->pos);

	memcpy(dest, runtime->dma_area + stream->pos, part);
	if (part < len)
		memcpy(dest + part, runtime->dma_area, len - part);

	return a20_stream_advance(stream, runtime, len);
}

/* round(4096 * 10^(dB / 20)) for 0..24 dB */
static const u16 a20_mic_gain_q12[A20_MIC_GAIN_MAX_DB + 1] = {
	4096, 4596, 5157, 5786, 6492, 7284,
	8173, 9170, 10289, 11544, 12953, 14533,
	16306, 18296, 20529, 23034, 25844, 28997,
	32536, 36506, 40960, 45958, 51566, 57858,
	64917,
};

/* amplify len bytes of captured samples into the ring buffer */
static bool a20_copy_capture(struct a20 *a20, struct snd_pcm_runtime *runtime,
			     const u8 *src, int len)
{
	struct a20_stream *stream = &a20->capture;
	unsigned int buf_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	unsigned int pos = stream->pos;
	s32 gain = 0;
	s32 val;
	int i;

	if (READ_ONCE(a20->mic_enabled))
		gain = a20_mic_gain_q12[READ_ONCE(a20->mic_gain_db)];

	for (i = 0; i + 1 < len; i += 2) {
		val = (s16)get_unaligned_le16(src + i) * gain >> 12;
		put_unaligned_le16(clamp_t(s32, val, S16_MIN, S16_MAX),
				   runtime->dma_area + pos);

		pos += 2;
		if (pos >= buf_bytes)
			pos = 0;
	}

	return a20_stream_advance(stream, runtime, len);
}

static void a20_fill_audio_out(struct a20 *a20, struct urb *urb)
{
	struct snd_pcm_substream *sub = READ_ONCE(a20->playback.substream);
	bool running = false, elapsed = false;
	unsigned long flags = 0;
	u8 *pkt;
	int i, n;

	if (sub) {
		snd_pcm_stream_lock_irqsave(sub, flags);
		running = sub->runtime && snd_pcm_running(sub);
	}

	for (i = 0; i < urb->number_of_packets; i++) {
		pkt = urb->transfer_buffer + urb->iso_frame_desc[i].offset;

		/* sequence numbers are never zero */
		if (!++a20->audio_seq)
			++a20->audio_seq;

		n = a20_gip_encode_header(pkt, GIP_CMD_AUDIO_SAMPLES,
					  GIP_OPT_INTERNAL, a20->audio_seq,
					  A20_OUT_FRAG);

		if (running)
			elapsed |= a20_copy_playback(&a20->playback,
						     sub->runtime, pkt + n,
						     A20_OUT_FRAG);
		else
			memset(pkt + n, 0, A20_OUT_FRAG);
	}

	if (sub) {
		snd_pcm_stream_unlock_irqrestore(sub, flags);
		if (elapsed)
			snd_pcm_period_elapsed(sub);
	}
}

static bool a20_urb_stopped(int status)
{
	return status == -ENOENT || status == -ECONNRESET ||
	       status == -ESHUTDOWN || status == -ENODEV;
}

static void a20_audio_out_complete(struct urb *urb)
{
	struct a20 *a20 = urb->context;
	int err;

	if (a20_urb_stopped(urb->status))
		return;

	a20_fill_audio_out(a20, urb);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err && err != -ENODEV && err != -EPERM)
		dev_err_ratelimited(&a20->intf_audio->dev,
				    "audio out submit failed: %d\n", err);
}

static void a20_audio_in_complete(struct urb *urb)
{
	struct a20 *a20 = urb->context;
	struct snd_pcm_substream *sub;
	struct usb_iso_packet_descriptor *desc;
	bool running = false, elapsed = false;
	unsigned long flags = 0;
	u8 cmd, options;
	u32 pkt_len;
	const u8 *pkt;
	int i, n, err;

	if (a20_urb_stopped(urb->status))
		return;

	sub = READ_ONCE(a20->capture.substream);
	if (sub) {
		snd_pcm_stream_lock_irqsave(sub, flags);
		running = sub->runtime && snd_pcm_running(sub);
	}

	for (i = 0; running && i < urb->number_of_packets; i++) {
		desc = &urb->iso_frame_desc[i];
		pkt = urb->transfer_buffer + desc->offset;

		if (desc->status || desc->actual_length < 4)
			continue;

		n = a20_gip_decode_header(pkt, desc->actual_length, &cmd,
					  &options, &pkt_len);
		if (cmd != GIP_CMD_AUDIO_SAMPLES ||
		    n + pkt_len > desc->actual_length)
			continue;

		/* le16 flow rate, then the samples */
		if (pkt_len <= 2)
			continue;

		elapsed |= a20_copy_capture(a20, sub->runtime, pkt + n + 2,
					    (pkt_len - 2) & ~1);
	}

	if (sub) {
		snd_pcm_stream_unlock_irqrestore(sub, flags);
		if (elapsed)
			snd_pcm_period_elapsed(sub);
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err && err != -ENODEV && err != -EPERM)
		dev_err_ratelimited(&a20->intf_audio->dev,
				    "audio in submit failed: %d\n", err);
}

static struct urb *a20_alloc_iso_urb(struct a20 *a20,
				     struct usb_endpoint_descriptor *ep,
				     int pkt_len, usb_complete_t complete)
{
	struct urb *urb;
	int i;

	urb = usb_alloc_urb(A20_NUM_AUDIO_PKTS, GFP_KERNEL);
	if (!urb)
		return NULL;

	urb->transfer_buffer = usb_alloc_coherent(a20->udev,
						  pkt_len * A20_NUM_AUDIO_PKTS,
						  GFP_KERNEL,
						  &urb->transfer_dma);
	if (!urb->transfer_buffer) {
		usb_free_urb(urb);
		return NULL;
	}

	urb->dev = a20->udev;
	if (usb_endpoint_dir_in(ep))
		urb->pipe = usb_rcvisocpipe(a20->udev, ep->bEndpointAddress);
	else
		urb->pipe = usb_sndisocpipe(a20->udev, ep->bEndpointAddress);
	urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
	urb->transfer_buffer_length = pkt_len * A20_NUM_AUDIO_PKTS;
	urb->number_of_packets = A20_NUM_AUDIO_PKTS;
	urb->interval = ep->bInterval;
	urb->context = a20;
	urb->complete = complete;

	for (i = 0; i < A20_NUM_AUDIO_PKTS; i++) {
		urb->iso_frame_desc[i].offset = i * pkt_len;
		urb->iso_frame_desc[i].length = pkt_len;
	}

	return urb;
}

static void a20_free_iso_urb(struct urb *urb)
{
	if (!urb)
		return;

	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	usb_free_urb(urb);
}

int a20_audio_init(struct a20 *a20)
{
	struct usb_host_interface *alt;
	struct usb_endpoint_descriptor *ep;
	int i;

	alt = usb_altnum_to_altsetting(a20->intf_audio, 1);
	if (!alt)
		return -ENODEV;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		ep = &alt->endpoint[i].desc;
		if (usb_endpoint_is_isoc_in(ep))
			a20->ep_audio_in = ep;
		else if (usb_endpoint_is_isoc_out(ep))
			a20->ep_audio_out = ep;
	}

	if (!a20->ep_audio_in || !a20->ep_audio_out)
		return -ENODEV;

	if (usb_endpoint_maxp(a20->ep_audio_out) < A20_OUT_PKT_LEN)
		return -ENODEV;

	for (i = 0; i < A20_NUM_AUDIO_URBS; i++) {
		a20->audio_out_urbs[i] =
			a20_alloc_iso_urb(a20, a20->ep_audio_out,
					  A20_OUT_PKT_LEN,
					  a20_audio_out_complete);
		a20->audio_in_urbs[i] =
			a20_alloc_iso_urb(a20, a20->ep_audio_in,
					  usb_endpoint_maxp(a20->ep_audio_in),
					  a20_audio_in_complete);
		if (!a20->audio_out_urbs[i] || !a20->audio_in_urbs[i])
			return -ENOMEM;
	}

	return 0;
}

int a20_audio_start(struct a20 *a20)
{
	int i, err;

	for (i = 0; i < A20_NUM_AUDIO_URBS; i++) {
		a20_fill_audio_out(a20, a20->audio_out_urbs[i]);
		err = usb_submit_urb(a20->audio_out_urbs[i], GFP_KERNEL);
		if (err)
			goto err_kill;

		err = usb_submit_urb(a20->audio_in_urbs[i], GFP_KERNEL);
		if (err)
			goto err_kill;
	}

	return 0;

err_kill:
	a20_audio_stop(a20);
	return err;
}

void a20_audio_stop(struct a20 *a20)
{
	int i;

	for (i = 0; i < A20_NUM_AUDIO_URBS; i++) {
		usb_kill_urb(a20->audio_out_urbs[i]);
		usb_kill_urb(a20->audio_in_urbs[i]);
	}
}

void a20_audio_free(struct a20 *a20)
{
	int i;

	for (i = 0; i < A20_NUM_AUDIO_URBS; i++) {
		a20_free_iso_urb(a20->audio_out_urbs[i]);
		a20_free_iso_urb(a20->audio_in_urbs[i]);
		a20->audio_out_urbs[i] = NULL;
		a20->audio_in_urbs[i] = NULL;
	}
}

static int a20_pcm_open(struct snd_pcm_substream *sub)
{
	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		sub->runtime->hw = a20_pcm_hw_playback;
	else
		sub->runtime->hw = a20_pcm_hw_capture;

	/* the pointer only moves in whole URBs */
	return snd_pcm_hw_constraint_step(sub->runtime, 0,
					  SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					  sub->stream == SNDRV_PCM_STREAM_PLAYBACK ?
					  A20_PERIOD_BYTES_OUT :
					  A20_PERIOD_BYTES_IN);
}

static int a20_pcm_close(struct snd_pcm_substream *sub)
{
	return 0;
}

static int a20_pcm_prepare(struct snd_pcm_substream *sub)
{
	struct a20_stream *stream = a20_get_stream(sub);

	stream->pos = 0;
	stream->period_pos = 0;

	return 0;
}

/* called with the substream lock held */
static int a20_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct a20_stream *stream = a20_get_stream(sub);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		WRITE_ONCE(stream->substream, sub);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		WRITE_ONCE(stream->substream, NULL);
		return 0;
	}

	return -EINVAL;
}

static snd_pcm_uframes_t a20_pcm_pointer(struct snd_pcm_substream *sub)
{
	struct a20_stream *stream = a20_get_stream(sub);

	return bytes_to_frames(sub->runtime, stream->pos);
}

static const struct snd_pcm_ops a20_pcm_ops = {
	.open = a20_pcm_open,
	.close = a20_pcm_close,
	.prepare = a20_pcm_prepare,
	.trigger = a20_pcm_trigger,
	.pointer = a20_pcm_pointer,
};

static const DECLARE_TLV_DB_SCALE(a20_mic_gain_tlv, 0, 100, 0);

static int a20_mic_volume_info(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = A20_MIC_GAIN_MAX_DB;

	return 0;
}

static int a20_mic_volume_get(struct snd_kcontrol *kctl,
			      struct snd_ctl_elem_value *val)
{
	struct a20 *a20 = snd_kcontrol_chip(kctl);

	val->value.integer.value[0] = READ_ONCE(a20->mic_gain_db);

	return 0;
}

static int a20_mic_volume_put(struct snd_kcontrol *kctl,
			      struct snd_ctl_elem_value *val)
{
	struct a20 *a20 = snd_kcontrol_chip(kctl);
	long db = val->value.integer.value[0];

	if (db < 0 || db > A20_MIC_GAIN_MAX_DB)
		return -EINVAL;

	if (db == READ_ONCE(a20->mic_gain_db))
		return 0;

	WRITE_ONCE(a20->mic_gain_db, db);

	return 1;
}

static int a20_mic_switch_get(struct snd_kcontrol *kctl,
			      struct snd_ctl_elem_value *val)
{
	struct a20 *a20 = snd_kcontrol_chip(kctl);

	val->value.integer.value[0] = READ_ONCE(a20->mic_enabled);

	return 0;
}

static int a20_mic_switch_put(struct snd_kcontrol *kctl,
			      struct snd_ctl_elem_value *val)
{
	struct a20 *a20 = snd_kcontrol_chip(kctl);
	bool enabled = !!val->value.integer.value[0];

	if (enabled == READ_ONCE(a20->mic_enabled))
		return 0;

	WRITE_ONCE(a20->mic_enabled, enabled);

	return 1;
}

static const struct snd_kcontrol_new a20_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Mic Capture Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = a20_mic_volume_info,
		.get = a20_mic_volume_get,
		.put = a20_mic_volume_put,
		.tlv.p = a20_mic_gain_tlv,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Mic Capture Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = a20_mic_switch_get,
		.put = a20_mic_switch_put,
	},
};

int a20_card_register(struct a20 *a20)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int i, err;

	err = snd_card_new(&a20->intf_data->dev, SNDRV_DEFAULT_IDX1, "A20",
			   THIS_MODULE, 0, &card);
	if (err)
		return err;

	strscpy(card->driver, "astro_a20", sizeof(card->driver));
	strscpy(card->shortname, A20_CARD_NAME, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s (game mode) at ",
		 A20_CARD_NAME);
	usb_make_path(a20->udev, card->longname + strlen(card->longname),
		      sizeof(card->longname) - strlen(card->longname));

	err = snd_pcm_new(card, A20_CARD_NAME, 0, 1, 1, &pcm);
	if (err)
		goto err_free;

	strscpy(pcm->name, A20_CARD_NAME, sizeof(pcm->name));
	pcm->private_data = a20;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &a20_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &a20_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	for (i = 0; i < ARRAY_SIZE(a20_controls); i++) {
		err = snd_ctl_add(card, snd_ctl_new1(&a20_controls[i], a20));
		if (err)
			goto err_free;
	}

	err = snd_card_register(card);
	if (err)
		goto err_free;

	a20->card = card;
	return 0;

err_free:
	snd_card_free(card);
	return err;
}

void a20_card_disconnect(struct a20 *a20)
{
	if (!a20->card)
		return;

	/* waits until userspace has closed every file of the card */
	snd_card_disconnect_sync(a20->card);
	snd_card_free(a20->card);
	a20->card = NULL;
}
