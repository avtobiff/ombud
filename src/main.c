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
    uint8_t     *service;   /* client command: "ADDRESS:PORT\r\n" */
};


/**
 * Convenience wrapper for adding and modifying epoll events.
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
 * Used for toggling client socket connection between READ_CMD and READ_REMOTE.
 */
static void
epoll_mod (int epollfd, struct command *command) {
    struct epoll_event      event;

    event.data.ptr = command;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl (epollfd, EPOLL_CTL_MOD, command->cfd, &event) < 0) {
        err (1, "Could not modify command to epoll");
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


static int
__connect_remote_host (const uint8_t *remote_srv, const ssize_t len)
{
    uint8_t             *remote_host,
                        *remote_port,
                        *s;

    struct addrinfo     hints,
                        *remoteinfo,
                        *rp;

    int                 rsock;


    /* extract remote host and port as strings */
    s = remote_host = (uint8_t *) strndup ((char *) remote_srv, len);
    s += len;
    /* search for ':' from the back of supplied string, stop when we searched
     * through everything. */
    for (; (*(--s) != ':') && (s != remote_host) ;);

    if (s == remote_host) {
        warn ("Invalid argument %s", remote_host);
        return -1;
    }

    /* split supplied string into host and port */
    *s = '\0';
    remote_port = s + 1;

    //uint8_t buf[BUFLEN] = { 0 };

    bzero (&hints, sizeof (struct addrinfo));
    hints.ai_family   = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;    /* TCP */

    int r;
    if ((r = getaddrinfo ((char *) remote_host, (char *) remote_port,
                          &hints, &remoteinfo)) != 0) {
        warn ("getaddrinfo: %s", gai_strerror (r));
        return -1;
    }

    for (rp = remoteinfo; rp != NULL; rp++) {
        if ((rsock = socket (rp->ai_family, rp->ai_socktype,
                             rp->ai_protocol)) < 0) {
            /* don't continue if we could not establish a socket connection */
            rp = NULL;
            break;
        }

        if (connect (rsock, rp->ai_addr, rp->ai_addrlen) < 0) {
            close (rsock);
            warn ("data: connect");
            continue;
        }

        break;
    }

    if (rp == NULL) {
        /* could not connect, silently drop this. */
        /* TODO remove from epoll */
        return -1;
    }

    if (mk_nonblock (rsock) < 0) {
        err (1, "could not make data socket nonblocking");
    }

    return rsock;
}


/**
 * Process read (client) command.
 */
static void
do_read_cmd (const int epollfd, struct command * command)
{
    uint8_t buf[BUFLEN] = { 0 };
    ssize_t readbytes;

    /* read command from client */
    if ((readbytes = read (command->cfd, buf, BUFLEN)) <= 0) {
        if (readbytes == 0) {
            /* EOF, client closed socket */
            ;
        } else {
            perror ("ctrlsock read error");
        }
        close (command->cfd); /* also removes from epoll */
    }
    /* send from cache or defer relay */
    else {
        uint8_t *service = calloc (1, NI_MAXHOST);
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
        if (!cache_sendfile (command->cfd, service))
        {
            int rsock;
            if ((rsock = __connect_remote_host (service, readbytes)) < 0) {
                warn ("could not connect to host");
                return;
            }

            struct command *newcmd = calloc (1, sizeof (struct command));

            /* add command to read remote host data to event queue */
            newcmd->cmd = READ_REMOTE;
            newcmd->cfd = command->cfd;
            newcmd->rfd = rsock;
            newcmd->service = service;

            /* add command to event queue */
            epoll_add (epollfd, newcmd);
        }

        struct command *readcmd = calloc (1, sizeof (struct command));

        /* back to READ_CMD */
        readcmd->cmd = READ_CMD;
        readcmd->cfd = command->cfd;
        epoll_mod (epollfd, readcmd);
    }
}


/**
 * Read from remote host.
 */
static void
do_read_remote (struct command *command, uint8_t *buf, size_t *buflen)
{
    ssize_t readbytes;

    /* recv on remote data socket */
    if ((readbytes = read (command->rfd, buf, BUFLEN)) <= 0) {
        /* connection closed or socket error */
        if (readbytes == 0) {
            /* EOF, remote host closed socket */
        } else {
            perror ("data recv error");
        }

        /* close socket socket to remote host */
        close (command->rfd);
    }

    if (cache_write (command->service, buf, readbytes) < 0) {
        warn ("Could not write to cache");
    }

    free (command->service);
    free (command);

    /* "return value" read bytes */
    *buflen = readbytes;
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

            /* epoll error */
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN)))
            {
                /* notified but nothing ready for processing */
                warn ("epoll error");
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
                        do_read_cmd (epollfd, command);
                        break;

                    case READ_REMOTE:
                        ;   /* hack needed for variable defs inside switch */
                        uint8_t buf[BUFLEN] = { 0 };
                        size_t buflen;
                        /* command is free'd in d_r_r() */
                        int cfd = command->cfd;

                        do_read_remote (command, buf, &buflen);
                        if (sendall (cfd, buf, &buflen) < 0) {
                            warn ("Could not relay back data to client");
                        }
                        break;

                    default:
                        break;
                }
            }
        }
    }

    free (events);
    close (listensock);

    return EXIT_SUCCESS;
}
