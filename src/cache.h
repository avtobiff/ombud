#pragma once

#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


#define HASHLEN     SHA_DIGEST_LENGTH * 2
#define PATH_MAXSIZ 1024


extern int cache_init (const uint8_t * cache_basedir);

extern int cache_write (const uint8_t * key, const uint8_t * buf,
                        const ssize_t buflen);

extern ssize_t cache_sendfile (const int socket, const uint8_t * key);
