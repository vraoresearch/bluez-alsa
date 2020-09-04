/*
 * server.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

struct spawn_bluealsa_data {
	pthread_mutex_t ready_count_mtx;
	pthread_cond_t ready_count_updated;
	unsigned int ready_count_a2dp;
	unsigned int ready_count_sco;
	FILE *output;
};

static char *bin_path = ".";

static void *spawn_bluealsa_server_stderr_proxy(void *userdata) {

	struct spawn_bluealsa_data *data = userdata;
	char buffer[512];

	while (fgets(buffer, sizeof(buffer), data->output) != NULL) {
		fputs(buffer, stderr);

		if (strncmp(buffer, "BLUEALSA_PCM_READY=A2DP:", 24) == 0) {
			pthread_mutex_lock(&data->ready_count_mtx);
			data->ready_count_a2dp++;
			pthread_cond_signal(&data->ready_count_updated);
			pthread_mutex_unlock(&data->ready_count_mtx);
		}

		if (strncmp(buffer, "BLUEALSA_PCM_READY=SCO:", 23) == 0) {
			pthread_mutex_lock(&data->ready_count_mtx);
			data->ready_count_sco++;
			pthread_cond_signal(&data->ready_count_updated);
			pthread_mutex_unlock(&data->ready_count_mtx);
		}

	}

	pthread_mutex_destroy(&data->ready_count_mtx);
	pthread_cond_destroy(&data->ready_count_updated);
	fclose(data->output);
	free(data);
	return NULL;
}

/**
 * Spawn bluealsa server mock.
 *
 * @param service BlueALSA D-Bus service name.
 * @param timeout Timeout passed to the server-mock.
 * @param wait_for_ready Block until PCMs are ready.
 * @param fuzzing Enable fuzzing - delayed startup.
 * @param source Start A2DP source.
 * @param sink Start A2DP sink.
 * @return PID of the bluealsa server mock. */
pid_t spawn_bluealsa_server(const char *service, unsigned int timeout,
	bool wait_for_ready, bool fuzzing, bool source, bool sink) {

	char arg_service[32] = "";
	if (service != NULL)
		sprintf(arg_service, "--dbus=%s", service);

	char arg_timeout[16];
	sprintf(arg_timeout, "--timeout=%d", timeout);

	char *argv[] = {
		"server-mock",
		arg_service,
		arg_timeout,
		fuzzing ? "--fuzzing" : "",
		source ? "--source" : "",
		sink ? "--sink" : "",
		NULL,
	};

	char path[256];
	sprintf(path, "%s/server-mock", bin_path);

	int fds[2];
	if (pipe(fds) == -1)
		return -1;

	pid_t pid;
	if ((pid = fork()) == 0) {
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		execv(path, argv);
	}

	close(fds[1]);

	struct spawn_bluealsa_data *data;
	unsigned int count_a2dp = 0;
	unsigned int count_sco = 0;
	char *address = NULL;
	char *ack = NULL;
	size_t size = 0;
	ssize_t len;

	if (source)
		count_a2dp += 2;
	if (sink)
		count_a2dp += 2;

	if ((data = calloc(1, sizeof(*data))) == NULL)
		return -1;

	pthread_mutex_init(&data->ready_count_mtx, NULL);
	pthread_cond_init(&data->ready_count_updated, NULL);
	if ((data->output = fdopen(fds[0], "r")) == NULL)
		return -1;

	/* first line contains env line with system bus address */
	if ((len = getline(&address, &size, data->output)) == -1)
		return -1;
	if (strncmp(address, "DBUS_SYSTEM_BUS_ADDRESS=", 24) == 0) {
		address[len - 1] = '\0';
		putenv(address);
	}

	/* second line acknowledges service name acquisition */
	if ((len = getline(&ack, &size, data->output)) == -1 ||
			strncmp(ack, "BLUEALSA_DBUS_SERVICE_NAME=", 27) != 0)
		return -1;
	free(ack);

	pthread_t tid;
	pthread_create(&tid, NULL, spawn_bluealsa_server_stderr_proxy, data);

	pthread_mutex_lock(&data->ready_count_mtx);
	while (wait_for_ready && (
				data->ready_count_a2dp < count_a2dp ||
				data->ready_count_sco < count_sco))
		pthread_cond_wait(&data->ready_count_updated, &data->ready_count_mtx);
	pthread_mutex_unlock(&data->ready_count_mtx);

	return pid;
}
