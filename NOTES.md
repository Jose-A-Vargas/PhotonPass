# PhotonPass Dev Notes

## Session 2026-08-07 — EPD47 partial update / DU waveform

### What was built

`_renderLine` in `src/display.cpp` now uses a custom Direct Update (DU) waveform
(`_duUpdate` / `_buildDuRow`) instead of `epd_clear_area_cycles` + `epd_draw_grayscale_image`.

**Result:** pixel-perfect partial text updates with zero bleed. Only the changed character
column(s) flash. Adjacent text is physically untouched (zero voltage outside the rect).

### How _renderLine works now

1. Compute which character columns changed (skip unchanged prefix/suffix for same-length strings)
2. `epd_fill_rect(clearL, uY, clearW, uH, WHITE, _fb)` — white changed columns in framebuffer
3. `_drawStr(newX, baselineY, newStr)` — draw new text into framebuffer
4. `_duUpdate({pushL, uY, pushW, uH}, _fb, 200, 2)` — drive panel to match framebuffer

`_duUpdate` scans all 540 rows per pass:
- `epd_skip()` for rows outside the rect (CKV advance only, zero voltage)
- `_buildDuRow()` builds a 240-byte 2bpp drive row:
  - white pixel in `_fb` → `0b10` (drive white)
  - dark pixel in `_fb`  → `0b01` (drive black)
  - column outside area  → `0b00` (zero voltage, panel pixel untouched)
- Primes both I2S buffers with first row, then `memcpy(epd_get_current_buffer(), ...)` before
  each `epd_output_row(200)` for per-row data
- 2 passes at `time_dus = 200`; increase to 300 or add a 3rd pass if ghosting persists

### Critical: I2S byte order — do NOT apply reorder_line_buffer on non-uniform data

`reorder_line_buffer()` in `epd_driver.c` swaps the upper/lower 16-bit halves of each 32-bit word.
It is applied inside `epd_push_pixels()` which is only ever called with uniform data
(`0xAA` / `0x55` in every byte — the clear waveform).

**The trap:** swapping `[0xAA, 0xAA, 0xAA, 0xAA]` is a no-op, so the reorder was never tested
against non-uniform pixel data. Applying it to DU data caused the "spliced/shifted" artifact
(characters appearing doubled or at wrong horizontal positions, visible in `unnamed.jpg`).

**Rule:** For any function writing non-uniform data to `epd_get_current_buffer()`, use natural
byte order — no 16-bit half-swap. This matches `calc_epd_input_4bpp` and `calc_epd_input_1bpp`
which both write natural byte order and work correctly.

### Public epdiy primitives available (include ed047tc1.h)

- `epd_start_frame()` / `epd_end_frame()`
- `epd_output_row(uint32_t time_dus)` — latch prev row, CKV pulse, DMA current buf, switch buf
- `epd_skip()` — CKV advance only, no latch, no DMA
- `epd_get_current_buffer()` — 240-byte writable I2S buffer (2bpp, 960px wide)
- `epd_switch_buffer()` — switch buffers (blocks if switched-to is in DMA use)

### Font: MonoFont (Consolas 28pt)

Regen command (run from project root in PowerShell — `>` won't work, writes UTF-16):
```
python .pio\libdeps\epd47_s3\LilyGo-EPD47\scripts\fontconvert.py MonoFont 28 "C:\Windows\Fonts\consola.ttf" | Out-File -Encoding utf8 src\mono.h
```

`_charWidth` is printed at boot: `[EPD47] MonoFont line_height=XX ascender=XX charWidth=XX`
Tune `kAbove`/`kBelow` in `showMessage` (currently 44/16, leftover FiraSans values) once you
have the real ascender.

### Dead code still in display.cpp

`_compactFromFb()` and `_pushRect()` — no longer called, kept for possible future use.
`_pushRect` still has `epd_clear_area_cycles` which bleeds ~5px; don't revive it for narrow rects.

### Next up

Step 11 — Electron/Node.js companion desktop app (`companion/` directory, not yet created).
See `steps.txt` for full roadmap.
