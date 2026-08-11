# PhotonPass Dev Notes

## Session 2026-08-10 — keyboard() widget + DU waveform fixes

### What was built this session

1. `Display::keyboard()` — full-featured soft keyboard in `src/display.cpp`
2. `Display::KeyboardMode` enum in `src/display.h`
3. Font: MonoFont (Consolas 28pt) — regenerated with ↷ glyph (U+21B7)
4. `_fb180Rotate()` — true 180° framebuffer flip for display rotation
5. `_duUpdate()` post-area skip fix — zero-voltage drain after area ends

### keyboard() layout (960×540)

```
y=  0.. 79  Input row: [ROT btn (63px)] [text box (fills rest)]
y= 80..139  OK zone:   small OK button above the 0 key (x≈868, text-width sized)
y=140..219  Row 0: !  @  #  $  %  ^  &  *  (  )  -  +   (12 keys × 80px)
y=220..299  Row 1: 1  2  3  4  5  6  7  8  9  0          (10 keys × 96px)
y=300..379  Row 2: q  w  e  r  t  y  u  i  o  p          (10 keys × 96px)
y=380..459  Row 3:   a  s  d  f  g  h  j  k  l           ( 9 keys, centred)
y=460..539  Row 4: SFT  z  x  c  v  b  n  m  DEL
```

Constants: `KB2_IN_H=80`, `KB2_OK_H=60`, `KB2_ROW_H=80` (5 rows × 80 = 400px)

NUMPAD mode: 4 rows × 4 cols (240px each), small OK in OK zone above col 1 (x=240)

### Rotation (true 180°)

- `_kb2Rotated` (static bool) persists across `keyboard()` calls
- Toggled by the ROT button (top-left of input area, 63×74px)
- `_fb180Rotate()` reverses row order + reverse+nibble-swap each row in `_fb`
- `fullRedraw()`: draw normally → if rotated: `_fb180Rotate()` → DU
- Touch coords inverted when rotated: `tx = EPD_WIDTH-1-tx`, `ty = EPD_HEIGHT-1-ty`
- On ROT press: `epd_poweron(); epd_clear(); _afterEpd();` then fullRedraw

### Rotation symbol ↷ (U+21B7)

`fontconvert.py` already has U+21B7 in its intervals (patched).
Regen command (from project root, PowerShell):
```
python -X utf8 .pio\libdeps\epd47_s3\LilyGo-EPD47\scripts\fontconvert.py MonoFont 28 "C:\Windows\Fonts\consola.ttf" "C:\Windows\Fonts\seguisym.ttf" | Out-File -Encoding utf8 src\mono.h
```
Then in display.cpp, change ROT button label from `"ROT"` to `"\xE2\x86\xB7"`.

### _duUpdate — critical fixes

**Post-area skip bleed fix (2026-08-10):**
`epd_skip()` only pulses CKV — the source stays latched to the last area row's drive data,
bleeding that pattern onto every subsequent skip row. Fix: after area ends, load zeros into
BOTH I2S buffers and do TWO `epd_output_row(10)` calls before switching to `epd_skip()`:
- Post-skip 0: `epd_output_row(10)` → latches last area row (1 unavoidable bleed row)
- Post-skip 1: `epd_output_row(10)` → latches zeros = zero voltage ✓
- Post-skip 2+: `epd_skip()` with zeros latched = zero voltage ✓

Same fix applied to pre-clear loop inside `_duUpdate`.

**Pre-flush zero fix (earlier):**
Before the final `epd_output_row(time_dus)` pipeline flush, zero the current I2S buffer so
the first post-area row doesn't get the last area row's pattern. Same applies in pre-clear loop.

### keyboard() fast input path

- **Character/DEL press**: `inputRedraw()` — partial DU of input row only (y=0..79)
  - `_duUpdate({0,0,960,80}, _fb, 200, pre_clears=1, passes=1)`
  - Pre-clear ensures old text erased cleanly; 1 pass sufficient for fresh renders
- **Shift press**: `fullRedraw()` — key labels change, full screen needed
- **ROT press**: wipe white + `fullRedraw()`
- **Rotated mode**: `inputRedraw()` falls back to full redraw (input at panel bottom)

### Calling keyboard()

```cpp
char buf[128] = {};
// Default (QWERTY_NO_SPACE):
display.keyboard(buf, sizeof(buf), "Passphrase:");
// Explicit mode:
display.keyboard(buf, sizeof(buf), "Search:", Display::KeyboardMode::QWERTY);
display.keyboard(buf, sizeof(buf), "PIN:",    Display::KeyboardMode::NUMPAD);
```

### Session 2026-08-07 — DU waveform foundation

`_renderLine` in `showMessage` uses DU instead of epd_clear_area_cycles.
See git log for details. Key insight: do NOT apply `reorder_line_buffer()` to non-uniform
pixel data — uniform clear data (0xAA/0x55) makes the reorder a no-op so it was never
caught. Natural byte order is what the ESP32-S3 I2S expects.

### Public epdiy primitives (include ed047tc1.h)

- `epd_start_frame()` / `epd_end_frame()`
- `epd_output_row(uint32_t time_dus)` — latch prev row, CKV, DMA current buf, switch buf
- `epd_skip()` — CKV advance only (leaves source at last latched value — see bleed note above)
- `epd_get_current_buffer()` — 240-byte writable 2bpp row buffer
- `epd_switch_buffer()` — blocks if switched-to buffer is in DMA use

### Next up (in priority order)

1. **Hook keyboard() into MFA_PHRASE state in main.cpp**
   - Replace `softKeyboard()` call with `display.keyboard(buf, maxLen, "Passphrase:")`
   - Use default mode (QWERTY_NO_SPACE) since passphrases don't need space
   - Or QWERTY if the existing vault data uses space in passphrases
   - Located in the DERIVING state entry; look for `softKeyboard` in main.cpp

2. **Portrait search UI** (user is designing HTML mockup)
   - Display is 960×540 landscape; portrait = 540 wide × 960 tall virtual space
   - Requires 90° framebuffer rotation (NOT 180° — different transform from _fb180Rotate)
   - 90° CW rotation: pixel(x,y) in portrait → pixel(EPD_HEIGHT-1-y, x) in landscape
   - Small keyboard: numbers + lowercase letters only, no shift, DEL + OK
   - Wait for user's HTML layout before implementing

3. **Steps.txt** — update completed steps after MFA_PHRASE hookup

### Font: MonoFont (Consolas 28pt @ 150 DPI)

`_charWidth` printed at boot: `[EPD47] MonoFont line_height=XX ascender=XX charWidth=XX`
`kAbove`/`kBelow` in `showMessage` (currently 44/16) may need tuning.

### Dead code in display.cpp

`_compactFromFb()`, `_pushRect()` — unused, kept. `_pushRect` uses `epd_clear_area_cycles`
which bleeds ~5px; don't revive for narrow rects.
