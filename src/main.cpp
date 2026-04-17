/*  statusd — notification-to-statusbar daemon
 *
 *  Reads lines from a FIFO in the format:
 *      <channel>|<priority>|<text>
 *
 *  Maintains a per-channel notification store (newer replaces older for the
 *  same channel).  Notifications scroll right-to-left through a fixed-width
 *  area; the clock occupies the right-hand edge.
 *
 *  Output is written to a plain text file intended for waybar's custom module.
 */

#include "statusd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

// ============================================================
//  ScrollState::window  (defined here to keep the header lean)
// ============================================================

std::string ScrollState::window() const
{
    // We work in bytes but the contract with the caller is that every
    // character in `tape` occupies exactly one terminal column (NF glyphs
    // are single-width).  Multi-byte UTF-8 sequences are therefore treated
    // as atomic units — we count *codepoints*, not bytes.
    //
    // Strategy: iterate codepoints, skip `offset` of them, then collect
    // up to SCROLL_WIDTH of them.

    auto it  = tape.cbegin();
    auto end = tape.cend();

    // Helper: advance `it` by one UTF-8 codepoint, return false at end
    auto next_cp = [&]() -> bool {
        if (it == end) return false;
        unsigned char c = static_cast<unsigned char>(*it);
        int len = 1;
        if      ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else                         len = 1; // malformed: skip byte
        it += std::min<std::ptrdiff_t>(len, end - it);
        return true;
    };

    // Skip `offset` codepoints
    for (int i = 0; i < offset && it != end; ++i)
        next_cp();

    // Collect up to SCROLL_WIDTH codepoints
    std::string result;
    result.reserve(config::SCROLL_WIDTH * 4); // worst-case 4 bytes/cp
    int collected = 0;
    auto cp_start = it;
    while (collected < config::SCROLL_WIDTH) {
        cp_start = it;
        if (!next_cp()) break;
        result.append(cp_start, it);
        ++collected;
    }

    // Pad with spaces if we ran out of tape
    result.append(static_cast<std::string::size_type>(config::SCROLL_WIDTH - collected), ' ');
    return result;
}

// ============================================================
//  Notification queue — one slot per channel, priority-ordered dequeue
// ============================================================

class NotificationStore
{
public:
    /// Insert or replace a notification for its channel.
    void upsert(Notification n)
    {
        store_[n.channel] = std::move(n);
    }

    /// Returns true if there is anything waiting.
    [[nodiscard]] bool empty() const { return store_.empty(); }

    /// Removes and returns the highest-priority notification.
    /// If two channels share the same priority, the one whose channel name
    /// sorts first is chosen (deterministic).
    [[nodiscard]] std::optional<Notification> pop_highest()
    {
        if (store_.empty()) return std::nullopt;

        auto best = store_.begin();
        for (auto it = std::next(best); it != store_.end(); ++it) {
            if (it->second.priority > best->second.priority)
                best = it;
        }

        Notification n = std::move(best->second);
        store_.erase(best);
        return n;
    }

private:
    std::map<std::string, Notification> store_;
};

// ============================================================
//  FIFO parsing
// ============================================================

/// Parse a single line "channel|priority|text" into a Notification.
/// Returns nullopt if the line is malformed.
static std::optional<Notification> parse_line(std::string_view line)
{
    // Trim trailing newline / carriage-return
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.remove_suffix(1);
    if (line.empty()) return std::nullopt;

    auto first_pipe = line.find('|');
    if (first_pipe == std::string_view::npos) return std::nullopt;

    auto second_pipe = line.find('|', first_pipe + 1);
    if (second_pipe == std::string_view::npos) return std::nullopt;

    std::string channel{line.substr(0, first_pipe)};
    std::string_view prio_sv = line.substr(first_pipe + 1, second_pipe - first_pipe - 1);
    std::string text{line.substr(second_pipe + 1)};

    if (channel.empty() || prio_sv.empty() || text.empty()) return std::nullopt;

    int priority = 0;
    for (char c : prio_sv) {
        if (c < '0' || c > '9') return std::nullopt;
        priority = priority * 10 + (c - '0');
    }
    priority = std::clamp(priority, 0, 9);

    return Notification{std::move(channel), priority, std::move(text)};
}

// ============================================================
//  Clock helper
// ============================================================

static std::string current_time_string()
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;   // "HH:MM:SS" — 8 chars
}

// ============================================================
//  timerfd helpers
// ============================================================

static int make_timerfd_ms(int period_ms)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) throw std::runtime_error(std::string("timerfd_create: ") + strerror(errno));

    itimerspec its{};
    its.it_value.tv_sec     = period_ms / 1000;
    its.it_value.tv_nsec    = (period_ms % 1000) * 1'000'000L;
    its.it_interval         = its.it_value;
    if (timerfd_settime(fd, 0, &its, nullptr) < 0)
        throw std::runtime_error(std::string("timerfd_settime: ") + strerror(errno));
    return fd;
}

static void drain_timerfd(int fd)
{
    uint64_t expirations = 0;
    // We read to drain the fd; the expiration count is not used.
    // The [[nodiscard]] suppression via a named variable satisfies -Wunused-result.
    ssize_t rc = read(fd, &expirations, sizeof(expirations));
    (void)rc;
}

// ============================================================
//  FIFO management
// ============================================================

/// Ensure the directory and FIFO exist.
static void ensure_fifo(const std::string& path)
{
    namespace fs = std::filesystem;
    fs::path p{path};
    fs::create_directories(p.parent_path());

    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISFIFO(st.st_mode))
            throw std::runtime_error(path + " exists but is not a FIFO");
        return; // already present
    }
    if (mkfifo(path.c_str(), 0666) < 0)
        throw std::runtime_error(std::string("mkfifo: ") + strerror(errno));
}

/// Open the FIFO for reading without blocking.
/// We also open a write-end fd and keep it alive so that the read end never
/// sees EOF when the last external writer disconnects.
struct FifoPair {
    int read_fd  = -1;
    int write_fd = -1;  ///< "dummy" writer to hold the pipe open
};

static FifoPair open_fifo(const std::string& path)
{
    // Open read end non-blocking first (won't block because we open write end next)
    int rfd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (rfd < 0) throw std::runtime_error(std::string("open(read): ") + strerror(errno));

    int wfd = open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (wfd < 0) {
        close(rfd);
        throw std::runtime_error(std::string("open(write): ") + strerror(errno));
    }
    return {rfd, wfd};
}

// ============================================================
//  Output writer
// ============================================================

static void write_output(const std::string& scroll_window,
                         const std::string& clock_str,
                         const std::string& output_path)
{
    // Write atomically via a temp file then rename
    std::string tmp = output_path + ".tmp";
    {
        std::ofstream f{tmp, std::ios::trunc};
        if (!f) return; // best-effort; don't crash on output errors
        // clock field: space + HH:MM:SS = 9 chars
        f << scroll_window << ' ' << clock_str << '\n';
    }
    std::rename(tmp.c_str(), output_path.c_str());
}

// ============================================================
//  Signal handling
// ============================================================

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running.store(false); }

// ============================================================
//  Build the scroll tape for a notification
// ============================================================
//
//  The tape layout is:
//
//    [SCROLL_WIDTH spaces] SEPARATOR text SEPARATOR [SCROLL_WIDTH spaces]
//
//  At offset 0  the window shows SCROLL_WIDTH spaces (blank entry).
//  The text enters from the right as the offset increases.
//  At the end of the tape the window shows SCROLL_WIDTH spaces again.
//  The next notification is loaded when finished() returns true.

static std::string build_tape(const std::string& text)
{
    std::string tape;
    tape.reserve(2 * config::SCROLL_WIDTH + 2 * config::SEPARATOR.size() + text.size());
    tape.append(config::SCROLL_WIDTH, ' ');
    tape.append(config::SEPARATOR);
    tape.append(text);
    tape.append(config::SEPARATOR);
    tape.append(config::SCROLL_WIDTH, ' ');
    return tape;
}

// ============================================================
//  Main event loop
// ============================================================

int main()
{
    // Block SIGPIPE (can occur when writing to files/pipes under some conditions)
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);

    const std::string fifo_path   = std::string(config::FIFO_PATH);
    const std::string output_path = std::string(config::OUTPUT_FILE);

    try {
        ensure_fifo(fifo_path);
        // Ensure output directory exists
        std::filesystem::create_directories(
            std::filesystem::path(output_path).parent_path());
    } catch (const std::exception& e) {
        std::cerr << "statusd: setup error: " << e.what() << '\n';
        return 1;
    }

    FifoPair fifo{};
    try {
        fifo = open_fifo(fifo_path);
    } catch (const std::exception& e) {
        std::cerr << "statusd: " << e.what() << '\n';
        return 1;
    }

    int scroll_tfd = -1;
    int clock_tfd  = -1;
    int epfd       = -1;

    try {
        scroll_tfd = make_timerfd_ms(config::SCROLL_TICK_MS);
        clock_tfd  = make_timerfd_ms(1000);

        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) throw std::runtime_error(std::string("epoll_create1: ") + strerror(errno));

        auto epoll_add = [&](int fd, uint32_t events, uint64_t data) {
            epoll_event ev{};
            ev.events   = events;
            ev.data.u64 = data;
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
                throw std::runtime_error(std::string("epoll_ctl: ") + strerror(errno));
        };

        constexpr uint64_t ID_FIFO   = 0;
        constexpr uint64_t ID_SCROLL = 1;
        constexpr uint64_t ID_CLOCK  = 2;

        epoll_add(fifo.read_fd, EPOLLIN, ID_FIFO);
        epoll_add(scroll_tfd,   EPOLLIN, ID_SCROLL);
        epoll_add(clock_tfd,    EPOLLIN, ID_CLOCK);

        NotificationStore store;
        ScrollState        scroll{"", 0};
        std::string        clock_str = current_time_string();
        std::string        current_window(config::SCROLL_WIDTH, ' ');

        // Partial-line buffer for FIFO reads
        std::string line_buf;

        // Write initial blank output immediately so waybar has a file to read
        write_output(current_window, clock_str, output_path);

        constexpr int MAX_EVENTS = 8;
        std::array<epoll_event, MAX_EVENTS> events{};

        while (g_running.load()) {
            int n = epoll_wait(epfd, events.data(), MAX_EVENTS, /*timeout_ms=*/500);
            if (n < 0) {
                if (errno == EINTR) continue;
                std::cerr << "statusd: epoll_wait: " << strerror(errno) << '\n';
                break;
            }

            bool output_dirty = false;

            for (int i = 0; i < n; ++i) {
                uint64_t id = events[i].data.u64;

                // ---- FIFO readable ----------------------------------------
                if (id == ID_FIFO) {
                    char buf[4096];
                    while (true) {
                        ssize_t r = read(fifo.read_fd, buf, sizeof(buf));
                        if (r < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            // Other errors: log and continue
                            std::cerr << "statusd: fifo read: " << strerror(errno) << '\n';
                            break;
                        }
                        if (r == 0) break; // EOF — shouldn't happen with dummy writer

                        line_buf.append(buf, static_cast<std::size_t>(r));

                        // Process all complete lines in the buffer
                        std::string::size_type pos;
                        while ((pos = line_buf.find('\n')) != std::string::npos) {
                            std::string line = line_buf.substr(0, pos + 1);
                            line_buf.erase(0, pos + 1);

                            if (auto n_opt = parse_line(line)) {
                                store.upsert(std::move(*n_opt));
                            }
                        }
                    }
                }

                // ---- Scroll tick ------------------------------------------
                else if (id == ID_SCROLL) {
                    drain_timerfd(scroll_tfd);

                    if (scroll.tape.empty()) {
                        // No active notification — try to dequeue one
                        if (auto next = store.pop_highest()) {
                            scroll.tape   = build_tape(next->text);
                            scroll.offset = 0;
                        }
                        // If still empty, current_window stays blank (spaces)
                    }

                    if (!scroll.tape.empty()) {
                        current_window = scroll.window();
                        scroll.offset++;

                        if (scroll.finished()) {
                            // This notification has fully scrolled off.
                            // Immediately try to load the next one so the tape
                            // transition is seamless (no extra blank frame).
                            if (auto next = store.pop_highest()) {
                                scroll.tape   = build_tape(next->text);
                                scroll.offset = 0;
                            } else {
                                scroll.tape.clear();
                                scroll.offset = 0;
                                current_window.assign(config::SCROLL_WIDTH, ' ');
                            }
                        }

                        output_dirty = true;
                    }
                }

                // ---- Clock tick -------------------------------------------
                else if (id == ID_CLOCK) {
                    drain_timerfd(clock_tfd);
                    clock_str    = current_time_string();
                    output_dirty = true;
                }
            }

            if (output_dirty) {
                write_output(current_window, clock_str, output_path);
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "statusd: fatal: " << e.what() << '\n';
        close(epfd);
        close(scroll_tfd);
        close(clock_tfd);
        close(fifo.read_fd);
        close(fifo.write_fd);
        return 1;
    }

    // Clean shutdown
    close(epfd);
    close(scroll_tfd);
    close(clock_tfd);
    close(fifo.read_fd);
    close(fifo.write_fd);

    std::cout << "statusd: shutdown cleanly\n";
    return 0;
}
