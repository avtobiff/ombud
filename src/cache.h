#pragma once

#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


#define HASHLEN     SHA_DIGEST_LENGTH * 2
#define PATH_MAXSIZ 1024


extern int cache_init (const uint8_t * dir);

extern int cache_lookup (const uint8_t * key);

extern int cache_write (const uint8_t * key, const uint8_t * buf);

extern size_t cache_fsize (const uint8_t * key);

extern int cache_open (const uint8_t * key);
