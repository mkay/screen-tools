# screen-tools

A [Wayfire](https://github.com/WayfireWM/wayfire) compositor plugin providing crosshair guides, pixel measurement, and a color picker with magnifying loupe. Based on the [crosshair](https://github.com/WayfireWM/wayfire-plugins-extra) plugin by Scott Moreau.

## Features

- **Crosshair guides** — full-screen horizontal and vertical lines that follow the cursor
- **Freeze** — lock the crosshair at the current cursor position
- **Pixel measurement** — click two points to measure pixel distances, displays a rectangle outline with a W x H label
- **Color picker** — magnifying loupe showing a grid of pixels around the cursor, copies the selected color to the clipboard via `wl-copy`
- **Multiple color formats** — hex, rgb, hsl, oklch (configurable in WCM)
- **Cancel** — press ESC to exit measurement or color picker without committing
- Separate colors for crosshair and measurement overlays
- Configurable crosshair fade during measurement
- Works correctly on scaled outputs

## Default Keybindings

| Action | Binding |
|--------|---------|
| Toggle guides | `Ctrl + F1` |
| Cancel | `Escape` |
| Freeze crosshair | `Ctrl + LMB` |
| Measure | `Super + LMB` |
| Pick color | `Super + RMB` |

All bindings are configurable via `wayfire.ini` or Wayfire Config Manager (WCM).

## Building

Requires Wayfire 0.11+ development headers.

```bash
meson setup build --prefix=/usr/local
meson compile -C build
sudo meson install -C build
```

## Configuration

Add `screen-tools` to your plugins list in `~/.config/wayfire.ini`:

```ini
[core]
plugins = ... screen-tools

[screen-tools]
toggle = <ctrl> KEY_F1
cancel = KEY_ESC
freeze = <ctrl> BTN_LEFT
measure = <super> BTN_LEFT
picker = <super> BTN_RIGHT
picker_format = hex
line_color = \#FF0000CC
line_width = 1
measure_color = \#00BFFFCC
measure_font_size = 14
crosshair_measure_opacity = 20
picker_loupe_radius = 4
picker_cell_size = 14
```

### Color Formats

| Format | Example Output |
|--------|---------------|
| `hex` | `#3A8FD4` |
| `rgb` | `rgb(58, 143, 212)` |
| `hsl` | `hsl(207, 65%, 53%)` |
| `oklch` | `oklch(0.638 0.134 249.5)` |

## Dependencies

- Wayfire 0.11+
- `wl-copy` (from `wl-clipboard`) for clipboard support

## License

MIT
