/**
 * This is Ombud, a command driven caching proxy.
 */

#include <err.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define _GNU_SOURCE
#include <sys/socket.h>

#include "netutil.h"
#include "cache.h"


#define NUMCHILDS           sysconf (_SC_NPROCESSORS_ONLN)  /* cpu cores */

#define DEFAULT_PORT        "8090"
#define BUFLEN              8192

#define CACHE_BASEDIR       "cache-ombud" /* TODO make this configurable */

#define SERVMAXLEN          NI_MAXHOST + NI_MAXSERV + 1   /* "addr:port" */

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


/* book keeping of child processes */
static pid_t *child_pids;


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
#if 0
        client_socket = accept (listensock, (struct sockaddr *) &peer_addr,
                                &peer_addr_len);
#endif
        client_socket = accept4 (listensock, (struct sockaddr *) &peer_addr,
                                 &peer_addr_len, SOCK_NONBLOCK);

        if (client_socket < 0) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                /* processed all incoming connections */
                break;
            } else {
                perror ("do_accept");
                break;
            }
        }

#if 0
        if (mk_nonblock (client_socket) < 0) {
            err (1, "Could not make client socket non-blocking");
        }
#endif

        /* create read client command */
        command = calloc (1, sizeof (struct command));
        command->cmd = READ_CMD;
        command->cfd = client_socket;

        /* add command to epoll event queue */
        epoll_add (epollfd, command);
    }
}


/**
 * Extract remote host and port from remote service string.
 */
static int
extract_host_port (const uint8_t *remote_srv, const ssize_t len,
                     uint8_t *remote_host, uint8_t *remote_port)
{
    uint8_t *s, *h, *p;

    s = h = (uint8_t *) strndup ((char *) remote_srv, len);
    s += len;
    /* search for ':' from the back of supplied string, stop when we searched
     * through everything. */
    for (; (*(--s) != ':') && (s != h) ;);

    if (s == remote_host) {
        warn ("Invalid argument %s", remote_host);
        return -1;
    }

    /* split supplied string into host and port */
    *s = '\0';
    p = s + 1;

    /* "return values", remote host and port */
    strncat ((char *) remote_host, (char *) h, strlen ((char *) h));
    strncat ((char *) remote_port, (char *) p, strlen ((char *) p));

    return 1;
}


/**
 * Connect to remote host, return socket.
 */
static int
connect_remote_host (const uint8_t *remote_srv, const ssize_t len)
{
    uint8_t             *remote_host = calloc (1, NI_MAXHOST),
                        *remote_port = calloc (1, NI_MAXSERV);

    struct addrinfo     hints,
                        *remoteinfo,
                        *rp;

    int                 rsock;


    /* extract remote host and port as strings */
    if (extract_host_port (remote_srv, len, remote_host, remote_port) < 0) {
        return -1;
    }

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
        return -1;
    }

    if (mk_nonblock (rsock) < 0) {
        err (1, "could not make data socket nonblocking");
    }

    return rsock;
}


/**
 * Extract commands from client buffer.
 *
 * The function dynamically allocates a memory region for the parsed result.
 */
static uint8_t**
extract_cmds (const uint8_t *buf)
{
    uint8_t     num = 0;
    uint8_t     **cmds = calloc (1, SERVMAXLEN);


    /* split on newlines, allow commands with only \n as well as \r\n */
    uint8_t     *tok = (uint8_t *) strtok ((char *) buf, "\n");

    /* extract each command from user input */
    for (;;) {
        /* exhausted input buffer */
        if (!tok) {
            cmds[num++] = NULL;
            break;
        }

        /* remove possible carriage return */
        uint8_t *cr;
        if ((cr = (uint8_t *) strrchr ((char *) tok, '\r')) != NULL) {
            *cr = '\0';
        }

        /* duplicate and save command if it exists */
        cmds[num++] = tok ? (uint8_t *) strdup ((char *) tok) : tok;

        /* alloc place for another command */
        cmds = realloc (cmds, (num + 2) * SERVMAXLEN);
        tok = (uint8_t *) strtok (NULL, "\n");
    }

    return cmds;
}


/**
 * Process read (client) command.
 */
static void
do_read_cmd (const int epollfd, struct command * command)
{
    uint8_t buf[BUFLEN] = { 0 };
    ssize_t readbytes;

    /* read command(s) from client */
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
        uint8_t **services = extract_cmds (buf);

        for (; services && *services; ++services) {
            uint8_t *service = *services;

            /* try sending from cache, upon miss defer remote host read */
            if (!cache_sendfile (command->cfd, service))
            {
                int rsock;
                if ((rsock = connect_remote_host (service, readbytes)) < 0) {
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
        }

        /* processed all commands, back to READ_CMD */
        struct command *readcmd = calloc (1, sizeof (struct command));

        readcmd->cmd = READ_CMD;
        readcmd->cfd = command->cfd;

        epoll_mod (epollfd, readcmd);
    }
}


/**
 * Read from remote host.
 */
static void
do_read_remote (struct command *command, uint8_t *buf, ssize_t *buflen)
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

        /* close socket to remote host */
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
 * Main server event loop.
 */
static int
child (const int8_t index, const uint8_t *server_port)
{
    int                         listensock,
                                epollfd;

    struct epoll_event          event,
                                *events;


    /* setup listen socket */
    if ((listensock = setup_listener (server_port)) < 0) {
        err (1, "Could not setup listen socket");
    }

    fprintf (stdout, "proc %d: Listening on port %s...\n",
             index, (char *) server_port);

    /* initialize cache */
    if (cache_init ((const uint8_t *) CACHE_BASEDIR) < 0) {
        err (1, "Could not create cache dir");
    }
    fprintf (stdout, "proc %d: Initialized cache...\n", index);

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

    fprintf (stdout, "proc %d: Entering main loop...\n", index);
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
                        ssize_t buflen;
                        /* command is free'd in d_r_r() */
                        int cfd = command->cfd;

                        do_read_remote (command, buf, &buflen);
                        /* verify that we actually have data to relay back */
                        if (buflen > 0) {
                            if (!sendall (cfd, buf, (size_t *) &buflen)) {
                                warn ("Could not relay back data to client");
                            }
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


/**
 * Signal handler, exits on SIGINT.
 */
static void
sighandler (int signal)
{
    if (signal == SIGINT) {
        for (int i = 0; i < NUMCHILDS; i++) {
            if (child_pids[i] != 0) {
                kill (child_pids[i], SIGKILL);
            }
        }
    }
}


/**
 * Ombud main entry point.
 */
int
main (int argc, char *argv[])
{
    int             status,
                    numchilds;

    uint8_t         *server_port;


    signal (SIGINT, sighandler);

    /* get (valid) port from command line or use default port */
    if ((argc >= 2) && (atoi (argv[1]) < 65536)) {
        size_t portlen = strlen (argv[1]);
        server_port = calloc (1, portlen);
        strncat ((char *) server_port, argv[1], portlen);
    } else {
        server_port = calloc (1, 5);
        strcat ((char *) server_port, DEFAULT_PORT);
    }

    /* get user defined number of concurrent processes */
    if ((argc >= 3) && (atoi (argv[2]) < sysconf (_SC_CHILD_MAX))) {
        numchilds = atoi(argv[2]);
    } else {
        numchilds = NUMCHILDS;
    }

    child_pids = calloc (numchilds, sizeof (pid_t));

    for (int8_t i = 0; i < numchilds; i++) {
        pid_t pid = fork ();

        if (pid == 0) {
            child (i, server_port);
            return EXIT_SUCCESS;
        }
        else if (pid < 0) {
            err (1, "fork");
        } else {
            /* parent process saves child pids */
            child_pids[i] = pid;
        }
    }

    wait (&status);

    free (child_pids);

    return EXIT_SUCCESS;
}
