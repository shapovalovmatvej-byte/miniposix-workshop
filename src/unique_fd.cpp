#include "miniposix/unique_fd.hpp"

#include <unistd.h>

namespace miniposix {

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() noexcept {
    if(fd_ != -1) {
        // Linux releases the descriptor even if close is interrupted, so do
        // not retry on EINTR (the number could already be reused).
        (void)::close(fd_);
    }
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    // release() clears other.fd_, so without this guard a self move would
    // close the descriptor that *this is supposed to keep.
    if(this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::get() const noexcept {
    return fd_;
}

int UniqueFd::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void UniqueFd::reset(int fd) noexcept {
    if(fd == fd_) {
        return;
    }
    if(fd_ != -1) {
        (void)::close(fd_);
    }
    fd_ = fd;
}

} // namespace miniposix
