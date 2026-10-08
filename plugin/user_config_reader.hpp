#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cosmic {

inline constexpr std::size_t userConfigMaxBytes = 1024 * 1024;

struct ReadResult {
    std::optional<std::string> source;
    std::string error;
    bool found = false;
};

namespace user_config_detail {

class FileDescriptor {
  public:
    explicit FileDescriptor(int descriptor) : m_descriptor(descriptor) {}
    ~FileDescriptor() { if (m_descriptor >= 0) ::close(m_descriptor); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return m_descriptor; }
    // Linux releases the descriptor even if close reports EINTR: never retry a
    // close on a descriptor number that another thread may already have reused.
    int finish() { return ::close(std::exchange(m_descriptor, -1)); }
  private:
    int m_descriptor;
};

inline ReadResult failure(std::string_view path, std::string_view stage, int code = 0) {
    std::string error = "cosmic: user config " + std::string(path) + ": " + std::string(stage);
    if (code) error += ": " + std::string(std::strerror(code));
    return {std::nullopt, std::move(error), true};
}

} // namespace user_config_detail

// Linux regular-file reader for the compositor's main thread. In particular,
// opening an unwritten FIFO must never block before we can reject its type.
// Symlinks to regular files are supported; no source bytes are logged here.
inline ReadResult readUserConfig(std::string_view path) {
    using user_config_detail::failure;
    if (path.empty() || path.find('\0') != std::string_view::npos)
        return failure("<invalid path>", "expected a nonempty filename without NUL bytes");
    const std::string filename(path);
    int opened;
    do { opened = ::open(filename.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY); }
    while (opened < 0 && errno == EINTR);
    if (opened < 0) {
        const int openError = errno;
        if (openError == ENOENT) {
            struct stat entry{};
            int inspected;
            do { inspected = ::lstat(filename.c_str(), &entry); }
            while (inspected < 0 && errno == EINTR);
            if (inspected < 0 && errno == ENOENT) return {};
            if (inspected < 0) return failure(path, "cannot inspect missing target", errno);
            return failure(path, "existing file has a missing target", openError);
        }
        return failure(path, "open failed", openError);
    }
    user_config_detail::FileDescriptor descriptor(opened);
    struct stat metadata{};
    int inspected;
    do { inspected = ::fstat(descriptor.get(), &metadata); }
    while (inspected < 0 && errno == EINTR);
    if (inspected < 0) return failure(path, "fstat failed", errno);
    if (!S_ISREG(metadata.st_mode)) return failure(path, "expected a regular file");
    if (metadata.st_size < 0 || static_cast<unsigned long long>(metadata.st_size) > userConfigMaxBytes)
        return failure(path, "file exceeds the one-MiB size limit");

    std::string source;
    source.reserve(static_cast<std::size_t>(metadata.st_size));
    std::array<char, 8192> buffer{};
    while (source.size() <= userConfigMaxBytes) {
        const auto remaining = userConfigMaxBytes + 1 - source.size();
        const auto count = std::min(buffer.size(), remaining);
        const auto received = ::read(descriptor.get(), buffer.data(), count);
        if (received < 0) {
            if (errno == EINTR) continue;
            return failure(path, "read failed", errno);
        }
        if (received == 0) break;
        source.append(buffer.data(), static_cast<std::size_t>(received));
        if (source.size() > userConfigMaxBytes)
            return failure(path, "file grew beyond the one-MiB size limit");
    }
    if (descriptor.finish() < 0) return failure(path, "close failed", errno);
    return {std::move(source), {}, true};
}

} // namespace cosmic
