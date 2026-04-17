# statusd

A minimal C++23 daemon that reads notifications from a FIFO and writes a
scrolling status line (with a live clock) to a text file for waybar to display.

---

## Protocol

Write lines to the FIFO in the format:

```
<channel>|<priority>|<text>
```

| Field      | Description                                              |
|------------|----------------------------------------------------------|
| `channel`  | Logical source, e.g. `git`, `pomo`, `build`             |
| `priority` | Integer 0–9. Higher priority is dequeued first.         |
| `text`     | Display text. Nerd Font codepoints are fully supported. |

**One notification per channel at a time.** If you send a second notification
for a channel whose previous notification hasn't scrolled yet, the old one is
replaced by the new one.

### Example

```sh
echo "git|3| main +2 ~1" > /run/statusd/statusd.fifo
echo "build|7|󰏗 build failed: 3 errors" > /run/statusd/statusd.fifo
echo "pomo|5|󰔛 25:00 focus" > /run/statusd/statusd.fifo
```

---

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build          # installs to /usr/local/bin/statusd
```

### Debug build (with ASAN/UBSAN)

```sh
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dbg -j$(nproc)
```

---

## Install as a user service (recommended)

```sh
mkdir -p ~/.config/systemd/user
cp statusd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now statusd
```

For a user service the runtime directory is automatically
`/run/user/<UID>/statusd/`. Update `config::FIFO_PATH` and
`config::OUTPUT_FILE` in `include/statusd.hpp` accordingly, e.g.:

```cpp
inline constexpr std::string_view FIFO_PATH   = "/run/user/1000/statusd/statusd.fifo";
inline constexpr std::string_view OUTPUT_FILE = "/run/user/1000/statusd/statusd.txt";
```

Or use the `$XDG_RUNTIME_DIR` pattern and pass paths via environment variables
if you prefer not to hard-code the UID.

---

## Waybar integration

Add to your waybar `config`:

```jsonc
"custom/statusd": {
    "exec": "tail -f /run/statusd/statusd.txt",
    "interval": "once",
    "format": "{}",
    "tooltip": false
}
```

And in your waybar `style.css`:

```css
#custom-statusd {
    font-family: "JetBrainsMono Nerd Font", monospace;
    font-size: 13px;
}
```

Add `"custom/statusd"` to the relevant position in your bar's `modules-left`,
`modules-center`, or `modules-right`.

---

## Configuration

All tunable constants live in **`include/statusd.hpp`** under `namespace config`:

| Constant          | Default                          | Description                              |
|-------------------|----------------------------------|------------------------------------------|
| `BAR_WIDTH`       | `200`                            | Total output width in terminal columns   |
| `CLOCK_WIDTH`     | `9`                              | Columns reserved for `" HH:MM:SS"`       |
| `SCROLL_WIDTH`    | `BAR_WIDTH - CLOCK_WIDTH`        | Derived; do not change directly          |
| `SCROLL_TICK_MS`  | `100`                            | Milliseconds per one-column scroll step  |
| `SEPARATOR`       | `"  ❯  "`                        | Inserted between notifications           |
| `FIFO_PATH`       | `"/run/statusd/statusd.fifo"`    | FIFO path clients write to               |
| `OUTPUT_FILE`     | `"/run/statusd/statusd.txt"`     | Output file waybar reads                 |

After changing any constant, rebuild and restart the service.
