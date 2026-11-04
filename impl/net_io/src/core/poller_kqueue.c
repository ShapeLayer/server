#include "core/poller.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

struct poller {
  int kq;   // kqueue file descriptor
  struct kevent *buf;  // buffer for kevent events
  int capacity;  // capacity of the buffer
};

poller_t *poller_create(void) {
  poller_t *p = calloc(1, sizeof *p);
  if (!p) return NULL;

  p->kq = kqueue();
  if (p->kq < 0) {
    free(p);
    return NULL;
  }

  // Set the close-on-exec flag for the kqueue file descriptor
  fcntl(p->kq, F_SETFD, FD_CLOEXEC);
  return p;
}

void poller_destroy(poller_t *p) {
  if (!p) return;
  close(p->kq);
  free(p->buf);
  free(p);
}

/**
 * Apply a change to the poller
 * @param p The poller
 * @param fd The file descriptor
 * @param udata User data
 * @param events The events to watch for
 * @return 0 on success, -1 on error
 */
static int apply(poller_t *p, int fd, void *udata, int events) {
  // ch[0]: read event, ch[1]: write event
  struct kevent ch[2];

  EV_SET(           // Set the event configuration
    &ch[0],         // into channel 0 (read buffer)
    (uintptr_t)fd,  // to monitoring the specific file descriptor
    EVFILT_READ,    // for read events
    EV_ADD | ((events & POLLER_IN) ? EV_ENABLE : EV_DISABLE),
                    // add fd when the fd has not been added to queue and enable immediately
                    // enable or disable the event based on the events parameter
    0,              // filter flags: none
    0,              // filter data: none
    udata
  );

  EV_SET(           // Set the event configuration
    &ch[1],         // into channel 1 (write buffer)
    (uintptr_t)fd,  // to monitoring the specific file descriptor
    EVFILT_WRITE,   // for write events
    EV_ADD | ((events & POLLER_OUT) ? EV_ENABLE : EV_DISABLE),
                    // add fd when the fd has not been added to queue and enable immediately
                    // enable or disable the event based on the events parameter
    0,              // filter flags: none
    0,              // filter data: none
    udata
  );

  return kevent(p->kq, ch, 2, NULL, 0, NULL) < 0 ? -1 : 0;
}

// Add a file descriptor to the poller
int poller_add(poller_t *p, int fd, void *udata, int events) {
  return apply(p, fd, udata, events);
}
// Modify the events for a file descriptor in the poller
int poller_mod(poller_t *p, int fd, void *udata, int events) {
  return apply(p, fd, udata, events);
}

int poller_del(poller_t *p, int fd) {
  struct kevent ch[2];
  EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  return kevent(p->kq, ch, 2, NULL, 0, NULL) < 0 ? -1 : 0;
}

int poller_wait(poller_t *p, poll_event_t *events, int max_events, int timeout_ms) {
  if (max_events <= 0) {
    errno = EINVAL;
    return -1;
  }

  if (max_events > p->capacity) {
    struct kevent *new_buf = realloc(p->buf, max_events * sizeof(struct kevent));
    if (!new_buf) return -1;
    p->buf = new_buf;
    p->capacity = max_events;
  }

  struct timespec timeout_ts;
  struct timespec *timeout_ptr = NULL;

  if (timeout_ms >= 0) {
    timeout_ts.tv_sec = timeout_ms / 1000;
    timeout_ts.tv_nsec = (timeout_ms % 1000) * 1000000;
    timeout_ptr = &timeout_ts;
  }

  int nevents = kevent(p->kq, NULL, 0, p->buf, max_events, timeout_ptr);
  if (nevents < 0) return errno == EINTR ? 0 : -1;

    for (int i = 0; i < nevents; i++) {
    const struct kevent *e = &p->buf[i];
    int ev = 0;
    if (e->flags & EV_ERROR)
      ev |= POLLER_ERR;
    else if (e->filter == EVFILT_READ)
      ev |= POLLER_IN;
    else if (e->filter == EVFILT_WRITE)
      ev |= POLLER_OUT;
    if (e->flags & EV_EOF) {
      ev |= POLLER_HUP;
      if (e->fflags != 0) /* EOF와 함께 소켓 에러 코드가 fflags로 옴 */
        ev |= POLLER_ERR;
    }
    events[i].udata  = e->udata;
    events[i].events = ev;
  }

  return nevents;
}
