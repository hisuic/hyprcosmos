#include "user_config_reader.hpp"

#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>

namespace {

int assertions = 0;

void check(bool condition, std::string_view description) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string(description));
}

class Fixtures {
  public:
    Fixtures() {
        char pattern[] = "/tmp/hyprcosmos-user-reader-test.XXXXXX";
        const char* result = ::mkdtemp(pattern);
        if (!result) throw std::runtime_error("Cannot create isolated reader fixtures");
        directory = result;
    }
    ~Fixtures() {
        for (const auto& file : files) ::unlink(file.c_str());
        for (const auto& child : directories) ::rmdir(child.c_str());
        ::rmdir(directory.c_str());
    }
    std::string path(std::string_view name) const { return directory + "/" + std::string(name); }
    std::string file(std::string_view name, std::string_view contents) {
        const auto filename = path(name);
        files.push_back(filename);
        std::ofstream output(filename, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) throw std::runtime_error("Cannot write isolated reader fixture");
        return filename;
    }
    std::string node(std::string_view name) { const auto result = path(name); files.push_back(result); return result; }
    std::string subdirectory(std::string_view name) {
        const auto result = path(name);
        if (::mkdir(result.c_str(), 0700) != 0) throw std::runtime_error("Cannot create fixture directory");
        directories.push_back(result);
        return result;
    }
  private:
    std::string directory;
    std::vector<std::string> files, directories;
};

void rejected(const cosmic::ReadResult& result, std::string_view path, std::string_view reason) {
    check(result.found && !result.source && !result.error.empty(), "invalid existing files fail closed with an error");
    check(result.error.find(path) != std::string::npos, "reader errors identify the exact file path");
    check(result.error.find(reason) != std::string::npos, "reader errors identify the failure reason");
}

std::size_t descriptors() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (!directory) throw std::runtime_error("Cannot inspect fixture process descriptor count");
    std::size_t count = 0;
    while (const auto* entry = ::readdir(directory))
        if (entry->d_name[0] != '.') ++count;
    ::closedir(directory);
    return count;
}

void run() {
    Fixtures fixtures;
    const auto missing = fixtures.path("absent.lua");
    const auto absent = cosmic::readUserConfig(missing);
    check(!absent.found && !absent.source && absent.error.empty(), "only a truly missing file requests default fallback");

    constexpr std::string_view contents = "return { idle_timeout = 20 }\n";
    const auto regular = fixtures.file("regular.lua", contents);
    const auto loaded = cosmic::readUserConfig(regular);
    check(loaded.found && loaded.source && *loaded.source == contents && loaded.error.empty(),
          "regular file bytes are read exactly without interpretation");
    const auto empty = cosmic::readUserConfig(fixtures.file("empty.lua", ""));
    check(empty.found && empty.source && empty.source->empty() && empty.error.empty(),
          "an empty regular file remains a found source, not a missing-file fallback");
    const auto newlineName = fixtures.file("name with\nnewline.lua", contents);
    const auto newlineFile = cosmic::readUserConfig(newlineName);
    check(newlineFile.found && newlineFile.source && *newlineFile.source == contents && newlineFile.error.empty(),
          "regular file names with whitespace and newlines are supported");

    const auto symlink = fixtures.node("regular-link.lua");
    check(::symlink(regular.c_str(), symlink.c_str()) == 0, "regular-file symlink fixture is created");
    const auto linked = cosmic::readUserConfig(symlink);
    check(linked.found && linked.source && *linked.source == contents && linked.error.empty(),
          "symlinks to regular files are supported");
    const auto dangling = fixtures.node("dangling.lua");
    check(::symlink(missing.c_str(), dangling.c_str()) == 0, "dangling symlink fixture is created");
    rejected(cosmic::readUserConfig(dangling), dangling, "missing target");
    const auto loop = fixtures.node("loop.lua");
    check(::symlink(loop.c_str(), loop.c_str()) == 0, "symlink-loop fixture is created");
    rejected(cosmic::readUserConfig(loop), loop, "open failed");

    const auto fifo = fixtures.node("never-written.fifo");
    check(::mkfifo(fifo.c_str(), 0600) == 0, "a real FIFO without any writer is created");
    const auto started = std::chrono::steady_clock::now();
    rejected(cosmic::readUserConfig(fifo), fifo, "regular file");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
          "an unwritten FIFO is rejected immediately rather than blocking the compositor");
    const auto fifoLink = fixtures.node("fifo-link.lua");
    check(::symlink(fifo.c_str(), fifoLink.c_str()) == 0, "FIFO symlink fixture is created");
    rejected(cosmic::readUserConfig(fifoLink), fifoLink, "regular file");
    const auto directory = fixtures.subdirectory("not-a-file");
    rejected(cosmic::readUserConfig(directory), directory, "regular file");
    rejected(cosmic::readUserConfig("/dev/null"), "/dev/null", "regular file");

    const auto socketPath = fixtures.node("not-a-file.socket");
    const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(socket >= 0, "isolated UNIX socket fixture is created");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    check(socketPath.size() < sizeof(address.sun_path), "fixture socket path fits the UNIX address");
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    const int bound = ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    ::close(socket);
    check(bound == 0, "isolated UNIX socket fixture is bound");
    rejected(cosmic::readUserConfig(socketPath), socketPath, "open failed");

    const auto protectedFile = fixtures.file("unreadable.lua", contents);
    check(::chmod(protectedFile.c_str(), 0000) == 0, "read permissions can be removed from the fixture");
    const auto inaccessible = cosmic::readUserConfig(protectedFile);
    if (::geteuid() == 0)
        check(inaccessible.source.has_value(), "privileged test process retains its OS-provided read permission");
    else
        rejected(inaccessible, protectedFile, "open failed");
    check(::chmod(protectedFile.c_str(), 0600) == 0, "fixture read permissions are restored");

    const auto exactLimit = fixtures.file("one-mib.lua", std::string(cosmic::userConfigMaxBytes, 'x'));
    const auto large = cosmic::readUserConfig(exactLimit);
    check(large.found && large.source && large.source->size() == cosmic::userConfigMaxBytes && large.error.empty(),
          "a regular file exactly one MiB long is accepted");
    const auto tooLarge = fixtures.file("too-large.lua", std::string(cosmic::userConfigMaxBytes + 1, 'x'));
    rejected(cosmic::readUserConfig(tooLarge), tooLarge, "size limit");
    const std::string embeddedNul = regular + std::string("\0ignored", 8);
    const auto invalid = cosmic::readUserConfig(embeddedNul);
    check(invalid.found && !invalid.source && invalid.error.find("NUL") != std::string::npos,
          "embedded NUL is rejected rather than truncating the requested filename");
    const auto emptyPath = cosmic::readUserConfig("");
    check(emptyPath.found && !emptyPath.source && !emptyPath.error.empty(), "empty file paths fail closed");

    const auto before = descriptors();
    for (int iteration = 0; iteration < 128; ++iteration) {
        (void)cosmic::readUserConfig(regular);
        (void)cosmic::readUserConfig(fifo);
        (void)cosmic::readUserConfig(tooLarge);
    }
    check(descriptors() == before, "successful and early-rejected reads do not leak descriptors");
}

} // namespace

int main() {
    try {
        run();
        std::cout << "Native user configuration reader: " << assertions << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native user configuration reader: " << error.what() << '\n';
        return 1;
    }
}
