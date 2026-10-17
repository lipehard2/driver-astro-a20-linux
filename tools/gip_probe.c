// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gip_probe - userspace prototype for the Astro A20 transmitter in game
 * (Xbox GIP) mode, 9886:0042.
 *
 * Performs the GIP handshake, logs every packet, and optionally streams a
 * test tone to the headset and records the microphone.
 *
 * Usage: gip_probe [-t] [-m mic.raw] [-s seconds] [-r] [-q]
 *   -t  play a 440 Hz tone once audio is ready
 *   -m  write raw microphone samples to file
 *   -s  run time in seconds (default 10)
 *   -r  reset the USB device before starting
 *   -q  do not log audio packets
 *   -v  send a host volume command N seconds after streaming starts
 *   -z  switch the audio interface back to alt 0 on exit
 *   -n  do not issue SET_INTERFACE (assume alt 1 is still active)
 *   -a  send an unauthenticated "auth complete" after power on
 *   -H  send an auth v1 Host Hello and request the Client Hello
 *   -g  graceful exit: power SLEEP, wait 1 s, then SET_INTERFACE(alt 0)
 */

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

#define A20_VID 0x9886
#define A20_PID_GAME 0x0042

#define INTF_DATA 0
#define INTF_AUDIO 1
#define EP_DATA_IN 0x81
#define EP_DATA_OUT 0x01
#define EP_AUDIO_OUT 0x02
#define EP_AUDIO_IN 0x83

#define DATA_PKT_LEN 64
#define AUDIO_PKTS 8 /* one packet per 1 ms frame, 8 ms per transfer */
#define AUDIO_XFERS 4
#define AUDIO_IN_MAXP 44

#define GIP_PKT_MAX_LENGTH 58

enum {
	GIP_CMD_ACKNOWLEDGE = 0x01,
	GIP_CMD_ANNOUNCE = 0x02,
	GIP_CMD_STATUS = 0x03,
	GIP_CMD_IDENTIFY = 0x04,
	GIP_CMD_POWER = 0x05,
	GIP_CMD_AUTHENTICATE = 0x06,
	GIP_CMD_VIRTUAL_KEY = 0x07,
	GIP_CMD_AUDIO_CONTROL = 0x08,
	GIP_CMD_LED = 0x0a,
	GIP_CMD_HID_REPORT = 0x0b,
	GIP_CMD_FIRMWARE = 0x0c,
	GIP_CMD_EXTENDED = 0x1e,
	GIP_CMD_AUDIO_SAMPLES = 0x60,
};

#define GIP_OPT_ACKNOWLEDGE 0x10
#define GIP_OPT_INTERNAL 0x20
#define GIP_OPT_CHUNK_START 0x40
#define GIP_OPT_CHUNK 0x80

enum {
	GIP_PWR_ON = 0x00,
	GIP_PWR_SLEEP = 0x01,
};

enum {
	GIP_AUD_CTRL_VOLUME_CHAT = 0x00,
	GIP_AUD_CTRL_FORMAT_CHAT = 0x01,
	GIP_AUD_CTRL_FORMAT = 0x02,
	GIP_AUD_CTRL_VOLUME = 0x03,
};

static const int gip_rates[] = {
	0, 8000, 8000, 12000, 12000, 16000, 16000, 20000, 20000,
	24000, 24000, 32000, 32000, 40000, 40000, 48000, 48000,
};

struct gip_header {
	uint8_t command;
	uint8_t options;
	uint8_t sequence;
	uint32_t length;
	uint32_t chunk_offset;
};

enum state {
	ST_WAIT_ANNOUNCE,
	ST_WAIT_IDENTIFY,
	ST_WAIT_FORMAT,
	ST_POWERING_ON,
	ST_STREAMING,
};

static const char *state_names[] = {
	"WAIT_ANNOUNCE", "WAIT_IDENTIFY", "WAIT_FORMAT", "POWERING_ON",
	"STREAMING",
};

static libusb_device_handle *dev;
static volatile sig_atomic_t stop;
static enum state state = ST_WAIT_ANNOUNCE;
static uint8_t data_seq = 1;
static uint8_t audio_seq;
static bool quiet_audio;
static bool play_tone;
static bool alt0_on_exit;
static bool skip_set_alt, send_auth_complete, send_auth_hello, graceful_exit;
static FILE *mic_file;

static uint8_t *chunk_buf;
static uint32_t chunk_len;
static uint8_t chunk_cmd;

static int fmt_in, fmt_out;
static int out_frag; /* bytes of samples per 1 ms packet */
static int out_pkt;  /* header + samples */
static double tone_phase;
static int power_tries;
static double next_power_at;
static bool got_volume, got_audio_in;
static unsigned long audio_in_pkts, audio_out_pkts, audio_out_errs;
static unsigned long mic_samples, mic_nonzero;
static int mic_peak, last_in_len = -1;
static double volume_at = -1;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double t0;

#define log(fmt, ...) printf("[%8.3f] " fmt "\n", now() - t0, ##__VA_ARGS__)

static void set_state(enum state s)
{
	log("state %s -> %s", state_names[state], state_names[s]);
	state = s;
}

static void hexdump(const char *prefix, const uint8_t *buf, int len)
{
	printf("[%8.3f] %s (%d):", now() - t0, prefix, len);
	for (int i = 0; i < len; i++)
		printf(" %02x", buf[i]);
	printf("\n");
}

static int encode_varint(uint8_t *buf, uint32_t val)
{
	int i;

	for (i = 0; i < 4; i++) {
		buf[i] = val & 0x7f;
		if (val > 0x7f)
			buf[i] |= 0x80;
		val >>= 7;
		if (!val)
			break;
	}

	return i + 1;
}

static int decode_varint(const uint8_t *data, int len, uint32_t *val)
{
	int i;

	*val = 0;
	for (i = 0; i < 4 && i < len; i++) {
		*val |= (uint32_t)(data[i] & 0x7f) << (i * 7);
		if (!(data[i] & 0x80))
			break;
	}

	return i + 1;
}

static int header_actual_len(const struct gip_header *hdr)
{
	uint32_t len = hdr->length, off = hdr->chunk_offset;
	int n = 3;

	do {
		n++;
		len >>= 7;
	} while (len);

	if (hdr->options & GIP_OPT_CHUNK) {
		while (off) {
			n++;
			off >>= 7;
		}
	}

	return n;
}

static int header_len(const struct gip_header *hdr)
{
	int n = header_actual_len(hdr);

	return n + (n % 2);
}

static int encode_header(const struct gip_header *hdr, uint8_t *buf)
{
	int n = 0;

	buf[n++] = hdr->command;
	buf[n++] = hdr->options;
	buf[n++] = hdr->sequence;
	n += encode_varint(buf + n, hdr->length);

	/* header length must be even */
	if (header_actual_len(hdr) % 2) {
		buf[n - 1] |= 0x80;
		buf[n++] = 0;
	}

	if (hdr->options & GIP_OPT_CHUNK)
		n += encode_varint(buf + n, hdr->chunk_offset);

	return header_len(hdr);
}

static int decode_header(struct gip_header *hdr, const uint8_t *data, int len)
{
	int n = 0;

	hdr->command = data[n++];
	hdr->options = data[n++];
	hdr->sequence = data[n++];
	hdr->chunk_offset = 0;
	n += decode_varint(data + n, len - n, &hdr->length);
	if (hdr->options & GIP_OPT_CHUNK)
		n += decode_varint(data + n, len - n, &hdr->chunk_offset);

	return n;
}

static void LIBUSB_CALL data_out_cb(struct libusb_transfer *xfer)
{
	if (xfer->status != LIBUSB_TRANSFER_COMPLETED)
		log("data out status %d", xfer->status);
}

static int send_pkt(uint8_t cmd, uint8_t options, uint8_t seq,
		    const void *payload, uint32_t len)
{
	struct gip_header hdr = {
		.command = cmd,
		.options = options,
		.sequence = seq,
		.length = len,
	};
	struct libusb_transfer *xfer;
	uint8_t buf[DATA_PKT_LEN], *copy;
	int n, err;

	if (len > GIP_PKT_MAX_LENGTH) {
		log("send_pkt: payload too long (%u), chunked send not needed yet",
		    len);
		return -EMSGSIZE;
	}

	if (!hdr.sequence) {
		hdr.sequence = data_seq++;
		if (!data_seq)
			data_seq = 1;
	}

	n = encode_header(&hdr, buf);
	if (len)
		memcpy(buf + n, payload, len);
	n += len;

	hexdump("OUT", buf, n);

	/* called from transfer callbacks, so it must not block */
	xfer = libusb_alloc_transfer(0);
	copy = malloc(n);
	memcpy(copy, buf, n);
	libusb_fill_interrupt_transfer(xfer, dev, EP_DATA_OUT, copy, n,
				       data_out_cb, NULL, 1000);
	xfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
	err = libusb_submit_transfer(xfer);
	if (err)
		log("send failed: %s", libusb_error_name(err));

	return err;
}

static void send_ack(const struct gip_header *hdr, uint32_t total)
{
	uint8_t pkt[9] = {};
	uint32_t len = hdr->chunk_offset + hdr->length;
	uint8_t client = hdr->options & 0x0f;

	pkt[1] = hdr->command;
	pkt[2] = client | GIP_OPT_INTERNAL;
	pkt[3] = len & 0xff;
	pkt[4] = len >> 8;
	if ((hdr->options & GIP_OPT_CHUNK) && total) {
		pkt[7] = (total - len) & 0xff;
		pkt[8] = (total - len) >> 8;
	}

	send_pkt(GIP_CMD_ACKNOWLEDGE, client | GIP_OPT_INTERNAL,
		 hdr->sequence, pkt, sizeof(pkt));
}

static void send_power(uint8_t mode)
{
	send_pkt(GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, &mode, 1);
}

static void send_format(int in, int out)
{
	uint8_t pkt[3] = { GIP_AUD_CTRL_FORMAT, in, out };

	send_pkt(GIP_CMD_AUDIO_CONTROL, GIP_OPT_INTERNAL, 0, pkt, sizeof(pkt));
}

static uint16_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static void parse_identify(const uint8_t *data, uint32_t len)
{
	const uint8_t *d;
	uint16_t off[8];
	static const char *names[] = {
		"client_commands", "firmware_versions", "audio_formats",
		"capabilities_out", "capabilities_in", "classes",
		"interfaces", "hid_descriptor",
	};

	hexdump("IDENTIFY raw", data, len);
	if (len < 32) {
		log("identify too short");
		return;
	}

	for (int i = 0; i < 8; i++) {
		off[i] = le16(data + 16 + 2 * i);
		log("  %-18s offset=0x%04x", names[i], off[i]);
	}

	/* offsets are relative to the end of the 16 byte unknown prefix */
	d = data + 16;
	len -= 16;

	if (off[2] && off[2] < len) {
		int count = d[off[2]];

		for (int i = 0; i < count && off[2] + 2 + 2 * i < len; i++) {
			int in = d[off[2] + 1 + 2 * i];
			int out = d[off[2] + 2 + 2 * i];

			log("  audio format #%d: in=0x%02x (%d Hz %s) out=0x%02x (%d Hz %s)",
			    i, in, in <= 16 ? gip_rates[in] : -1,
			    (in & 1) ? "mono" : "stereo", out,
			    out <= 16 ? gip_rates[out] : -1,
			    (out & 1) ? "mono" : "stereo");
			if (i == 0) {
				fmt_in = in;
				fmt_out = out;
			}
		}
	}

	if (off[5] && off[5] < len) {
		uint16_t o = off[5];
		int count = d[o++];

		for (int i = 0; i < count && o + 2 <= len; i++) {
			uint16_t sl = le16(d + o);

			o += 2;
			if (o + sl > len)
				break;
			log("  class: %.*s", sl, d + o);
			o += sl;
		}
	}

	if (off[6] && off[6] < len) {
		int count = d[off[6]];

		for (int i = 0; i < count; i++) {
			const uint8_t *g = d + off[6] + 1 + 16 * i;

			if (g + 16 > d + len)
				break;
			printf("[%8.3f]   interface guid:", now() - t0);
			for (int j = 0; j < 16; j++)
				printf("%02x", g[j]);
			printf("\n");
		}
	}
}

static void auth_send(const uint8_t *pkt, int len, bool ack)
{
	send_pkt(GIP_CMD_AUTHENTICATE,
		 GIP_OPT_INTERNAL | (ack ? GIP_OPT_ACKNOWLEDGE : 0), 0, pkt, len);
}

static void auth_complete(void)
{
	/* context CONTROL, control COMPLETE */
	uint8_t pkt[2] = { 0x01, 0x00 };

	log("auth: sending complete");
	auth_send(pkt, sizeof(pkt), false);
}

static void auth_hello(void)
{
	/*
	 * handshake header: context, options (ACK | FROM_HOST), error,
	 * command, be16 length; data header: command, version, be16 length;
	 * then 32 random bytes, 8 unknown bytes and an 8 byte trailer
	 */
	uint8_t pkt[58] = { 0x00, 0x41, 0x00, 0x01, 0x00, 44,
			    0x01, 0x01, 0x00, 40 };

	for (int i = 0; i < 32; i++)
		pkt[10 + i] = rand();
	log("auth: sending v1 host hello");
	auth_send(pkt, sizeof(pkt), true);
}

static void auth_request(uint8_t cmd, uint16_t len)
{
	/* options REQUEST | FROM_HOST, length includes the data header */
	uint8_t pkt[14] = { 0x00, 0x42, 0x00, cmd, (len + 4) >> 8,
			    (len + 4) & 0xff };

	log("auth: requesting packet 0x%02x (%u bytes)", cmd, len);
	auth_send(pkt, sizeof(pkt), true);
}

static void handle_auth(const uint8_t *data, uint32_t len)
{
	hexdump("AUTH from device", data, len);
	if (len < 6)
		return;
	log("auth: context=%u options=0x%02x error=%u command=0x%02x length=%u",
	    data[0], data[1], data[2], data[3], data[4] << 8 | data[5]);
	if (len >= 10 && !(data[1] & 0x01))
		log("auth: data command=0x%02x version=%u length=%u",
		    data[6], data[7], data[8] << 8 | data[9]);

	/* acknowledgment of our host hello: ask for the client hello */
	if ((data[1] & 0x01) && data[3] == 0x01 && send_auth_hello)
		auth_request(0x02, 80);
}

static void handle_audio_control(const uint8_t *data, uint32_t len)
{
	switch (data[0]) {
	case GIP_AUD_CTRL_FORMAT:
		log("audio format reply: in=0x%02x out=0x%02x", data[1], data[2]);
		if (state != ST_WAIT_FORMAT)
			break;
		if (data[1] != fmt_in || data[2] != fmt_out) {
			log("format rejected, accepting device suggestion");
			fmt_in = data[1];
			fmt_out = data[2];
			send_format(fmt_in, fmt_out);
			break;
		}
		out_frag = gip_rates[fmt_out] * (2 - (fmt_out & 1)) * 2 / 1000;
		{
			struct gip_header h = {
				.command = GIP_CMD_AUDIO_SAMPLES,
				.length = out_frag,
			};

			out_pkt = header_len(&h) + out_frag;
		}
		log("audio out: %d bytes/ms, packet %d bytes", out_frag, out_pkt);
		if (send_auth_hello)
			auth_hello();
		if (send_auth_complete)
			auth_complete();
		set_state(ST_POWERING_ON);
		power_tries = 0;
		next_power_at = 0;
		break;
	case GIP_AUD_CTRL_VOLUME:
		if (len >= 5)
			log("volume: mute=0x%02x out=%u chat=%u in=%u",
			    data[1], data[2], data[3], data[4]);
		got_volume = true;
		break;
	case GIP_AUD_CTRL_VOLUME_CHAT:
		log("chat volume report");
		got_volume = true;
		break;
	default:
		log("unknown audio control subcommand 0x%02x", data[0]);
	}
}

static void dispatch(const struct gip_header *hdr, const uint8_t *data,
		     uint32_t len)
{
	if (!(hdr->options & GIP_OPT_INTERNAL)) {
		hexdump("client cmd payload", data, len);
		return;
	}

	switch (hdr->command) {
	case GIP_CMD_ACKNOWLEDGE:
		log("ack from device");
		break;
	case GIP_CMD_ANNOUNCE:
		if (len >= 28)
			log("announce: vid=%04x pid=%04x fw=%u.%u.%u.%u hw=%u.%u.%u.%u",
			    le16(data + 8), le16(data + 10), le16(data + 12),
			    le16(data + 14), le16(data + 16), le16(data + 18),
			    le16(data + 20), le16(data + 22), le16(data + 24),
			    le16(data + 26));
		if (state == ST_WAIT_ANNOUNCE)
			set_state(ST_WAIT_IDENTIFY);
		/* repeated announces mean our identify request got lost */
		if (state == ST_WAIT_IDENTIFY && !chunk_buf)
			send_pkt(GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
		break;
	case GIP_CMD_STATUS:
		if (len >= 1)
			log("status: 0x%02x connected=%d batt_type=%d batt_lvl=%d",
			    data[0], !!(data[0] & 0x80), (data[0] >> 2) & 3,
			    data[0] & 3);
		break;
	case GIP_CMD_IDENTIFY:
		parse_identify(data, len);
		if (state == ST_WAIT_IDENTIFY && fmt_out) {
			set_state(ST_WAIT_FORMAT);
			send_power(GIP_PWR_SLEEP);
			send_format(fmt_in, fmt_out);
		}
		break;
	case GIP_CMD_AUTHENTICATE:
		handle_auth(data, len);
		break;
	case GIP_CMD_AUDIO_CONTROL:
		handle_audio_control(data, len);
		break;
	case GIP_CMD_EXTENDED:
		hexdump("extended", data, len);
		break;
	default:
		hexdump("unhandled internal cmd payload", data, len);
	}
}

static void process_pkt(struct gip_header *hdr, const uint8_t *data)
{
	if (hdr->options & GIP_OPT_CHUNK_START) {
		free(chunk_buf);
		chunk_len = hdr->chunk_offset;
		chunk_cmd = hdr->command;
		chunk_buf = calloc(1, chunk_len ? chunk_len : 1);
		hdr->chunk_offset = 0;
		log("chunked transfer cmd=0x%02x total=%u", chunk_cmd, chunk_len);
	}

	if (hdr->options & GIP_OPT_CHUNK) {
		uint32_t end = hdr->chunk_offset + hdr->length;

		if (!chunk_buf || hdr->command != chunk_cmd) {
			log("unexpected chunk");
			return;
		}

		if (hdr->length) {
			if (end > chunk_len) {
				log("chunk overflow");
				return;
			}
			if ((hdr->options & GIP_OPT_ACKNOWLEDGE) ||
			    end == chunk_len)
				send_ack(hdr, chunk_len);
			memcpy(chunk_buf + hdr->chunk_offset, data, hdr->length);
			return;
		}

		/* empty chunk completes the transfer */
		dispatch(hdr, chunk_buf, chunk_len);
		free(chunk_buf);
		chunk_buf = NULL;
		return;
	}

	if (hdr->options & GIP_OPT_ACKNOWLEDGE)
		send_ack(hdr, 0);

	dispatch(hdr, data, hdr->length);
}

static void process_buffer(const uint8_t *data, int len)
{
	struct gip_header hdr;
	int n;

	while (len > 3) {
		n = decode_header(&hdr, data, len);
		if (len < n + (int)hdr.length) {
			log("truncated packet");
			return;
		}
		log("IN  cmd=0x%02x opt=0x%02x seq=0x%02x len=%u off=%u",
		    hdr.command, hdr.options, hdr.sequence, hdr.length,
		    hdr.chunk_offset);
		process_pkt(&hdr, data + n);
		data += n + hdr.length;
		len -= n + hdr.length;
	}
}

/* ---------------------------------------------------------------------- */
/* asynchronous transfers                                                 */

static void LIBUSB_CALL data_in_cb(struct libusb_transfer *xfer)
{
	if (xfer->status == LIBUSB_TRANSFER_COMPLETED && xfer->actual_length) {
		hexdump("IN ", xfer->buffer, xfer->actual_length);
		process_buffer(xfer->buffer, xfer->actual_length);
	} else if (xfer->status != LIBUSB_TRANSFER_TIMED_OUT &&
		   xfer->status != LIBUSB_TRANSFER_COMPLETED) {
		log("data in status %d", xfer->status);
		if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
			stop = 1;
			return;
		}
	}

	if (!stop)
		libusb_submit_transfer(xfer);
}

static void fill_audio_out(struct libusb_transfer *xfer)
{
	int16_t samples[512];
	int channels = 2 - (fmt_out & 1);
	int frames = out_frag / (2 * channels);
	double step = 2 * M_PI * 440.0 / gip_rates[fmt_out];

	for (int p = 0; p < AUDIO_PKTS; p++) {
		uint8_t *dst = xfer->buffer + p * out_pkt;
		struct gip_header hdr = {
			.command = GIP_CMD_AUDIO_SAMPLES,
			.options = GIP_OPT_INTERNAL,
			.length = out_frag,
		};
		int n;

		if (!++audio_seq)
			++audio_seq;
		hdr.sequence = audio_seq;
		n = encode_header(&hdr, dst);

		for (int f = 0; f < frames; f++) {
			int16_t v = play_tone ? (int16_t)(sin(tone_phase) * 6000) : 0;

			for (int c = 0; c < channels; c++)
				samples[channels * f + c] = v;
			tone_phase += step;
		}
		memcpy(dst + n, samples, out_frag);
		xfer->iso_packet_desc[p].length = out_pkt;
	}
}

static void LIBUSB_CALL audio_out_cb(struct libusb_transfer *xfer)
{
	if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
		stop = 1;
		return;
	}
	if (xfer->status != LIBUSB_TRANSFER_COMPLETED)
		log("audio out transfer status %d", xfer->status);
	for (int p = 0; p < xfer->num_iso_packets; p++) {
		struct libusb_iso_packet_descriptor *d = &xfer->iso_packet_desc[p];

		if (d->status || d->actual_length != d->length)
			audio_out_errs++;
		else
			audio_out_pkts++;
	}
	if (stop)
		return;
	fill_audio_out(xfer);
	libusb_submit_transfer(xfer);
}

static void LIBUSB_CALL audio_in_cb(struct libusb_transfer *xfer)
{
	if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
		stop = 1;
		return;
	}

	for (int p = 0; p < xfer->num_iso_packets; p++) {
		struct libusb_iso_packet_descriptor *desc = &xfer->iso_packet_desc[p];
		uint8_t *buf = libusb_get_iso_packet_buffer_simple(xfer, p);
		struct gip_header hdr;
		int n;

		if (desc->status || !desc->actual_length)
			continue;

		audio_in_pkts++;
		if (!got_audio_in) {
			got_audio_in = true;
			hexdump("first audio IN packet", buf, desc->actual_length);
		} else if (!quiet_audio && audio_in_pkts % 1000 == 0) {
			hexdump("audio IN sample", buf, desc->actual_length);
		}

		n = decode_header(&hdr, buf, desc->actual_length);
		if (hdr.command != GIP_CMD_AUDIO_SAMPLES) {
			process_buffer(buf, desc->actual_length);
			continue;
		}
		/* payload: le16 flow rate, then samples */
		if (hdr.length > 2) {
			const int16_t *smp = (const int16_t *)(buf + n + 2);

			for (uint32_t i = 0; i < (hdr.length - 2) / 2; i++) {
				if (smp[i])
					mic_nonzero++;
				if (abs(smp[i]) > mic_peak)
					mic_peak = abs(smp[i]);
			}
			mic_samples += (hdr.length - 2) / 2;
			if (mic_file)
				fwrite(buf + n + 2, 1, hdr.length - 2, mic_file);
		}
		if ((int)desc->actual_length != last_in_len) {
			last_in_len = desc->actual_length;
			hexdump("audio IN length changed", buf, desc->actual_length);
		}
	}

	if (!stop)
		libusb_submit_transfer(xfer);
}

static int set_alt(int alt)
{
	double t = now();
	int err = libusb_set_interface_alt_setting(dev, INTF_AUDIO, alt);

	log("set alt %d: %s (%.0f ms)", alt, err ? libusb_error_name(err) : "ok",
	    (now() - t) * 1000);

	return err;
}

static int start_streaming(void)
{
	int err;

	if (skip_set_alt) {
		log("skipping SET_INTERFACE, assuming alt 1");
	} else {
		err = set_alt(1);
		if (err)
			return err;
	}

	for (int i = 0; i < AUDIO_XFERS; i++) {
		struct libusb_transfer *x = libusb_alloc_transfer(AUDIO_PKTS);
		uint8_t *buf = calloc(AUDIO_PKTS, out_pkt);

		libusb_fill_iso_transfer(x, dev, EP_AUDIO_OUT, buf,
					 AUDIO_PKTS * out_pkt, AUDIO_PKTS,
					 audio_out_cb, NULL, 1000);
		fill_audio_out(x);
		err = libusb_submit_transfer(x);
		if (err)
			log("audio out submit: %s", libusb_error_name(err));

		x = libusb_alloc_transfer(AUDIO_PKTS);
		buf = calloc(AUDIO_PKTS, AUDIO_IN_MAXP);
		libusb_fill_iso_transfer(x, dev, EP_AUDIO_IN, buf,
					 AUDIO_PKTS * AUDIO_IN_MAXP, AUDIO_PKTS,
					 audio_in_cb, NULL, 1000);
		libusb_set_iso_packet_lengths(x, AUDIO_IN_MAXP);
		err = libusb_submit_transfer(x);
		if (err)
			log("audio in submit: %s", libusb_error_name(err));
	}

	return 0;
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char **argv)
{
	struct libusb_transfer *data_in;
	uint8_t data_in_buf[DATA_PKT_LEN];
	double run_for = 10, last_stats = 0, stream_start = 0;
	bool reset = false;
	int opt, err;

	while ((opt = getopt(argc, argv, "tm:s:rqv:znaHg")) != -1) {
		switch (opt) {
		case 't':
			play_tone = true;
			break;
		case 'm':
			mic_file = fopen(optarg, "wb");
			if (!mic_file) {
				perror(optarg);
				return 1;
			}
			break;
		case 's':
			run_for = atof(optarg);
			break;
		case 'r':
			reset = true;
			break;
		case 'q':
			quiet_audio = true;
			break;
		case 'v':
			volume_at = atof(optarg);
			break;
		case 'z':
			alt0_on_exit = true;
			break;
		case 'n':
			skip_set_alt = true;
			break;
		case 'a':
			send_auth_complete = true;
			break;
		case 'H':
			send_auth_hello = true;
			break;
		case 'g':
			graceful_exit = true;
			break;
		default:
			fprintf(stderr,
				"usage: %s [-t] [-m mic.raw] [-s seconds] [-r] [-q] [-v sec] [-z] [-n] [-a] [-H] [-g]\n",
				argv[0]);
			return 1;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	t0 = now();

	err = libusb_init_context(NULL, NULL, 0);
	if (err) {
		fprintf(stderr, "libusb init: %s\n", libusb_error_name(err));
		return 1;
	}

	dev = libusb_open_device_with_vid_pid(NULL, A20_VID, A20_PID_GAME);
	if (!dev) {
		fprintf(stderr,
			"9886:0042 not found or no permission (is the dongle in game mode? udev rule installed?)\n");
		return 1;
	}

	if (reset) {
		log("resetting device");
		err = libusb_reset_device(dev);
		if (err)
			log("reset: %s", libusb_error_name(err));
	}

	libusb_set_auto_detach_kernel_driver(dev, 1);
	for (int i = INTF_DATA; i <= INTF_AUDIO; i++) {
		err = libusb_claim_interface(dev, i);
		if (err) {
			fprintf(stderr, "claim interface %d: %s\n", i,
				libusb_error_name(err));
			return 1;
		}
	}
	/*
	 * Do not touch the alt setting here: after a session the firmware
	 * wedges its control endpoint on SET_INTERFACE(alt 0).
	 */

	data_in = libusb_alloc_transfer(0);
	libusb_fill_interrupt_transfer(data_in, dev, EP_DATA_IN, data_in_buf,
				       sizeof(data_in_buf), data_in_cb, NULL, 0);
	libusb_submit_transfer(data_in);
	log("listening, waiting for announce...");

	while (!stop && now() - t0 < run_for) {
		struct timeval tv = { 0, 50000 };
		double t = now() - t0;

		libusb_handle_events_timeout_completed(NULL, &tv, NULL);

		/* the dongle may only announce on connect: ask explicitly */
		if (state == ST_WAIT_ANNOUNCE && t > 2.0) {
			log("no announce after 2 s, requesting identify anyway");
			set_state(ST_WAIT_IDENTIFY);
			send_pkt(GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
		}

		if (state == ST_POWERING_ON && t >= next_power_at) {
			if (got_volume || got_audio_in || power_tries > 6) {
				log("starting audio (volume=%d audio=%d tries=%d)",
				    got_volume, got_audio_in, power_tries);
				set_state(ST_STREAMING);
				stream_start = t;
				start_streaming();
			} else {
				power_tries++;
				log("power on, try %d", power_tries);
				send_power(GIP_PWR_ON);
				next_power_at = t + 0.5;
			}
		}

		if (state == ST_STREAMING && volume_at >= 0 &&
		    t - stream_start >= volume_at) {
			/* subcmd VOLUME: mute, out, chat, in, unknown[3] */
			uint8_t pkt[8] = { GIP_AUD_CTRL_VOLUME, 0x04, 100, 50, 100 };

			log("sending host volume command");
			send_pkt(GIP_CMD_AUDIO_CONTROL, GIP_OPT_INTERNAL, 0, pkt,
				 sizeof(pkt));
			volume_at = -1;
		}

		if (state == ST_STREAMING && t - last_stats >= 2.0) {
			last_stats = t;
			log("stats: audio out ok=%lu err=%lu, in pkts=%lu, mic samples=%lu nonzero=%lu peak=%d",
			    audio_out_pkts, audio_out_errs, audio_in_pkts,
			    mic_samples, mic_nonzero, mic_peak);
			mic_samples = mic_nonzero = 0;
			mic_peak = 0;
		}
	}

	log("stopping");
	stop = 1;
	{
		struct timeval tv = { 0, 200000 };

		/* let in-flight audio transfers drain before leaving alt 1 */
		libusb_handle_events_timeout_completed(NULL, &tv, NULL);
		if (state == ST_STREAMING && graceful_exit) {
			/* audio transfers are drained: power down, then leave alt 1 */
			stop = 0;
			send_power(GIP_PWR_SLEEP);
			for (int i = 0; i < 5; i++)
				libusb_handle_events_timeout_completed(NULL, &tv, NULL);
			stop = 1;
			set_alt(0);
		} else if (state == ST_STREAMING && alt0_on_exit) {
			set_alt(0);
		}

		/* end the session so the dongle announces again next time */
		if (state >= ST_WAIT_FORMAT) {
			stop = 0;
			send_power(GIP_PWR_SLEEP);
			libusb_handle_events_timeout_completed(NULL, &tv, NULL);
			stop = 1;
		}

		libusb_cancel_transfer(data_in);
		libusb_handle_events_timeout_completed(NULL, &tv, NULL);
	}
	libusb_release_interface(dev, INTF_AUDIO);
	libusb_release_interface(dev, INTF_DATA);
	libusb_close(dev);
	libusb_exit(NULL);
	if (mic_file)
		fclose(mic_file);

	return 0;
}
