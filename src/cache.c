/**
 * Simple dictionary based filesystem cache.
 *
 * Keys are composed of "addr:port" combinations. Contents are cached on
 * filesystem where the first two characters of the key hash is a directory and
 * the remaining key hash is the filename. This creates a simple, yet
 * efficient, load balancing.
 */

#include "cache.h"


static uint8_t cache_basedir[PATH_MAXSIZ + 1] = { 0 };


/**
 * Calculate hash based on "addr:port"
 */
void
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

    fprintf (stdout, "compute_hash: %s\n", hash);
}


/**
 * Get cache directory name.
 */
void
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
void
cache_fpath (const uint8_t * hash, uint8_t * cache_file_path)
{
    uint8_t cache_dir_[PATH_MAXSIZ + 1] = { 0 };

    cache_dir (hash, cache_dir_);

    fprintf (stdout, "fpath: hash = %s\n", hash);

    strncat ((char *) cache_file_path, (char *) cache_dir_, strlen ((char *) cache_dir_));
    strncat ((char *) cache_file_path, "/", 1);
    /* last 18 hex digits are the file name */
    strncat ((char *) cache_file_path, (char *) hash + 2, HASHLEN - 2);

    fprintf (stdout, "%s <--\n", cache_file_path);
}

/**
 * Initialize the cache.
 *
 * Create cache directory if it does not already exist.
 *
 * Note this is not handling nestling of directories, i.e. mkdir -p.
 */
int
cache_init (const uint8_t *dir)
{
    struct stat     st;
    int             status = 0;

    /* setup cache_basedir */
    strncat ((char *) cache_basedir, (char *) dir, strlen ((char *) dir));

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
 * Calculate cache file size.
 *
 * Note! This is without error handling for missing keys, perform
 * cache_lookup() first to ensure cache existance.
 */
size_t
cache_fsize (const uint8_t * key)
{
    uint8_t hash[HASHLEN]  = { 0 };
    uint8_t cache_file_path[PATH_MAXSIZ + 1] = { 0 };
    FILE *fp;
    size_t fsize;

    compute_hash (key, hash);
    cache_fpath (hash, cache_file_path);

    fp = fopen ((char *) cache_file_path, "r");
    fseek (fp, 0, SEEK_END);
    fsize = ftell (fp);
    fclose (fp);

    return fsize;
}


/**
 * Perform a cache lookup.
 */
int
cache_lookup (const uint8_t * key)
{
    uint8_t hash[HASHLEN] = { 0 };
    uint8_t cache_file_path[PATH_MAXSIZ + 1] = { 0 };
    struct stat st;

    compute_hash (key, hash);
    fprintf (stdout, "lookup: hash = %s\n", hash);
    cache_fpath (hash, cache_file_path);

    if (stat ((char *) cache_file_path, &st) != 0) {
        /* cache miss, file does not exist */
        fprintf (stdout, "cache missssy\n");
        fprintf (stdout, "%s\n", cache_file_path);
        perror ("lol");
        return 0;
    }

    fprintf (stdout, "cache hitty\n");
    fprintf (stdout, "S_ISREG (st.st_mode) = %d\n", S_ISREG (st.st_mode));
    /* cache hit, if file exists and is a regular file */
    return S_ISREG (st.st_mode);
}


/**
 * Store buf in cache at key.
 */
int
cache_write (const uint8_t * key, const uint8_t * buf)
{
    uint8_t hash[HASHLEN] = { 0 };
    uint8_t cache_dir_[PATH_MAXSIZ + 1] = { 0 };
    uint8_t cache_file_path[PATH_MAXSIZ + 1] = { 0 };
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
    write (fp, buf, strlen ((char *) buf));
    fsync (fp); // ensure everything is flushed to disk
    close (fp);

    fprintf (stdout, "cache write\n");

    return 0;
}

/**
 * Open existing cache file, return file descriptor
 *
 * Note! Data must exist, perform cache_lookup() first.
 */
int
cache_open (const uint8_t *key)
{
    uint8_t hash[HASHLEN] = { 0 };
    uint8_t cache_file_path[PATH_MAXSIZ + 1] = { 0 };

    compute_hash (key, hash);
    cache_fpath (hash, cache_file_path);

    return open ((char *) cache_file_path, O_RDONLY);
}
