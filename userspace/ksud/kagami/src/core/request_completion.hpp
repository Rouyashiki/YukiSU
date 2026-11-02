#pragma once

#include <poll.h>
#include <sys/socket.h>
#include <cerrno>

namespace kagami::daemon_protocol {

// The server and its mount workers retain the request socket until they exit
// the operation. A receive timeout is not a completion signal.
inline bool wait_for_request_completion(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return false;
    }
    char discard[4096];
    for (;;) {
        pollfd descriptor{fd, POLLIN, 0};
        if (poll(&descriptor, 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return false;
        }
        const auto count = recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
        if (count == 0 || (count < 0 && errno == ECONNRESET))
            return true;
        if (count > 0 || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return false;
    }
}

}  // namespace kagami::daemon_protocol
