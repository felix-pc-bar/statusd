#pragma once

#include <string>
#include <string_view>
#include <chrono>
#include <map>
#include <optional>

// ============================================================
//  User-tunable configuration — edit these freely
// ============================================================

namespace config {

/// Total width of the output line in terminal columns.
/// NB: Nerd Font glyphs are typically 1 column wide; double-width
///     Unicode (CJK etc.) would break the column count.
inline constexpr int BAR_WIDTH = 213;

/// Width reserved for the clock on the right: " HH:MM:SS" (9 chars)
inline constexpr int CLOCK_WIDTH = 9;

/// Width of the scrolling notification area
inline constexpr int SCROLL_WIDTH = BAR_WIDTH - CLOCK_WIDTH;

/// Milliseconds between each one-column scroll step
inline constexpr int SCROLL_TICK_MS = 100;

/// Separator inserted between the end of one notification and the
/// start of the next as they flow through the scroll area.
/// Also used as the leading/trailing padding sentinel on the tape.
inline constexpr std::string_view SEPARATOR = "  ❯  ";

/// Path to the FIFO that clients write notifications into
inline constexpr std::string_view FIFO_PATH = "/run/statusd/statusd.fifo";

/// Path to the output file that waybar (or any other reader) polls
inline constexpr std::string_view OUTPUT_FILE = "/run/statusd/statusd.txt";

} // namespace config

// ============================================================
//  Domain types
// ============================================================

/// A single notification as parsed from the FIFO
struct Notification {
    std::string channel;   ///< Logical source, e.g. "git", "pomo"
    int         priority;  ///< 0-9; higher wins when dequeuing
    std::string text;      ///< Display text (may contain Nerd Font codepoints)

    /// For priority_queue / sorting: higher priority sorts first
    bool operator<(const Notification& o) const {
        return priority < o.priority;          // intentionally reversed below
    }
};

/// State of the currently active scrolling notification
struct ScrollState {
    std::string tape;   ///< The full virtual tape string to scroll through
    int         offset; ///< Current left-edge index into `tape`

    /// Returns true when the offset has passed the end of the tape
    [[nodiscard]] bool finished() const {
        return offset >= static_cast<int>(tape.size());
    }

    /// Extracts a fixed-width view of SCROLL_WIDTH columns starting at offset.
    /// Any columns past the end of the tape are filled with spaces.
    [[nodiscard]] std::string window() const;
};
