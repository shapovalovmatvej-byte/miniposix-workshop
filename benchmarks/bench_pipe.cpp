#include "miniposix/pipe.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

    constexpr std::size_t kMib = 1024 * 1024;

    // The verification pattern repeats every 251 bytes. Keeping the period as a
    // named constant lets the fast path precompute one reference block.
    constexpr std::size_t kPatternPeriod = 251;

    // Byte: original per-byte modulo loop. Memcmp: precomputed reference block
    // copied/compared with memcpy/memcmp. Both verify every transferred byte.
    enum class PatternMode { Byte, Memcmp };

    struct Options {
        std::filesystem::path output = "bench-results/pipe.csv";
        std::size_t total_bytes = 8 * kMib;
        std::size_t repeats = 3;
        std::vector<std::size_t> buffer_sizes = {512, 4096, 65536};
        std::size_t pipe_size = 0; // 0 leaves the kernel default (64 KiB) alone.
        PatternMode pattern_mode = PatternMode::Byte;
    };

    struct Result {
        std::string variant;
        std::size_t buffer_bytes;
        std::size_t total_bytes;
        std::size_t repeat;
        double seconds;
        double throughput_mib_s;
    };

    std::size_t positive_integer(std::string_view text, std::string_view option) {
        std::size_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
            throw std::invalid_argument(std::string(option) + " requires a positive integer");
        }
        return value;
    }

    std::size_t nonnegative_integer(std::string_view text, std::string_view option) {
        std::size_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            throw std::invalid_argument(std::string(option) + " requires a non-negative integer");
        }
        return value;
    }

    PatternMode parse_pattern_mode(std::string_view text) {
        if (text == "byte") {
            return PatternMode::Byte;
        }
        if (text == "memcmp") {
            return PatternMode::Memcmp;
        }
        throw std::invalid_argument("--pattern-mode expects byte or memcmp");
    }

    std::vector<std::size_t> parse_buffer_sizes(std::string_view text) {
        std::vector<std::size_t> values;
        while (!text.empty()) {
            const auto separator = text.find(',');
            const auto piece = text.substr(0, separator);
            values.push_back(positive_integer(piece, "--buffer-sizes"));
            if (separator == std::string_view::npos) {
                break;
            }
            text.remove_prefix(separator + 1);
            if (text.empty()) {
                throw std::invalid_argument("--buffer-sizes cannot end with a comma");
            }
        }
        if (values.empty()) {
            throw std::invalid_argument("--buffer-sizes requires a comma-separated list");
        }
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }

    void usage(const char *program) {
        std::cout << "Usage: " << program
                  << " [--output FILE] [--bytes N] [--repeats N] [--buffer-sizes N,N,...]\n"
                  << "       [--pipe-size N] [--pattern-mode byte|memcmp]\n"
                  << "Defaults: bench-results/pipe.csv, 8388608 bytes, 3 repeats, 512,4096,65536 byte buffers,\n"
                  << "          kernel-default pipe capacity, byte pattern. --pipe-size 0 keeps the default;\n"
                  << "          values above /proc/sys/fs/pipe-max-size are refused unless privileged.\n";
    }

    Options parse_options(int argc, char **argv) {
        Options options;
        for (int index = 1; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (option == "--help") {
                usage(argv[0]);
                std::exit(0);
            }
            if (index + 1 == argc) {
                throw std::invalid_argument(std::string(option) + " requires a value");
            }
            const std::string_view value(argv[++index]);
            if (option == "--output") {
                if (value.empty()) {
                    throw std::invalid_argument("--output requires a file path");
                }
                options.output = value;
            } else if (option == "--bytes") {
                options.total_bytes = positive_integer(value, option);
            } else if (option == "--repeats") {
                options.repeats = positive_integer(value, option);
            } else if (option == "--buffer-sizes") {
                options.buffer_sizes = parse_buffer_sizes(value);
            } else if (option == "--pipe-size") {
                options.pipe_size = nonnegative_integer(value, option);
            } else if (option == "--pattern-mode") {
                options.pattern_mode = parse_pattern_mode(value);
            } else {
                throw std::invalid_argument("unknown option: " + std::string(option));
            }
        }
        return options;
    }

    unsigned char byte_at(std::size_t offset) {
        return static_cast<unsigned char>(offset % kPatternPeriod);
    }

    // Precomputed reference block for the offset-dependent test pattern. The
    // extra period allows a single memcpy/memcmp for any absolute offset phase.
    class TestPattern {
    public:
        explicit TestPattern(std::size_t buffer_bytes) : reference_(buffer_bytes + kPatternPeriod) {
            for (std::size_t offset = 0; offset < reference_.size(); ++offset) {
                reference_[offset] = byte_at(offset);
            }
        }

        void fill(std::size_t absolute_offset, unsigned char *destination, std::size_t count) const {
            std::memcpy(destination, reference_.data() + phase(absolute_offset), count);
        }

        [[nodiscard]] bool verify(std::size_t absolute_offset, const unsigned char *source, std::size_t count) const {
            return std::memcmp(source, reference_.data() + phase(absolute_offset), count) == 0;
        }

    private:
        [[nodiscard]] static std::size_t phase(std::size_t absolute_offset) { return absolute_offset % kPatternPeriod; }

        std::vector<unsigned char> reference_;
    };

    void fill_block(unsigned char *buffer, std::size_t count, std::size_t absolute_offset, PatternMode mode,
                    const TestPattern &pattern) {
        if (mode == PatternMode::Memcmp) {
            pattern.fill(absolute_offset, buffer, count);
            return;
        }
        for (std::size_t offset = 0; offset < count; ++offset) {
            buffer[offset] = byte_at(absolute_offset + offset);
        }
    }

    bool verify_block(const unsigned char *buffer, std::size_t count, std::size_t absolute_offset, PatternMode mode,
                      const TestPattern &pattern) {
        if (mode == PatternMode::Memcmp) {
            return pattern.verify(absolute_offset, buffer, count);
        }
        bool correct = true;
        for (std::size_t offset = 0; offset < count; ++offset) {
            if (buffer[offset] != byte_at(absolute_offset + offset)) {
                correct = false;
            }
        }
        return correct;
    }

    void write_exactly(int descriptor, std::size_t total_bytes, std::size_t buffer_bytes, PatternMode mode,
                       const TestPattern &pattern) {
        std::vector<unsigned char> buffer(buffer_bytes);
        std::size_t sent = 0;
        while (sent < total_bytes) {
            const auto count = std::min(buffer_bytes, total_bytes - sent);
            fill_block(buffer.data(), count, sent, mode, pattern);
            std::size_t cursor = 0;
            while (cursor < count) {
                const auto written = ::write(descriptor, buffer.data() + cursor, count - cursor);
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                if (written < 0) {
                    throw std::system_error(errno, std::generic_category(), "pipe write");
                }
                if (written == 0) {
                    throw std::runtime_error("pipe write made no progress");
                }
                cursor += static_cast<std::size_t>(written);
            }
            sent += count;
        }
    }

    bool read_and_verify(int descriptor, std::size_t total_bytes, std::size_t buffer_bytes, PatternMode mode,
                         const TestPattern &pattern) {
        std::vector<unsigned char> buffer(buffer_bytes);
        std::size_t received = 0;
        bool correct = true;
        while (received < total_bytes) {
            const auto count = std::min(buffer_bytes, total_bytes - received);
            const auto read_count = ::read(descriptor, buffer.data(), count);
            if (read_count < 0 && errno == EINTR) {
                continue;
            }
            if (read_count < 0) {
                throw std::system_error(errno, std::generic_category(), "pipe read");
            }
            if (read_count == 0) {
                throw std::runtime_error("pipe reached EOF before the expected byte count");
            }
            const auto received_count = static_cast<std::size_t>(read_count);
            if (!verify_block(buffer.data(), received_count, received, mode, pattern)) {
                correct = false;
            }
            received += received_count;
        }
        return correct;
    }

    int set_pipe_capacity(int descriptor, std::size_t requested) {
#ifdef F_SETPIPE_SZ
        if (requested != 0) {
            if (requested > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                std::cerr << "warning: F_SETPIPE_SZ(" << requested << ") is too large; keeping the default\n";
            } else {
                const auto size = static_cast<int>(requested);
                if (::fcntl(descriptor, F_SETPIPE_SZ, size) == -1) {
                    // Capacity is a best-effort tuning knob: keep the kernel default.
                    std::cerr << "warning: F_SETPIPE_SZ(" << requested << ") failed: " << std::strerror(errno) << '\n';
                }
            }
        }
        return ::fcntl(descriptor, F_GETPIPE_SZ);
#else
        (void) descriptor;
        if (requested != 0) {
            std::cerr << "warning: F_SETPIPE_SZ is not available on this platform\n";
        }
        return -1;
#endif
    }

    std::string variant_name(PatternMode mode, std::size_t requested_pipe_size, int pipe_bytes) {
        std::string name = "miniposix_pipe";
        if (mode == PatternMode::Memcmp) {
            name += "_memcmp";
        }
        if (requested_pipe_size != 0) {
            const auto capacity = pipe_bytes > 0 ? static_cast<std::size_t>(pipe_bytes) : requested_pipe_size;
            name += "_pipesz_" + std::to_string(capacity);
        }
        return name;
    }

    Result measure(std::size_t total_bytes, std::size_t buffer_bytes, std::size_t repeat, PatternMode mode,
                   std::size_t pipe_size) {
        auto pipe = miniposix::Pipe::create();
        const int pipe_bytes = set_pipe_capacity(pipe.write_end().get(), pipe_size);
        // Built before the clock starts: it is experiment fixture, not transfer work.
        const TestPattern pattern(buffer_bytes);
        std::exception_ptr writer_failure;
        std::exception_ptr reader_failure;
        bool correct = false;

        const auto start = std::chrono::steady_clock::now();
        std::jthread writer([&] {
            try {
                write_exactly(pipe.write_end().get(), total_bytes, buffer_bytes, mode, pattern);
            } catch (...) {
                writer_failure = std::current_exception();
            }
            pipe.write_end().reset();
        });
        try {
            correct = read_and_verify(pipe.read_end().get(), total_bytes, buffer_bytes, mode, pattern);
        } catch (...) {
            reader_failure = std::current_exception();
            // Release the reader so a failed run cannot leave the writer blocked.
            pipe.read_end().reset();
        }
        writer.join();
        const auto end = std::chrono::steady_clock::now();

        if (writer_failure) {
            std::rethrow_exception(writer_failure);
        }
        if (reader_failure) {
            std::rethrow_exception(reader_failure);
        }
        if (!correct) {
            throw std::runtime_error("pipe transferred corrupted data");
        }

        const double seconds = std::chrono::duration<double>(end - start).count();
        if (seconds <= 0) {
            throw std::runtime_error("clock did not advance during measurement");
        }
        return {variant_name(mode, pipe_size, pipe_bytes), buffer_bytes, total_bytes, repeat, seconds,
                (static_cast<double>(total_bytes) / static_cast<double>(kMib)) / seconds};
    }

    void save_results(const std::filesystem::path &output, const std::vector<Result> &results) {
        if (!output.parent_path().empty()) {
            std::filesystem::create_directories(output.parent_path());
        }
        const bool needs_header = !std::filesystem::exists(output) || std::filesystem::file_size(output) == 0;
        std::ofstream file(output, std::ios::app);
        if (!file) {
            throw std::runtime_error("cannot open CSV output: " + output.string());
        }
        if (needs_header) {
            file << "variant,buffer_bytes,total_bytes,seconds,throughput_mib_s,repeat\n";
        }
        file << std::setprecision(12);
        for (const auto &result: results) {
            file << result.variant << ',' << result.buffer_bytes << ',' << result.total_bytes << ','
                 << result.seconds << ',' << result.throughput_mib_s << ',' << result.repeat << '\n';
        }
        if (!file) {
            throw std::runtime_error("failed to write CSV output: " + output.string());
        }
    }

} // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_options(argc, argv);
        // A failed reader closes its end; EPIPE then lets the writer exit and join.
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            throw std::system_error(errno, std::generic_category(), "ignore SIGPIPE");
        }
        std::vector<Result> results;
        for (const auto buffer_bytes: options.buffer_sizes) {
            for (std::size_t repeat = 1; repeat <= options.repeats; ++repeat) {
                const auto result = measure(options.total_bytes, buffer_bytes, repeat, options.pattern_mode,
                                            options.pipe_size);
                results.push_back(result);
                std::cout << result.variant << ", " << buffer_bytes << " bytes, repeat " << repeat << ": "
                          << std::fixed << std::setprecision(2) << result.throughput_mib_s << " MiB/s\n";
            }
        }
        save_results(options.output, results);
        std::cout << "Saved " << results.size() << " measurements to " << options.output << '\n';
    } catch (const std::exception &error) {
        std::cerr << "bench_pipe: " << error.what() << '\n';
        return 1;
    }
}
