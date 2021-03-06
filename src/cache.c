/**
 * Simple dictionary based filesystem cache.
 *
 * Keys are composed of "addr:port" combinations. Contents are cached on
 * filesystem where the first two characters of the key hash is a directory
 * and the remaining key hash is the filename. This creates a simple, yet
 * efficient, load balancing.
 */

#include "cache.h"


static uint8_t cache_basedir[PATH_MAXSIZ] = { 0 };


/*******************************************************************************
 *
 *  Internal helper functions
 *
 ******************************************************************************/

/**
 * Calculate hash based on "addr:port"
 */
static void
compute_hash (const uint8_t * key, uint8_t * hash)
{
    uint8_t tmphash[SHA_DIGEST_LENGTH] = { 0 };

    /* hash "addr:port" as cache key */
    SHA1 ((unsigned char *) key,
          strlen ((char *) key),
          (unsigned char *) tmphash);

    /* reformat to hexadecimal */
    for (uint8_t i = 0; i < SHA_DIGEST_LENGTH; i++) {
        sprintf ((char *) &(hash[i * 2]), "%02x", tmphash[i]);
    }
}


/**
 * Get cache directory name.
 */
static void
cache_dir (const uint8_t * hash, uint8_t * cache_dir_)
{
    /* path to base cache dir */
    strncat ((char *) cache_dir_, (char *) cache_basedir,
             strlen ((char *) cache_basedir));
    strncat ((char *) cache_dir_, "/", 1);
    /* two first hex digits are directory name */
    strncat ((char *) cache_dir_, (char *) hash, 2);
}


/**
 * Format cache file path.
 */
static void
cache_fpath (const uint8_t * hash, uint8_t * cache_file_path)
{
    uint8_t cache_dir_[PATH_MAXSIZ] = { 0 };

    cache_dir (hash, cache_dir_);

    strncat ((char *) cache_file_path, (char *) cache_dir_,
             strlen ((char *) cache_dir_));
    strncat ((char *) cache_file_path, "/", 2);
    /* last 18 hex digits are the file name */
    strncat ((char *) cache_file_path, (char *) hash + 2, HASHLEN - 2);
}


/*******************************************************************************
 *
 *  API
 *
 ******************************************************************************/

/**
 * Initialize the cache.
 *
 * Create cache directory if it does not already exist.
 *
 * Note this is not handling nestling of directories, i.e. mkdir -p.
 */
int
cache_init (const uint8_t * cache_basedir_input)
{
    struct stat     st;
    int             status = 0;

    /* setup cache basedir */
    strncat ((char *) cache_basedir, (char *) cache_basedir_input,
             strlen ((char *) cache_basedir_input));

    if (stat ((char *) cache_basedir, &st) != 0) {
        /* cache directory does not exist, create it */
        if (mkdir ((char *) cache_basedir, 0777) != 0 && errno != EEXIST) {
            status = -1;
        }
    } else if (!S_ISDIR (st.st_mode)) {
        errno = ENOTDIR;
        status = -1;
    }

    return status;
}


/**
 * Store buf in cache at key.
 */
int
cache_write (const uint8_t * key, const uint8_t * buf, const ssize_t buflen)
{
    uint8_t hash[HASHLEN] = { 0 };
    uint8_t cache_dir_[PATH_MAXSIZ] = { 0 };
    uint8_t cache_file_path[PATH_MAXSIZ] = { 0 };
    int fp;

    compute_hash (key, hash);
    cache_dir (hash, cache_dir_);
    cache_fpath (hash, cache_file_path);

    if (mkdir ((char *) cache_dir_, 0777) != 0 && errno != EEXIST) {
        /* could not create cache dir */
        return -1;
    }

    /* create cache file and store contents */
    fp = open ((char *) cache_file_path,
               O_WRONLY | O_CREAT,
               S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    write (fp, buf, buflen);
    fsync (fp); // ensure everything is flushed to disk
    close (fp);

    return 0;
}


/**
 * Send cache contents at "key" to supplied socket "socket".
 *
 * This uses sendfile(2) which shuffles all the data from file to socket in
 * kernel space.
 */
ssize_t
cache_sendfile (const int socket, const uint8_t * key)
{
    uint8_t     hash[HASHLEN] = { 0 };
    uint8_t     cache_file_path[PATH_MAXSIZ] = { 0 };
    FILE        *fp;
    size_t      fsize;
    ssize_t     sentbytes = 0;

    compute_hash (key, hash);
    cache_fpath (hash, cache_file_path);

    /* calculate cache content size */
    if ((fp = fopen ((char *) cache_file_path, "r")) == NULL) {
        /* cache miss */
        return 0;
    }

    /* cache hit, send to supplied socket */

    /* calculate cache file size */
    fseek (fp, 0, SEEK_END);
    fsize = ftell (fp);
    fclose (fp);

    int fd = open ((char *) cache_file_path, O_RDONLY);
    while (sentbytes < (ssize_t) fsize) {
        if ((sentbytes = sendfile (socket, fd, NULL, fsize)) == -1) {
            perror ("could not send from cache");
            break;
        }
    }

    close (fd);

    return sentbytes;
}
