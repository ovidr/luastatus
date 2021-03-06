#include "wakeup_fifo.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include "osdep.h"
#include "time_utils.h"

int
ls_wakeup_fifo_init(LSWakeupFifo *w, const char *fifo, struct timespec timeout, sigset_t *sigmask)
{
    w->fifo = fifo;
    w->timeout = timeout;
    FD_ZERO(&w->fds_);
    w->fd_ = -1;
    if (sigmask) {
        w->sigmask = *sigmask;
    } else {
        if (sigfillset(&w->sigmask) < 0) {
            return -1;
        }
    }
    return 0;
}

int
ls_wakeup_fifo_open(LSWakeupFifo *w)
{
    if (w->fd_ < 0 && w->fifo) {
        w->fd_ = open(w->fifo, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (w->fd_ < 0) {
            return -1;
        }
        if (w->fd_ >= FD_SETSIZE) {
            ls_close(w->fd_);
            w->fd_ = -1;
            errno = EMFILE;
            return -1;
        }
        return 0;
    }
    return 0;
}

int
ls_wakeup_fifo_wait(LSWakeupFifo *w)
{
    if (w->fd_ >= 0) {
        FD_SET(w->fd_, &w->fds_);
    }
    const int r = pselect(
        w->fd_ >= 0 ? w->fd_ + 1 : 0,
        &w->fds_, NULL, NULL,
        ls_timespec_is_invalid(w->timeout) ? NULL : &w->timeout,
        &w->sigmask);
    if (r < 0) {
        int saved_errno = errno;
        if (w->fd_ >= 0) {
            FD_CLR(w->fd_, &w->fds_);
        }
        ls_close(w->fd_);
        w->fd_ = -1;
        errno = saved_errno;
        return -1;
    } else if (r == 0) {
        return 0;
    } else {
        FD_CLR(w->fd_, &w->fds_);
        ls_close(w->fd_);
        w->fd_ = -1;
        return 1;
    }
}

void
ls_wakeup_fifo_destroy(LSWakeupFifo *w)
{
    ls_close(w->fd_);
}
