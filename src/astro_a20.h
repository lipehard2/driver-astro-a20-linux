/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Astro A20 wireless transmitter, game (Xbox GIP) mode.
 */

#ifndef ASTRO_A20_H
#define ASTRO_A20_H

#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#define A20_VID 0x9886
#define A20_PID_GAME 0x0042

#define A20_INTF_DATA 0
#define A20_INTF_AUDIO 1

#define A20_DATA_PKT_LEN 64
#define A20_NUM_DATA_OUT_URBS 16

#define A20_NUM_AUDIO_URBS 4
#define A20_NUM_AUDIO_PKTS 8 /* one per 1 ms frame */

/* the only formats the transmitter offers (identify descriptor) */
#define A20_GIP_FMT_IN 0x05  /* 16 kHz mono */
#define A20_GIP_FMT_OUT 0x10 /* 48 kHz stereo */

#define A20_OUT_RATE 48000
#define A20_OUT_CHANNELS 2
#define A20_IN_RATE 16000
#define A20_IN_CHANNELS 1

/* sample bytes per 1 ms packet */
#define A20_OUT_FRAG (A20_OUT_RATE / 1000 * A20_OUT_CHANNELS * 2)
#define A20_IN_FRAG (A20_IN_RATE / 1000 * A20_IN_CHANNELS * 2)

/* GIP header (6 bytes for 192 byte payloads) plus samples */
#define A20_OUT_PKT_LEN (6 + A20_OUT_FRAG)

#define GIP_CMD_AUDIO_SAMPLES 0x60
#define GIP_OPT_INTERNAL 0x20

enum a20_state {
	A20_ST_WAIT_ANNOUNCE,
	A20_ST_WAIT_IDENTIFY,
	A20_ST_WAIT_FORMAT,
	A20_ST_POWERING_ON,
	A20_ST_STREAMING,
	A20_ST_FAILED,
};

struct snd_card;
struct snd_pcm_substream;

struct a20_stream {
	struct snd_pcm_substream *substream; /* non-NULL while running */
	unsigned int pos;		     /* bytes into the ring buffer */
	unsigned int period_pos;
};

struct a20 {
	struct usb_device *udev;
	struct usb_interface *intf_data;
	struct usb_interface *intf_audio;

	/* GIP control channel (interface 0) */
	struct urb *data_in_urb;
	struct usb_anchor data_out_idle;
	struct usb_anchor data_out_busy;
	struct usb_endpoint_descriptor *ep_data_in;
	struct usb_endpoint_descriptor *ep_data_out;

	/* protocol state, touched from URB completions */
	spinlock_t lock;
	enum a20_state state;
	u8 data_seq;
	bool got_volume;
	bool auth_hello_sent;
	int power_tries;
	u8 *chunk_buf;
	u32 chunk_len;
	u8 chunk_cmd;

	struct delayed_work state_work;
	struct work_struct start_work;
	bool audio_started; /* alt 1 may only be selected once per plug */
	bool disconnected;

	/* audio (interface 1, alt 1) */
	struct usb_endpoint_descriptor *ep_audio_in;
	struct usb_endpoint_descriptor *ep_audio_out;
	struct urb *audio_out_urbs[A20_NUM_AUDIO_URBS];
	struct urb *audio_in_urbs[A20_NUM_AUDIO_URBS];
	u8 audio_seq;

	struct snd_card *card;
	struct a20_stream playback;
	struct a20_stream capture;
};

/* gip.c */
int a20_gip_header_len(u32 len);
int a20_gip_encode_header(u8 *buf, u8 cmd, u8 options, u8 seq, u32 len);
int a20_gip_decode_header(const u8 *data, int len, u8 *cmd, u8 *options,
			  u32 *pkt_len);
int a20_gip_send(struct a20 *a20, u8 cmd, u8 options, u8 seq,
		 const void *payload, u32 len);
int a20_gip_send_power(struct a20 *a20, u8 mode);
void a20_gip_process(struct a20 *a20, const u8 *data, int len);
void a20_gip_state_work(struct work_struct *work);

#define GIP_PWR_ON 0x00
#define GIP_PWR_SLEEP 0x01

/* audio.c */
int a20_audio_init(struct a20 *a20);
int a20_audio_start(struct a20 *a20);
void a20_audio_stop(struct a20 *a20);
void a20_audio_free(struct a20 *a20);
int a20_card_register(struct a20 *a20);
void a20_card_disconnect(struct a20 *a20);

#endif /* ASTRO_A20_H */
