/* net.h */

/* Network Utilities */

#pragma once
#ifndef NET_H
#define NET_H

#include <stdint.h>

/**
 * Create a TCP listening socket on the specified IP and port.
 */
int net_listen_tcp(const char *ip, uint16_t port, int backlog);

/**
 * Create a TCP connection to the specified IP and port.
 */
int net_connect_tcp(const char *ip, uint16_t port);

/**
 * Set a socket to non-blocking mode.
 */
int net_set_nonblock(int fd);

/**
 * Set the TCP_NODELAY option on a socket.
 */
int net_set_nodelay(int fd, int on);

/**
 * Set the SO_NOSIGPIPE option on a socket.
 */
int net_set_nosigpipe(int fd);

/**
 * Tune the session options for a socket.
 */
int net_tune_session(int fd);

/**
 * Close a socket and abort any pending operations.
 */
void net_abort_close(int fd);


#endif  // NET_H
