// SPDX-License-Identifier: MIT
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

int main(int argc, char **argv)
{
	struct pcm *tx = NULL, *rx = NULL;
	unsigned int card, device;

	DBusMessage *msg;
	DBusConnection *conn;
	DBusError err;

	if (argc != 2 || sscanf(argv[1], "hw:%u,%u", &card, &device) != 2) {
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
	dbus_connection_flush(conn);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "Match error: %s\n", err.message);
		dbus_error_free(&err);
		return 1;
	}

	printf("Listening for VoiceCallManager signals.\n");

	// Loop listening for signals being emmitted
	while (dbus_connection_read_write(conn, -1)) {
		// We need to process all received messages
		while (msg = dbus_connection_pop_message(conn)) {
			// Check if the message is a signal from the correct interface and with the correct name
			if (dbus_message_is_signal(msg, "org.ofono.VoiceCallManager", "CallAdded")) {
				if (!tx) {
					/*
					 * Opening the PCM devices starts the stream.
					 * This should be replaced by a codec2codec link probably.
					 */
					tx = pcm_open(card, device, PCM_IN, &pcm_config_voice_call);
					if (!pcm_is_ready(tx))
						perror("Failed to open tx");

					rx = pcm_open(card, device, PCM_OUT, &pcm_config_voice_call);
					if (!pcm_is_ready(rx))
						perror("Failed to open rx");

					printf("PCM devices were opened.\n");
				} else
					printf("PCM is already opened!\n");

			} else if (dbus_message_is_signal(msg, "org.ofono.VoiceCallManager", "CallRemoved")) {
				if (rx) {
					pcm_close(rx);
					pcm_close(tx);

					printf("PCM devices were closed.\n");
					rx = tx = NULL;
				} else
					printf("PCM is already closed!\n");
			}

			dbus_message_unref(msg);
		}
	}

	return 0;
}
