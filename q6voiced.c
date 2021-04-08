// SPDX-License-Identifier: MIT
#include <stdbool.h>
#include <stdio.h>
#include <dbus/dbus.h>
#include <tinyalsa/asoundlib.h>

/* Note: These parameters have little relevance (no audio data written) */
struct pcm_config pcm_config_voice_call = {
	.channels	= 1,
	.rate		= 8000,
	.period_size	= 160,
	.period_count	= 2,
	.format		= PCM_FORMAT_S16_LE,
};

struct q6voiced {
	unsigned int card, device;
	struct pcm *tx, *rx;
};

static void q6voiced_open(struct q6voiced *v)
{
	if (v->tx)
		return; /* Already active */

	/*
	 * Opening the PCM devices starts the stream.
	 * This should be replaced by a codec2codec link probably.
	 */
	v->tx = pcm_open(v->card, v->device, PCM_IN, &pcm_config_voice_call);
	v->rx = pcm_open(v->card, v->device, PCM_OUT, &pcm_config_voice_call);
	if (!pcm_is_ready(v->tx) || pcm_prepare(v->tx))
		perror("Failed to open tx");
	if (!pcm_is_ready(v->rx) || pcm_prepare(v->rx))
		perror("Failed to open rx");

	printf("PCM devices were opened.\n");
}

static void q6voiced_close(struct q6voiced *v)
{
	if (!v->tx)
		return; /* Not active */

	pcm_close(v->rx);
	pcm_close(v->tx);
	v->rx = v->tx = NULL;

	printf("PCM devices were closed.\n");
}

/* See ModemManager-enums.h */
enum MMCallState {
	MM_CALL_STATE_DIALING		= 1,
	MM_CALL_STATE_RINGING_OUT	= 2,
	MM_CALL_STATE_ACTIVE		= 4,
};

static bool mm_state_is_active(int state)
{
	/*
	 * Some modems seem to be incapable of reporting DIALING -> ACTIVE.
	 * Therefore we also consider DIALING/RINGING_OUT as active.
	 */
	switch (state) {
	case MM_CALL_STATE_DIALING:
	case MM_CALL_STATE_RINGING_OUT:
	case MM_CALL_STATE_ACTIVE:
		return true;
	default:
		return false;
	}
}

static void handle_signal(struct q6voiced *v, DBusMessage *msg, DBusError *err)
{
	// Check if the message is a signal from the correct interface and with the correct name
	// TODO: Should we also check the call state for oFono?
	if (dbus_message_is_signal(msg, "org.ofono.VoiceCallManager", "CallAdded")) {
		q6voiced_open(v);
	} else if (dbus_message_is_signal(msg, "org.ofono.VoiceCallManager", "CallRemoved")) {
		q6voiced_close(v);
	} else if (dbus_message_is_signal(msg, "org.freedesktop.ModemManager1.Call", "StateChanged")) {
		/*
		 * For ModemManager call objects are created in advance
		 * and not necessarily immediately started.
		 * Need to listen for call state changes.
		 */
		int old_state, new_state;

		if (!dbus_message_get_args(msg, err,
					   DBUS_TYPE_INT32, &old_state,
					   DBUS_TYPE_INT32, &new_state,
					   DBUS_TYPE_INVALID))
			return;

		if (old_state == new_state)
			return; /* No change */

		if (mm_state_is_active(new_state))
			q6voiced_open(v);
		else if (mm_state_is_active(old_state) && !mm_state_is_active(new_state))
			q6voiced_close(v);
	}
}

int main(int argc, char **argv)
{
	struct q6voiced v = {0};

	DBusMessage *msg;
	DBusConnection *conn;
	DBusError err;

	if (argc != 2 || sscanf(argv[1], "hw:%u,%u", &v.card, &v.device) != 2) {
		fprintf(stderr, "Usage: q6voiced hw:<card>,<device>\n");
		return 1;
	}

	// See: http://web.archive.org/web/20100309103206/http://dbus.freedesktop.org/doc/dbus/libdbus-tutorial.html
	// "Receiving a Signal"

	dbus_error_init(&err);

	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "Connection error: %s\n", err.message);
		dbus_error_free(&err);
		return 1;
	}
	if (!conn)
		return 1;

	dbus_bus_add_match(conn, "type='signal',interface='org.ofono.VoiceCallManager'", &err);
	dbus_bus_add_match(conn, "type='signal',interface='org.freedesktop.ModemManager1.Call'", &err);
	dbus_connection_flush(conn);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "Match error: %s\n", err.message);
		dbus_error_free(&err);
		return 1;
	}

	// Loop listening for signals being emmitted
	while (dbus_connection_read_write(conn, -1)) {
		// We need to process all received messages
		while (msg = dbus_connection_pop_message(conn)) {
			handle_signal(&v, msg, &err);
			if (dbus_error_is_set(&err)) {
				fprintf(stderr, "Failed to handle signal: %s\n", err.message);
				dbus_error_free(&err);
			}

			dbus_message_unref(msg);
		}
	}

	return 0;
}
