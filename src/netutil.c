/**
 * Network utility functions, defined here to save characters and lines in the
 * main server program.
 */

#include "netutil.h"


/**
 * Make supplied socket non-blocking.
 */
int
mk_nonblock (const int socket)
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
int
setup_listener (const uint8_t *server_port)
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

        /* enable several processes listening on the same port */
        int true_ = true;
        if (setsockopt (listensock, SOL_SOCKET, SO_REUSEPORT, &true_,
                        sizeof (int)) < 0) {
            err (1, "could not set SO_REUSEPORT on listen socket");
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
 * Send all data in buf to socket, number of bytes written is stored in buflen.
 */
int
sendall (const int socket, const uint8_t *buf, size_t *buflen)
{
    ssize_t     sentbytes = 0,
                bytesleft = *buflen,
                numbytes = 0;

    while (numbytes < (ssize_t) *buflen) {
        numbytes = send (socket, (char *) buf + sentbytes, bytesleft, 0);
        if (numbytes < 0) { break; }
        sentbytes += numbytes;
        bytesleft -= numbytes;
    }

    /* set buflen, i.e. "return value" */
    *buflen = sentbytes;

    return numbytes == -1 ? -1 : 0;
}
