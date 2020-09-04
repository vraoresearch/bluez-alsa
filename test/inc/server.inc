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

static char *bin_path = ".";

static void *spawn_bluealsa_server_stderr_proxy(void *data) {

	FILE *f = (FILE *)data;
	char buffer[512];

	while (fgets(buffer, sizeof(buffer), f) != NULL)
		fputs(buffer, stderr);

	fclose(f);
	return NULL;
}

/**
 * Spawn bluealsa server mock.
 *
 * @param service BlueALSA D-Bus service name.
 * @param timeout Timeout passed to the server-mock.
 * @param fuzzing Enable fuzzing - delayed startup.
 * @param source Start A2DP source.
 * @param sink Start A2DP sink.
 * @return PID of the bluealsa server mock. */
pid_t spawn_bluealsa_server(const char *service, unsigned int timeout, bool fuzzing, bool source, bool sink) {

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

	FILE *f = fdopen(fds[0], "r");
	char *address = NULL;
	char *ack = NULL;
	size_t size = 0;
	ssize_t len;

	/* first line contains env line with system bus address */
	if ((len = getline(&address, &size, f)) == -1)
		return -1;
	if (strncmp(address, "DBUS_SYSTEM_BUS_ADDRESS=", 24) == 0) {
		address[len - 1] = '\0';
		putenv(address);
	}

	/* second line acknowledges service name acquisition */
	if ((len = getline(&ack, &size, f)) == -1 ||
			strncmp(ack, "BLUEALSA_DBUS_SERVICE_NAME=", 27) != 0)
		return -1;
	free(ack);

	pthread_t tid;
	pthread_create(&tid, NULL, spawn_bluealsa_server_stderr_proxy, f);

	return pid;
}
