#include <assert.h>
#include <err.h>
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

#include "cache.h"


#define DEFAULT_PORT    "8090"
#define BUFSIZE         8192

#define CACHE_BASEDIR    "cache-ombud" /* TODO make this configurable */
#define ADDR_PORT_STRLEN 22


/* socket sets and highest file descriptor, for use with select() */
static fd_set   sockset,
                ctrlsocks,
                datasocks;

static int      fdmax;


/**
 * Make supplied socket non-blocking.
 */
static int
mk_nonblock (int socket)
{
    int flags = fcntl (socket, F_GETFL);
    if (flags < 0) {
        return flags;
    } else {
        flags = fcntl (socket, F_SETFL, flags | O_NONBLOCK);
        return flags;
    }

    return -1;
}


/**
 * Setup listen socket and bind to it.
 */
static int
setup_listener(const uint8_t *server_port)
{
    struct addrinfo     hints,
                        *srvinfo,
                        *c;

    int                 listensock;

    /* configure listen connection */
    bzero (&hints, sizeof (struct addrinfo));
    hints.ai_family   = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;    /* TCP  */
    hints.ai_flags    = AI_PASSIVE;     /* all interfaces */

    /* get server address info for our configuration */
    int r = getaddrinfo (NULL, (char *) server_port, &hints, &srvinfo);
    if (r != 0) {
        err (1, "getaddrinfo: %s\n", gai_strerror (r));
    }

    /**
     * loop through possible addr configs and try to open listen socket and
     * bind it
     */
    for (c = srvinfo; c != NULL; c = c->ai_next) {
        /* setup a non-blocking listen socket */
        listensock = socket (c->ai_family, c->ai_socktype, c->ai_protocol);
        if (mk_nonblock (listensock) < 0) {
            err (1, "could not make listen socket nonblocking");
        }

        if (listensock < 0) {
            perror ("could not open socket");
            continue;
        }

        /* enable reuse of sockets busy in TIME_WAIT */
        int true_ = true;
        if (setsockopt (listensock, SOL_SOCKET, SO_REUSEADDR, &true_,
                        sizeof (int)) < 0) {
            err (1, "could not set SO_REUSEADDR on listen socket");
        }

        if (bind (listensock, c->ai_addr, c->ai_addrlen) < 0) {
            close (listensock);
            perror("could not bind socket");
            continue;
        }

        /* socket bound */
        break;
    }

    if (c == NULL) {
        fprintf (stderr, "Could not bind socket on any address\n");
        exit (EXIT_FAILURE);
    }

    freeaddrinfo (srvinfo);

    if (listen (listensock, SOMAXCONN) < 0) {
        err (1, "could not listen");
    }

    return listensock;
}


/**
 * Accept incoming connection.
 */
static void
do_accept (int listensock)
{
    int                         client_socket;
    struct sockaddr_storage     peer_addr;
    socklen_t                   peer_addr_len;


    peer_addr_len = sizeof (peer_addr);
    client_socket = accept (listensock, (struct sockaddr *) &peer_addr,
                            &peer_addr_len);

    /* enable non-blocking socket */
    if (mk_nonblock (client_socket) < 0) {
        err (1, "could not make control socket non-blocking");
    }

    if (client_socket < 0) {
        perror ("could not accept");
    } else {
        /* add accepted client socket to socket set */
        FD_SET (client_socket, &sockset);

        /* update fdmax if client_socket is larger */
        if (client_socket > fdmax) {
            fdmax = client_socket;
        }
    }
}


static int
sendall (int socket, char *buf, size_t *buflen)
{
    int32_t   sentbytes = 0,
              bytesleft = *buflen,
              numbytes = 0;

    while (numbytes < (int32_t) *buflen) {
        numbytes = send (socket, buf + sentbytes, bytesleft, 0);
        if (numbytes < 0) { break; }
        sentbytes += numbytes;
        bytesleft -= numbytes;
    }

    /* set buflen, i.e. "return value" */
    *buflen = sentbytes;

    return numbytes == -1 ? -1 : 0;
}


int
main (int argc, char *argv[])
{
    int                         listensock;

    ssize_t                     numbytes;

    uint8_t                     *server_port;


    /* get (valid) port from command line or use default port */
    if (argc == 2 && atoi (argv[1]) < 65536) {
        size_t portlen = strlen (argv[1]);
        server_port = calloc (1, portlen);
        strncat ((char *) server_port, argv[1], portlen);
    } else {
        server_port = calloc (1, 5);
        strcat ((char *) server_port, DEFAULT_PORT);
    }

    /* setup listen socket */
    if ((listensock = setup_listener (server_port)) < 0) {
        err (1, "Could not setup listen socket");
    }

    fprintf (stdout, "Listening on port %s...\n", (char *) server_port);
    free (server_port);

    /* initialize cache */
    if (cache_init ((const uint8_t *) CACHE_BASEDIR) < 0) {
        fprintf (stderr, "could not create cache dir\n");
        exit (EXIT_FAILURE);
    }

    /* clear socket sets */
    FD_ZERO (&sockset);
    FD_ZERO (&ctrlsocks);
    FD_ZERO (&datasocks);

    /* add listensock to client socket set, largest fd is listensock so far */
    FD_SET (listensock, &sockset);
    fdmax = listensock;

    fprintf (stdout, "Entering main loop...\n");
    for (;;) {
        /* copy socket set for use with select */
        ctrlsocks = sockset;
        datasocks = sockset;

        fprintf (stdout, "Another lap in the loop...\n");
        if (select (fdmax + 1, &ctrlsocks, NULL, NULL, NULL) < 0) {
            err (1, "select sockset");
        }

        /* loop through all existing connections for data to read */
        for (int sk = 0; sk <= fdmax; sk++) {
            /* found one socket to read from */
            if (FD_ISSET (sk, &ctrlsocks)) {
                if (sk == listensock) {
                    /* accept new connection */
                    do_accept (sk);
                } else {
                    /* handle data from client or remote host */
                    uint8_t buf[BUFSIZE] = { 0 };
                    if ((numbytes = recv(sk, buf, sizeof (buf), 0)) <= 0) {
                        /* connection closed or socket error */
                        if (numbytes == 0) {
                            fprintf (stdout, "socket %d closed\n", sk);
                        } else {
                            perror ("control recv error");
                        }

                        /* close socket and remove it from socket set */
                        close (sk);
                        FD_CLR (sk, &sockset);
                    } else {
                        /* connect to client supplied service */

                        uint8_t *tok, *s, *orig, srv[3][16] = {0}, i = 0;

                        /* save original string for freeing */
                        orig = s = (uint8_t *) strdup((char *) buf);
                        assert (s != NULL);

                        while ((tok = (uint8_t*) strsep (
                                        (char **) &s, ":\r\n")) != NULL) {
                            strcpy ((char *) srv[i++], (char *) tok);
                        }

                        free (orig);

                        /* cache lookup of hash (srv[1]:srv[2]) else dl */

                        uint8_t addr_port[ADDR_PORT_STRLEN] = { 0 };

                        strncat ((char *) addr_port, (char *) srv[0], strlen ((char *) srv[0]));
                        strncat ((char *) addr_port, ":", 1);
                        strncat ((char *) addr_port, (char *) srv[1], strlen ((char *) srv[1]));


                        /**
                         * send from cache, otherwise connect to given address
                         * and port, download data, write to cache, relay back
                         * to client.
                         */
                        if (cache_sendfile (sk, addr_port)) {
                            fprintf (stdout, "cache hit\n");
                        } else {
                            fprintf (stdout, "cache miss\n");

                            uint8_t buf[BUFSIZE] = { 0 };
                            struct addrinfo hints, *remoteinfo, *rp;

                            bzero (&hints, sizeof (struct addrinfo));
                            hints.ai_family   = AF_UNSPEC;
                            hints.ai_socktype = SOCK_STREAM;

                            int r;
                            if ((r = getaddrinfo ((char *) srv[0], (char *) srv[1],
                                                      &hints, &remoteinfo)) != 0) {
                                fprintf (stderr, "getaddrinfo: %s\n",
                                         gai_strerror (r));
                                continue;
                            }

                            int dsk;
                            for (rp = remoteinfo; rp != NULL; rp++) {
                                if ((dsk = socket (rp->ai_family, rp->ai_socktype,
                                                   rp->ai_protocol)) < 0) {
                                    perror("data: socket");
                                    /**
                                     * don't continue if we could not establish
                                     * a socket connection
                                     */
                                    rp = NULL;
                                    break;
                                }

                                if (connect (dsk, rp->ai_addr, rp->ai_addrlen) < 0) {
                                    close (dsk);
                                    perror ("data: connect");
                                    continue;
                                }

                                break;
                            }

                            if (dsk > fdmax) {
                                fdmax = dsk;
                            }

                            if (rp == NULL) {
                                fprintf (stderr,
                                         "data: could not connect to %s:%s\n",
                                         (char *) srv[0], (char *) srv[1]);
                                /* TODO what to do if connection fails */
                                break;
                            }

                            fprintf (stdout, "connected to %s:%s with socket %d\n",
                                     (char *) srv[0], (char *) srv[1], dsk);

#if 0
                            if (setnonblock (dsk) < 0) {
                                perror ("could not make data socket nonblocking");
                                continue;
                            }
#endif

                            /* add data socket to socket set */
                            //FD_SET (dsk, &sockset);

                /**
                 * TODO THIS SHOULD GO INTO recv on remote data socket
                 */

                            /* recv on remote data socket */
                            if ((numbytes = recv(dsk, buf, sizeof (buf), 0)) <= 0) {
                                /* connection closed or socket error */
                                if (numbytes == 0) {
                                    fprintf (stdout, "socket %d closed\n", dsk);
                                } else {
                                    perror ("data recv error");
                                }

                                /* close socket and remove it from socket set */
                                close (dsk);
                                FD_CLR (sk, &datasocks);
                            }

                            fprintf (stdout, "%s", (char *) buf);

                            cache_write (addr_port, buf);

                            /* relay back to client */
                            size_t buflen = strlen ((char *) buf);
                            if (sendall (sk, (char *) buf, &buflen) < 0) {
                                perror ("sendall");
                                fprintf (stderr, "only sent %d bytes.\n", (int) buflen);
                            }
                /**
                 * END
                 */

                            freeaddrinfo (remoteinfo);
                        }
                    }
                }
            } else if (FD_ISSET (sk, &datasocks)) {
                /* TODO */
                ;
#if 0
                /* recv on remote data socket */
                uint8_t buf[BUFSIZE] = { 0 };
                if ((numbytes = recv(sk, buf, sizeof (buf), 0)) <= 0) {
                    /* connection closed or socket error */
                    if (numbytes == 0) {
                        fprintf (stdout, "socket %d closed\n", sk);
                    } else {
                        perror ("data recv error");
                    }

                    /* close socket and remove it from socket set */
                    close (sk);
                    FD_CLR (sk, &datasocks);
                }

                fprintf (stdout, "%s", (char *) buf);
#endif
            }
        }
    }

    return 0;
}
