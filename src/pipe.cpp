#include "miniposix/pipe.hpp"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace miniposix {

Pipe Pipe::create() {
    int fds[2] = {-1, -1};
    if(::pipe2(fds, O_CLOEXEC) == -1) {
        // Capture errno before constructing anything else.
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "pipe2");
    }

    // pipe2 allocated both descriptors atomically, and UniqueFd's constructor
    // is noexcept, so ownership is transferred without a failure window.
    Pipe result;
    result.read_end_ = UniqueFd{fds[0]};
    result.write_end_ = UniqueFd{fds[1]};
    return result;
}

} // namespace miniposix
