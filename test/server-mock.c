/*
 * server-mock.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server. When
 * connecting to the bluealsa device, one should use "hci-mock" interface.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include "inc/dbus.inc"
#include "inc/sine.inc"

#include "../src/a2dp.c"
#include "../src/a2dp-audio.c"
#include "../src/at.c"
#include "../src/audio.c"
#include "../src/ba-adapter.c"
#include "../src/ba-device.c"
#include "../src/ba-rfcomm.c"
#include "../src/ba-transport.c"
#include "../src/bluealsa-dbus.c"
#include "../src/bluealsa-iface.c"
#include "../src/bluealsa.c"
#include "../src/dbus.c"
#include "../src/hci.c"
#include "../src/msbc.c"
#include "../src/sbc.c"
#include "../src/sco.c"
#include "../src/utils.c"
#include "../src/shared/ffb.c"
#include "../src/shared/log.c"
#include "../src/shared/rt.c"

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

static struct ba_adapter *a = NULL;
static const char *service = "org.bluealsa";
static unsigned int timeout = 5;
static bool fuzzing = false;
static bool source = false;
static bool sink = false;
static bool sco = false;

static bool main_loop_on = true;
static gboolean main_loop_exit_handler(void *userdata) {
	main_loop_on = false;
	g_main_loop_quit((GMainLoop *)userdata);
	return G_SOURCE_REMOVE;
}

static int sigusr1_count = 0;
static int sigusr2_count = 0;
static void test_sigusr_handler(int sig) {
	switch (sig) {
	case SIGUSR1:
		sigusr1_count++;
		debug("Dispatching SIGUSR1: %d", sigusr1_count);
		break;
	case SIGUSR2:
		sigusr2_count++;
		debug("Dispatching SIGUSR2: %d", sigusr2_count);
		break;
	default:
		error("Unsupported signal: %d", sig);
	}
}

bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s", __func__, current_dbus_sep_path); (void)sep;
	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Not supported");
	return false;
}

static void *test_a2dp_sink_sbc(struct ba_transport *t) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct asrsync asrs = { .frames = 0 };
	int16_t buffer[1024 * 2];
	int x = 0;

	while (sigusr1_count == 0) {

		if (t->a2dp.pcm.fd == -1) {
			usleep(10000);
			continue;
		}

		fprintf(stderr, ".");

		if (asrs.frames == 0)
			asrsync_init(&asrs, t->a2dp.pcm.sampling);

		int samples = sizeof(buffer) / sizeof(int16_t);
		x = snd_pcm_sine_s16le(buffer, samples, 2, x, 1.0 / 128);

		if (ba_transport_pcm_write(&t->a2dp.pcm, buffer, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		asrsync_sync(&asrs, samples / 2);
	}

	pthread_cleanup_pop(1);
	return NULL;
}

static int test_transport_acquire(struct ba_transport *t) {

	int bt_fds[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds) == 0);

	t->bt_fd = bt_fds[0];
	t->mtu_read = 256;
	t->mtu_write = 256;

	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		assert(ba_transport_pthread_create(t, a2dp_source_sbc, "ba-a2dp") == 0);
	else if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		assert(ba_transport_pthread_create(t, test_a2dp_sink_sbc, "ba-a2dp") == 0);
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		assert(ba_transport_pthread_create(t, sco_thread, "ba-sco") == 0);

	return 0;
}

static int test_transport_release(struct ba_transport *t) {
	if (t->bt_fd != -1)
		close(t->bt_fd);
	t->bt_fd = -1;
	return 0;
}

static struct ba_transport *test_transport_new_a2dp(struct ba_device *d,
		struct ba_transport_type type, const char *owner, const char *path,
		const struct a2dp_codec *codec, const void *configuration) {
	if (fuzzing)
		sleep(1);
	struct ba_transport *t = ba_transport_new_a2dp(d, type, owner, path, codec, configuration);
	t->acquire = test_transport_acquire;
	t->release = test_transport_release;
	return t;
}

static struct ba_transport *test_transport_new_sco(struct ba_device *d,
		struct ba_transport_type type, const char *owner, const char *path) {
	if (fuzzing)
		sleep(1);
	struct ba_transport *t = ba_transport_new_sco(d, type, owner, path, -1);
	t->acquire = test_transport_acquire;
	t->release = test_transport_release;
	return t;
}

void *test_bt_mock(void *userdata) {

	bdaddr_t addr;
	struct ba_device *d1, *d2;
	struct ba_transport *t1d1 = NULL, *t2d1 = NULL, *t3d1 = NULL;
	struct ba_transport *t1d2 = NULL, *t2d2 = NULL, *t3d2 = NULL;
	GMainLoop *loop = userdata;

	str2ba("12:34:56:78:9A:BC", &addr);
	assert((d1 = ba_device_new(a, &addr)) != NULL);

	str2ba("12:34:56:9A:BC:DE", &addr);
	assert((d2 = ba_device_new(a, &addr)) != NULL);

	if (source) {
		struct ba_transport_type ttype = {
			.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
			.codec = A2DP_CODEC_SBC };
		assert((t1d1 = test_transport_new_a2dp(d1, ttype, ":test", "/source/1",
					&a2dp_codec_source_sbc, &config_sbc_44100_stereo)) != NULL);
		assert((t1d2 = test_transport_new_a2dp(d2, ttype, ":test", "/source/2",
					&a2dp_codec_source_sbc, &config_sbc_44100_stereo)) != NULL);
	}

	if (sink) {
		struct ba_transport_type ttype = {
			.profile = BA_TRANSPORT_PROFILE_A2DP_SINK,
			.codec = A2DP_CODEC_SBC };
		assert((t2d1 = test_transport_new_a2dp(d1, ttype, ":test", "/sink/1",
						&a2dp_codec_sink_sbc, &config_sbc_44100_stereo)) != NULL);
		assert(t2d1->acquire(t2d1) == 0);
		assert((t2d2 = test_transport_new_a2dp(d2, ttype, ":test", "/sink/2",
						&a2dp_codec_sink_sbc, &config_sbc_44100_stereo)) != NULL);
		assert(t2d2->acquire(t2d2) == 0);
	}

	if (sco) {
		struct ba_transport_type ttype = { .profile = BA_TRANSPORT_PROFILE_HSP_AG };
		assert((t3d1 = test_transport_new_sco(d1, ttype, ":test", "/sco/1")) != NULL);
		ttype.profile = BA_TRANSPORT_PROFILE_HFP_AG;
		assert((t3d2 = test_transport_new_sco(d2, ttype, ":test", "/sco/2")) != NULL);
		if (fuzzing) {
			t3d2->type.codec = HFP_CODEC_CVSD;
			bluealsa_dbus_pcm_update(&t3d2->sco.spk_pcm,
					BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
			bluealsa_dbus_pcm_update(&t3d2->sco.mic_pcm,
					BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
		}
	}

	ba_device_unref(d1);
	ba_device_unref(d2);

	while (timeout != 0 && main_loop_on)
		timeout = sleep(timeout);

	if (t1d1 != NULL)
		ba_transport_destroy(t1d1);
	if (t2d1 != NULL)
		ba_transport_destroy(t2d1);
	if (t3d1 != NULL)
		ba_transport_destroy(t3d1);

	if (fuzzing)
		sleep(1);

	if (t1d2 != NULL)
		ba_transport_destroy(t1d2);
	if (t2d2 != NULL)
		ba_transport_destroy(t2d2);
	if (t3d2 != NULL)
		ba_transport_destroy(t3d2);

	if (fuzzing)
		sleep(1);

	g_main_loop_quit(loop);
	return NULL;
}

static void dbus_name_acquired(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;
	GMainLoop *loop = userdata;

	fprintf(stderr, "BLUEALSA_DBUS_SERVICE_NAME=%s\n", name);

	/* emulate dummy test HCI device */
	assert((a = ba_adapter_new(0)) != NULL);

	/* do not generate lots of data */
	config.sbc_quality = SBC_QUALITY_LOW;

	/* run actual BlueALSA mock thread */
	g_thread_new(NULL, test_bt_mock, loop);

}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hb:t:F";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "dbus", required_argument, NULL, 'b' },
		{ "timeout", required_argument, NULL, 't' },
		{ "fuzzing", no_argument, NULL, 'F' },
		{ "source", no_argument, NULL, 1 },
		{ "sink", no_argument, NULL, 2 },
		{ "sco", no_argument, NULL, 3 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [--source] [--sink] [--sco] [--timeout SEC]\n", argv[0]);
			return EXIT_SUCCESS;
		case 1:
			source = true;
			break;
		case 2:
			sink = true;
			break;
		case 3:
			sco = true;
			break;
		case 'b':
			service = optarg;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		case 'F':
			fuzzing = true;
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	assert(bluealsa_config_init() == 0);
	assert((config.dbus = g_test_dbus_connection_new_sync(NULL)) != NULL);

	/* receive EPIPE error code */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* register USR signals handler */
	sigact.sa_handler = test_sigusr_handler;
	sigaction(SIGUSR1, &sigact, NULL);
	sigaction(SIGUSR2, &sigact, NULL);

	/* main loop with gracefull termination handlers */
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, main_loop_exit_handler, loop);
	g_unix_signal_add(SIGTERM, main_loop_exit_handler, loop);

	assert(bluealsa_dbus_manager_register(NULL) != 0);
	assert(g_bus_own_name_on_connection(config.dbus, service,
				G_BUS_NAME_OWNER_FLAGS_NONE, dbus_name_acquired, NULL, loop, NULL) != 0);

	g_main_loop_run(loop);

	ba_adapter_unref(a);
	return EXIT_SUCCESS;
}
