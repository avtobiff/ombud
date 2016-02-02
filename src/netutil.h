#pragma once

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>


extern int mk_nonblock (const int socket);

extern int setup_listener(const uint8_t * server_port);

extern int sendall (const int socket, const uint8_t * buf, size_t * buflen);
