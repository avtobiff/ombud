/**
 * This is Ombud, a command driven caching proxy.
 */

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "netutil.h"
#include "cache.h"


#define DEFAULT_PORT        "8090"
#define BUFLEN              8192

#define CACHE_BASEDIR       "cache-ombud" /* TODO make this configurable */
#define ADDR_PORT_STRLEN    22

/* constants we use with epoll */
#define MAXEVENTS           64
#define READ_CMD            1
#define READ_REMOTE         2       /* read remote host data */
#define RELAY_BACK          4       /* send remote host data to client */


struct command {
    uint8_t     cmd;        /* command, READ_REMOTE or RELAY_BACK */
    int         cfd;        /* client socket */
    int         rfd;        /* remote host socket */
    uint8_t     *remote;    /* client command: "ADDRESS:PORT\r\n" */
    uint8_t     *buf;       /* what was read from remote host */
};


/**
 * Convenience wrapper for adding epoll events.
 */
static void
epoll_add (int epollfd, struct command *command) {
    struct epoll_event      event;
    int                     fd;

    if (command->cmd == READ_REMOTE) {
        fd = command->rfd;  /* remote host socket */
    } else {
        fd = command->cfd;  /* client socket */
    }

    event.data.ptr = command;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl (epollfd, EPOLL_CTL_ADD, fd, &event) < 0) {
        err (1, "Could not add command to epoll");
    }
}


/**
 * Process all incoming connections.
 */
static void
do_accept (const int listensock, const int epollfd)
{
    for (;;) {
        int                         client_socket;
        struct sockaddr_storage     peer_addr;
        socklen_t                   peer_addr_len;
        struct command              *command;


        peer_addr_len = sizeof (peer_addr);
        /* TODO use accept4() instead of accept and mk_nonblock, saves calls to
         * fcntl and userspace flag bit twiddling. */
        client_socket = accept (listensock, (struct sockaddr *) &peer_addr,
                                &peer_addr_len);

        if (client_socket < 0) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                /* processed all incoming connections */
                break;
            } else {
                perror ("do_accept");
                break;
            }
        }

        if (mk_nonblock (client_socket) < 0) {
            err (1, "Could not make client socket non-blocking");
        }

        /* create read client command */
        command = calloc (1, sizeof (struct command));
        command->cmd = READ_CMD;
        command->cfd = client_socket;

        /* add command to epoll event queue */
        epoll_add (epollfd, command);
    }
}


/**
 * Process read (client) command.
 */
static void
do_read_cmd (const int cfd)
{
    uint8_t buf[BUFLEN] = { 0 };
    ssize_t readbytes;

    /* read command from client */
    if ((readbytes = read (cfd, buf, BUFLEN)) < 0) {
        if (readbytes == 0) {
            /* EOF, client closed socket */
            ;
        } else {
            perror ("ctrlsock read error");
        }
        close (cfd); /* also removes from epoll */
    }
    /* send from cache or defer relay */
    else {
        uint8_t service[NI_MAXHOST] = { 0 };
        uint8_t *cr, *nl;

        /* strip \r and \n from command, creating key used in the cache */
        strncat ((char *) service, (char *) buf, readbytes);
        if ((cr = (uint8_t *) strrchr ((char *) service, '\r')) != NULL) {
            *cr = '\0';
        }
        if ((nl = (uint8_t *) strrchr ((char *) service, '\n')) != NULL) {
            *nl = '\0';
        }

        /* try sending from cache, upon miss defer remote host read */
        if (!cache_sendfile (cfd, service))
        {
            struct command *newcmd =
                    calloc (1, sizeof (struct command));
            fprintf (stdout, "defer remote host read\n");
            /* add command to read remote host data to event queue */
            newcmd->cmd = READ_REMOTE;
            newcmd->cfd = cfd;
            //newcmd->rfd = command->rfd;
            newcmd->remote = buf;

            /* add command to event queue */
            //epoll_add (epollfd, newcmd);
        }
    }
}


/**
 * Ombud main entry point.
 */
int
main (int argc, char *argv[])
{
    int                         listensock,
                                epollfd;

    struct epoll_event          event,
                                *events;

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
        err (1, "Could not create cache dir");
    }
    fprintf (stdout, "Initialized cache...\n");

    /* initialize epoll */
    if ((epollfd = epoll_create1 (0)) < 0) {
        err (1, "Could not initialize epoll");
    }

    /* add epoll event for handling listen socket */
    struct command *lcmd = calloc (1, sizeof (struct command));
    lcmd->cfd = listensock;
    epoll_add (epollfd, lcmd);

    /* event buffer */
    events = calloc (MAXEVENTS, sizeof (event));

    fprintf (stdout, "Entering main loop...\n");
    for (;;) {
        /* block until we get some events to process */
        int numevents = epoll_wait (epollfd, events, MAXEVENTS, -1);
        struct command *command;

        /* process all events */
        for (int i = 0; i < numevents; i++) {
            /* get command */
            command = events[i].data.ptr;
            fprintf (stdout, "command->cmd = %d\n", command->cmd);

            /* epoll error */
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN)))
            {
                /* notified but nothing ready for processing */
                warn ("epoll error\n");
                close (command->cfd);
                close (command->rfd);
                continue;
            }
            /* ACCEPT */
            else if (command->cfd == listensock) {
                do_accept (listensock, epollfd);
                /* processed all incoming events on listensock, continue to
                 * next event. */
                continue;
            }
            /* HANDLE COMMANDS */
            else {
                switch (command->cmd) {
                    case READ_CMD:
                        do_read_cmd (command->cfd);
                        break;

                    case READ_REMOTE:
                        fprintf (stdout, "read remote\n");
                        break;

                    case RELAY_BACK:
                        fprintf (stdout, "relay back\n");
                        break;

                    default:
                        break;
                }
            }
        }
#if 0
        /* copy socket set for use with select */
        ctrlsocks = sockset;
        datasocks = sockset;

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
                    uint8_t buf[BUFLEN] = { 0 };
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

                            uint8_t buf[BUFLEN] = { 0 };
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
                            if (sendall (sk, buf, &buflen) < 0) {
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
                uint8_t buf[BUFLEN] = { 0 };
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
#endif
    }


    free (events);
    close (listensock);

    return EXIT_SUCCESS;
}
