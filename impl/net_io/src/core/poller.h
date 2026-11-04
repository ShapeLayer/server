/* poller.h */

/* Polling interface for network I/O */

#pragma once
#ifndef POLLER_H
#define POLLER_H

typedef struct poller poller_t;

enum {
  POLLER_IN = 1 << 0,   // interested in read events
                        // data to read from the socket is available
                        // can be accepted
  POLLER_OUT = 1 << 1,  // interested in write events
                        // sending buffer has space for more data
  POLLER_ERR = 1 << 2,  // interested in error events
                        // sockets throw the error
  POLLER_HUP = 1 << 3,  // interested in hang-up events
                        // sockets are closed by the peer
};

typedef struct { void *udata; int events; } poll_event_t;

poller_t *poller_create(void);
void poller_destroy(poller_t *p);
int poller_add(poller_t *p, int fd, void *udata, int events);
int poller_mod(poller_t *p, int fd, void *udata, int events);
int poller_del(poller_t *p, int fd);
int poller_wait(poller_t *p, poll_event_t *events, int max_events, int timeout_ms);

#endif  // POLLER_H
