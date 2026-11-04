#include "core/log.h"
#include "core/poller.h"

#define NOT_IMPL { LOG_ERRNO("Not implemented"); errno = ENOSYS; return -1; }

poller_t *poller_create(void) {
  LOG_E("%s: not implemented", __func__);
  errno = ENOSYS;
  return NULL;
}

void poller_destroy(poller_t *p) {
  (void)p;
}

int poller_add(poller_t *p, int fd, void *udata, int events) { NOT_IMPL }
int poller_mod(poller_t *p, int fd, void *udata, int events) { NOT_IMPL }
int poller_del(poller_t *p, int fd) { NOT_IMPL }
int poller_wait(poller_t *p, poll_event_t *events, int max_events, int timeout_ms) { NOT_IMPL }
