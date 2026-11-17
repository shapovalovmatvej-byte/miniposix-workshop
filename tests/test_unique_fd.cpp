#include "miniposix/unique_fd.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <fcntl.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

class StdinRestorer {
public:
    StdinRestorer() : was_open_(is_open(STDIN_FILENO)), saved_(was_open_ ? ::dup(STDIN_FILENO) : -1) {}

    ~StdinRestorer() {
        if(saved_ >= 0) {
            ::dup2(saved_, STDIN_FILENO);
            ::close(saved_);
        } else if(!was_open_) {
            ::close(STDIN_FILENO);
        }
    }

    [[nodiscard]] bool can_restore() const { return !was_open_ || saved_ >= 0; }

private:
    bool was_open_;
    int saved_;
};

} // namespace

TEST(UniqueFd, StartsEmptyAndCannotBeCopied) {
    static_assert(!std::is_copy_constructible_v<miniposix::UniqueFd>);
    static_assert(!std::is_copy_assignable_v<miniposix::UniqueFd>);
    static_assert(std::is_nothrow_move_constructible_v<miniposix::UniqueFd>);
    static_assert(std::is_nothrow_move_assignable_v<miniposix::UniqueFd>);

    miniposix::UniqueFd fd;
    EXPECT_EQ(fd.get(), -1);
}

TEST(UniqueFd, DescriptorZeroCanBeReleasedAndClosed) {
    StdinRestorer restore;
    ASSERT_TRUE(restore.can_restore());
    int ends[2];
    ASSERT_EQ(::pipe(ends), 0);
    const int replaced = ::dup2(ends[0], STDIN_FILENO);
    if(ends[0] != STDIN_FILENO) ::close(ends[0]);
    ::close(ends[1]);
    ASSERT_EQ(replaced, STDIN_FILENO);

    {
        miniposix::UniqueFd owner{STDIN_FILENO};
        EXPECT_EQ(owner.get(), STDIN_FILENO);
        EXPECT_EQ(owner.release(), STDIN_FILENO);
        EXPECT_EQ(owner.get(), -1);
    }
    ASSERT_TRUE(is_open(STDIN_FILENO));

    {
        miniposix::UniqueFd owner{STDIN_FILENO};
        EXPECT_EQ(owner.get(), STDIN_FILENO);
    }
    EXPECT_FALSE(is_open(STDIN_FILENO));
}

TEST(UniqueFd, AdoptsAndClosesARealDescriptor) {
    int ends[2];
    ASSERT_EQ(::pipe(ends), 0);
    const int read_fd = ends[0];
    ::close(ends[1]);

    {
        miniposix::UniqueFd owner{read_fd};
        EXPECT_EQ(owner.get(), read_fd);
        EXPECT_TRUE(is_open(read_fd));
    }

    EXPECT_FALSE(is_open(read_fd));
    if(is_open(read_fd)) ::close(read_fd);
}

TEST(UniqueFd, ReleaseReturnsOwnershipWithoutClosing) {
    int ends[2];
    ASSERT_EQ(::pipe(ends), 0);
    const int read_fd = ends[0];
    ::close(ends[1]);

    int released = -1;
    {
        miniposix::UniqueFd owner{read_fd};
        released = owner.release();
        EXPECT_EQ(released, read_fd);
        EXPECT_EQ(owner.get(), -1);
    }

    EXPECT_TRUE(is_open(read_fd));
    if(is_open(read_fd)) ::close(read_fd);
}

TEST(UniqueFd, ResetClosesOldDescriptorAndAdoptsNewOne) {
    int first[2];
    int second[2];
    ASSERT_EQ(::pipe(first), 0);
    ASSERT_EQ(::pipe(second), 0);
    const int old_fd = first[0];
    const int new_fd = second[0];
    ::close(first[1]);
    ::close(second[1]);

    {
        miniposix::UniqueFd owner{old_fd};
        owner.reset(new_fd);
        EXPECT_FALSE(is_open(old_fd));
        EXPECT_EQ(owner.get(), new_fd);
        EXPECT_TRUE(is_open(new_fd));
        owner.reset(new_fd);
        EXPECT_TRUE(is_open(new_fd));
        owner.reset();
        EXPECT_EQ(owner.get(), -1);
        EXPECT_FALSE(is_open(new_fd));
    }

    if(is_open(old_fd)) ::close(old_fd);
    if(is_open(new_fd)) ::close(new_fd);
}

TEST(UniqueFd, MoveConstructionTransfersOwnership) {
    int ends[2];
    ASSERT_EQ(::pipe(ends), 0);
    const int read_fd = ends[0];
    ::close(ends[1]);

    {
        miniposix::UniqueFd source{read_fd};
        miniposix::UniqueFd target{std::move(source)};
        EXPECT_EQ(source.get(), -1);
        EXPECT_EQ(target.get(), read_fd);
        EXPECT_TRUE(is_open(read_fd));
    }

    EXPECT_FALSE(is_open(read_fd));
    if(is_open(read_fd)) ::close(read_fd);
}

TEST(UniqueFd, MoveAssignmentClosesPreviousAndTransfersNewDescriptor) {
    int first[2];
    int second[2];
    ASSERT_EQ(::pipe(first), 0);
    ASSERT_EQ(::pipe(second), 0);
    const int old_fd = first[0];
    const int new_fd = second[0];
    ::close(first[1]);
    ::close(second[1]);

    {
        miniposix::UniqueFd target{old_fd};
        miniposix::UniqueFd source{new_fd};
        target = std::move(source);
        EXPECT_FALSE(is_open(old_fd));
        EXPECT_EQ(source.get(), -1);
        EXPECT_EQ(target.get(), new_fd);
        auto& same_target = target;
        target = std::move(same_target);
        EXPECT_EQ(target.get(), new_fd);
        EXPECT_TRUE(is_open(new_fd));
    }

    EXPECT_FALSE(is_open(new_fd));
    if(is_open(old_fd)) ::close(old_fd);
    if(is_open(new_fd)) ::close(new_fd);
}

TEST(UniqueFd, ReleaseTwiceLeavesReleasedDescriptorOpen) {
    int ends[2];
    ASSERT_EQ(::pipe(ends), 0);
    const int read_fd = ends[0];
    ::close(ends[1]);

    {
        miniposix::UniqueFd owner{read_fd};
        EXPECT_EQ(owner.release(), read_fd);
        EXPECT_EQ(owner.release(), -1);
        EXPECT_EQ(owner.get(), -1);
        EXPECT_TRUE(is_open(read_fd));
    }

    // The destructor of the now-empty owner must not close the released fd.
    EXPECT_TRUE(is_open(read_fd));
    if(is_open(read_fd)) ::close(read_fd);
}

TEST(UniqueFd, MovedFromObjectCanAdoptNewDescriptor) {
    int first[2];
    int second[2];
    ASSERT_EQ(::pipe(first), 0);
    ASSERT_EQ(::pipe(second), 0);
    const int first_fd = first[0];
    const int second_fd = second[0];
    ::close(first[1]);
    ::close(second[1]);

    {
        miniposix::UniqueFd source{first_fd};
        miniposix::UniqueFd target{std::move(source)};
        ASSERT_EQ(source.get(), -1);
        ASSERT_EQ(target.get(), first_fd);

        source.reset(second_fd);
        EXPECT_EQ(source.get(), second_fd);
        EXPECT_TRUE(is_open(second_fd));
        EXPECT_EQ(target.get(), first_fd);
    }

    // Both descriptors are closed exactly once, by their current owners.
    EXPECT_FALSE(is_open(first_fd));
    EXPECT_FALSE(is_open(second_fd));
    if(is_open(first_fd)) ::close(first_fd);
    if(is_open(second_fd)) ::close(second_fd);
}
