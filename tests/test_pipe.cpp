#include "miniposix/pipe.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/resource.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

// The iterator itself holds one descriptor, so both measurements include it.
int open_fd_count() {
    std::error_code error;
    int count = 0;
    for(const auto &entry : std::filesystem::directory_iterator("/proc/self/fd", error)) {
        (void)entry;
        ++count;
    }
    return error ? -1 : count;
}

} // namespace

TEST(Pipe, CannotBeCopiedAndCanBeMoved) {
    static_assert(!std::is_copy_constructible_v<miniposix::Pipe>);
    static_assert(!std::is_copy_assignable_v<miniposix::Pipe>);
    static_assert(std::is_nothrow_move_constructible_v<miniposix::Pipe>);
    static_assert(std::is_nothrow_move_assignable_v<miniposix::Pipe>);
}

TEST(Pipe, TransfersBytesThroughRealDescriptors) {
    auto pipe = miniposix::Pipe::create();
    const int reader = pipe.read_end().get();
    const int writer = pipe.write_end().get();
    ASSERT_GE(reader, 0);
    ASSERT_GE(writer, 0);
    ASSERT_NE(reader, writer);
    ASSERT_TRUE(is_open(reader));
    ASSERT_TRUE(is_open(writer));
    ASSERT_NE(::fcntl(reader, F_SETFL, ::fcntl(reader, F_GETFL) | O_NONBLOCK), -1);

    constexpr char sent = 'P';
    struct sigaction ignored {};
    ignored.sa_handler = SIG_IGN;
    struct sigaction previous {};
    ASSERT_EQ(::sigaction(SIGPIPE, &ignored, &previous), 0);
    const auto written = ::write(writer, &sent, 1);
    ASSERT_EQ(::sigaction(SIGPIPE, &previous, nullptr), 0);
    ASSERT_EQ(written, 1);
    char received = '\0';
    ASSERT_EQ(::read(reader, &received, 1), 1);
    EXPECT_EQ(received, sent);
}

TEST(Pipe, OwnsAndClosesBothEnds) {
    int reader = -1;
    int writer = -1;
    {
        auto pipe = miniposix::Pipe::create();
        reader = pipe.read_end().get();
        writer = pipe.write_end().get();
        ASSERT_GE(reader, 0);
        ASSERT_GE(writer, 0);
        EXPECT_TRUE(is_open(reader));
        EXPECT_TRUE(is_open(writer));
    }

    EXPECT_FALSE(is_open(reader));
    EXPECT_FALSE(is_open(writer));
    if(is_open(reader)) ::close(reader);
    if(is_open(writer)) ::close(writer);
}

TEST(Pipe, ClosingWriterProducesEndOfFile) {
    auto pipe = miniposix::Pipe::create();
    ASSERT_GE(pipe.read_end().get(), 0);
    ASSERT_GE(pipe.write_end().get(), 0);

    pipe.write_end().reset();
    const int reader = pipe.read_end().get();
    ASSERT_NE(::fcntl(reader, F_SETFL, ::fcntl(reader, F_GETFL) | O_NONBLOCK), -1);
    char byte = '\0';
    EXPECT_EQ(::read(reader, &byte, 1), 0);
}

TEST(Pipe, MovingTransfersBothEnds) {
    auto source = miniposix::Pipe::create();
    const int reader = source.read_end().get();
    const int writer = source.write_end().get();
    ASSERT_GE(reader, 0);
    ASSERT_GE(writer, 0);

    auto target = std::move(source);
    EXPECT_EQ(source.read_end().get(), -1);
    EXPECT_EQ(source.write_end().get(), -1);
    EXPECT_EQ(target.read_end().get(), reader);
    EXPECT_EQ(target.write_end().get(), writer);
}

TEST(Pipe, ReportsCreationFailureWithOperatingSystemError) {
    struct rlimit original {};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
    struct rlimit limited = original;
    limited.rlim_cur = 0;
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &limited), 0);

    bool threw_system_error = false;
    int error_code = 0;
    try {
        auto pipe = miniposix::Pipe::create();
        (void)pipe;
    } catch(const std::system_error& error) {
        threw_system_error = true;
        error_code = error.code().value();
    } catch(...) {
        // Restore the limit before reporting the unexpected exception.
    }

    const int restore_result = ::setrlimit(RLIMIT_NOFILE, &original);
    ASSERT_EQ(restore_result, 0);
    EXPECT_TRUE(threw_system_error);
    if(threw_system_error) EXPECT_EQ(error_code, EMFILE);
}

TEST(Pipe, MoveAssignmentClosesDestinationDescriptors) {
    auto source = miniposix::Pipe::create();
    auto target = miniposix::Pipe::create();

    const int source_reader = source.read_end().get();
    const int source_writer = source.write_end().get();
    const int old_reader = target.read_end().get();
    const int old_writer = target.write_end().get();
    ASSERT_GE(source_reader, 0);
    ASSERT_GE(source_writer, 0);
    ASSERT_GE(old_reader, 0);
    ASSERT_GE(old_writer, 0);
    ASSERT_TRUE(is_open(old_reader));
    ASSERT_TRUE(is_open(old_writer));

    target = std::move(source);

    // The destination's previous descriptors must be closed, not overwritten.
    EXPECT_FALSE(is_open(old_reader));
    EXPECT_FALSE(is_open(old_writer));
    EXPECT_EQ(source.read_end().get(), -1);
    EXPECT_EQ(source.write_end().get(), -1);
    EXPECT_EQ(target.read_end().get(), source_reader);
    EXPECT_EQ(target.write_end().get(), source_writer);
    EXPECT_TRUE(is_open(source_reader));
    EXPECT_TRUE(is_open(source_writer));

    // The moved-to pipe must still work, not just hold the right integers.
    char sent = 'M';
    ASSERT_EQ(::write(target.write_end().get(), &sent, 1), 1);
    char received = '\0';
    ASSERT_EQ(::read(target.read_end().get(), &received, 1), 1);
    EXPECT_EQ(received, sent);
}

TEST(Pipe, SelfMoveAssignmentKeepsOwnedDescriptors) {
    auto pipe = miniposix::Pipe::create();
    const int reader = pipe.read_end().get();
    const int writer = pipe.write_end().get();
    ASSERT_GE(reader, 0);
    ASSERT_GE(writer, 0);

    auto &same = pipe;
    pipe = std::move(same);

    EXPECT_EQ(pipe.read_end().get(), reader);
    EXPECT_EQ(pipe.write_end().get(), writer);
    EXPECT_TRUE(is_open(reader));
    EXPECT_TRUE(is_open(writer));
}

TEST(Pipe, RepeatedCreateAndDestroyDoesNotLeakDescriptors) {
    const int before = open_fd_count();
    ASSERT_GE(before, 0);

    for(int iteration = 0; iteration < 1000; ++iteration) {
        auto pipe = miniposix::Pipe::create();
        ASSERT_GE(pipe.read_end().get(), 0);
        ASSERT_GE(pipe.write_end().get(), 0);
    }

    const int after = open_fd_count();
    ASSERT_GE(after, 0);
    EXPECT_EQ(after, before);
}

TEST(Pipe, CreationFailureDoesNotLeakDescriptors) {
    struct rlimit original {};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
    const int before = open_fd_count();
    ASSERT_GE(before, 0);

    struct rlimit limited = original;
    limited.rlim_cur = 0;
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &limited), 0);

    bool threw_system_error = false;
    int error_code = 0;
    try {
        auto pipe = miniposix::Pipe::create();
        (void)pipe;
    } catch(const std::system_error &error) {
        threw_system_error = true;
        error_code = error.code().value();
    } catch(...) {
        // Restore the limit below before reporting the unexpected exception.
    }

    // Measure only after restoring the limit, so /proc/self/fd can be opened.
    const int restore_result = ::setrlimit(RLIMIT_NOFILE, &original);
    const int after = open_fd_count();
    EXPECT_EQ(restore_result, 0);
    ASSERT_GE(after, 0);
    EXPECT_TRUE(threw_system_error);
    if(threw_system_error) EXPECT_EQ(error_code, EMFILE);
    EXPECT_EQ(after, before);
}
