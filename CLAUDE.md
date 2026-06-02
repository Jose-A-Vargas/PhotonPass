# PhotonPass — Claude Code Context

## What this project is

An open-source, fully air-gapped hardware password manager running on the Seeed Studio XIAO ESP32S3 Plus. The device:
- Stores credentials in AES-256-GCM encrypted `vault.bin` on internal flash (LittleFS)
- Unlocks via two physical factors: a scanned QR token + a typed passphrase (PBKDF2 key derivation)
- Delivers passwords via USB HID keyboard emulation or E-Ink QR display
- Has Wi-Fi and Bluetooth permanently disabled in firmware

## Development status

See `steps.txt` for the full phase-by-phase roadmap with completion checkboxes.
Update the "Session Notes" section at the bottom of `steps.txt` at the end of each session.

**Completed:** Steps 1–10 (all Phase 1 firmware + Phase 2 sync & backup)
**Next:** Step 11 — Electron/Node.js companion desktop app (`companion/`)

## Repository layout

```
include/config.h       pins, crypto constants, vault limits — single source of truth
src/crypto.h/.cpp      PBKDF2-HMAC-SHA256 + AES-256-GCM via mbedTLS, secureClear()
src/vault.h/.cpp       VaultRecord/VaultState structs, binary serializer, LittleFS I/O
src/passgen.h/.cpp     password generator + phonotactic passphrase engine
src/scanner.h/.cpp     GM60 UART driver + physical scan button debounce
src/display.h/.cpp     GDEY042T81-T02 E-Ink + GT911 touch driver + soft keyboard
src/hid.h/.cpp         USB HID keyboard output wrapper
src/sync.h/.cpp        device-to-device QR frame sync (PPSS: protocol)
src/backup.h/.cpp      encrypted DMZ backup export / import
src/main.cpp           18-state UI state machine, session management
partitions.csv         custom 8 MB flash partition layout
platformio.ini         espressif32, Arduino framework, PSRAM, LittleFS FS, GxEPD2, QRCode
steps.txt              full development roadmap
```

## Hardware target

- **Board:** `seeed_xiao_esp32s3` (Plus variant — 8 MB OPI PSRAM)
- **Framework:** Arduino via PlatformIO
- **Key peripherals:**
  - E-Ink display: SPI (D8 SCK, D10 MOSI, D1 CS, D2 DC, D0 RST, D3 BUSY)
  - Touch digitizer GT911: I2C (D4 SDA, D5 SCL, D9 INT)
  - GM60 scanner: Serial1 UART (D6 TX, D7 RX, 9600 baud)
  - Scan button: GPIO 12 (active-low, wired in parallel with GM60 trigger)
- D9 (MISO) is repurposed as touch INT — safe because E-Ink is write-only SPI

## UI state machine (src/main.cpp)

18 states — `enterState()` + `gStateEntered` flag drives entry-action pattern:

| State | Description |
|---|---|
| `LOCKED` | Vault locked, tap to begin unlock |
| `MFA_SCAN` | Block on scan button press, poll up to 3 s for TOKEN frame |
| `MFA_PHRASE` | Blocking `softKeyboard()` for passphrase |
| `DERIVING` | PBKDF2 + vault decrypt; first boot creates empty vault |
| `UNLOCKED` | Idle; bottom row: [SEARCH] · [BACKUP] · [SYNC] |
| `QUERYING` | Companion query matched; brief domain/user preview |
| `OUTPUT_SEL` | Choose: Keyboard or QR Code (session timeout enforced) |
| `OUTPUT_HID` | Type via USB HID; wipes `gMatchedRecord` after delivery |
| `OUTPUT_QR` | Show password QR; auto-clears after `QR_DISPLAY_TIMEOUT_MS` (30 s) |
| `LOCKED_OUT` | `MAX_UNLOCK_ATTEMPTS` exceeded; reboot required |
| `SYNC_MENU` | Choose Send or Receive |
| `SYNC_PEER_SEL` | Pick PRESHARE_KEY peer with arrow navigation |
| `SYNC_OUTBOUND` | Stream QR frames; tap to advance |
| `SYNC_INBOUND` | Auto-accumulate PPSS: frames; not button-gated |
| `SYNC_RESULT` | Show outcome, return to UNLOCKED |
| `SEARCH_INPUT` | Blocking soft-keyboard query entry |
| `SEARCH_RESULTS` | 4-per-page results list; tap → OUTPUT_SEL |
| `BACKUP_MENU` | Choose Export or Import |
| `BACKUP_EXPORT` | One-shot: encrypt vault → DMZ `/backup.bin` |
| `BACKUP_IMPORT` | One-shot: read DMZ, decrypt, merge records |

UNLOCKED bottom-button layout (400 px wide):
- `[SEARCH]` x = 13–109
- `[BACKUP]` x = 152–248
- `[SYNC]`   x = 291–387

## Key design decisions

### Flash partition layout (`partitions.csv`)
```
nvs       20 KB   NVS storage (SDK requirement)
otadata    8 KB   OTA slot selector
app0       2 MB   firmware
spiffs   512 KB   vault LittleFS  — label "spiffs", mounted by default LittleFS.begin()
dmz        5 MB   backup LittleFS — label "dmz",   mounted only during backup operations
Total  7.56 MB / 8 MB
```
The DMZ uses a second independent `LITTLEFSFS` object in `backup.cpp` (`static LITTLEFSFS sDmzFS`).
`dmzMount()` / `dmzUnmount()` bracket every export/import operation.

### Vault binary format (`vault.bin`)
- 72-byte unencrypted header: magic `PPV1`, version, record count, 32-byte PBKDF2 salt,
  12-byte GCM nonce, 16-byte GCM auth tag, 4-byte payload length
- Encrypted payload: packed records, big-endian length-prefixed binary fields
- GCM AAD = first `offsetof(VaultHeader, tag)` bytes (magic, version, salt, nonce all authenticated)
- Nonce is regenerated fresh on every `saveVault()` call

### Key derivation
- PBKDF2 full salt = `device_salt (32 bytes) || scanned_QR_token (N bytes)`
- Both factors (possession + knowledge) contribute to the derived key
- Device salt is generated once at vault creation, stored in the header, never changes

### Sync protocol (PPSS: frames)
- Frame: `PPSS:<sess8hex>:<idx3dec>/<tot3dec>:<hexbytes>` — all uppercase, QR alphanumeric mode
- Payload: `nonce(12) || GCM-tag(16) || AES-256-GCM ciphertext` — no AAD (unlike vault.bin)
- `PRESHARE_KEY` records: `password` field = 64-char uppercase hex (32-byte raw AES key)
- `SyncReceiver` is a 20 KB global to avoid stack overflow; tracks per-frame lengths for last-frame padding
- Inbound: tries every `PRESHARE_KEY` record until GCM auth succeeds (no explicit peer selection)
- Merge policy: duplicate UUID → keep record with newer `lastChanged`

### Backup file format (`backup.bin` on DMZ)
- `BackupHeader` (56 bytes): magic `PPBK`, version, recordCount, `keyUuid[16]`, nonce
- AAD = first 36 bytes (magic through nonce); GCM tag at offset 36
- `BACKUP_KEY` records: same 64-char hex convention as `PRESHARE_KEY`
- Import locates the key by `keyUuid` stored in the header — no brute-force trial
- Export auto-generates a `BACKUP_KEY` record (and saves vault) if none exists

### Physical scan button
- `PIN_SCAN_BTN` = GPIO 12 (active-low, `INPUT_PULLUP`, 30 ms debounce)
- Wired in parallel with the GM60 trigger pin — one press fires both MCU and scanner hardware
- `MFA_SCAN` and `UNLOCKED` block on `scanBtnPressed()` before polling up to `SCAN_BTN_TIMEOUT_MS` (3 s)
- `SYNC_INBOUND` is intentionally **not** button-gated (auto-accumulates multi-frame streams)

### Password QR output security
- QR spec minimum is `ECC_LOW` (7% recovery) — no lower level exists
- Mitigated by: `showQR()` uses `maxSize = 150` (~1 mm/module), unresolvable by cameras beyond ~50 cm
- `QR_DISPLAY_TIMEOUT_MS = 30 000` — auto-dismiss limits camera capture window
- `qrBuf` stack buffer is `secureClear()`ed on **all** return paths in `display.cpp::showQR()`

### Vault search
- `SEARCH_INPUT` / `SEARCH_RESULTS` states; `gSearchResults[VAULT_MAX_RECORDS]` index array
- `containsCI()` — case-insensitive substring match, no heap
- `doSearch()` matches on domain, username, queryValue, and type name (`recordTypeName()`)
- 4 results per page; tapping a result deep-copies into `gMatchedRecord` → `OUTPUT_SEL`

### Security invariants to maintain
- `VaultState.masterKey` must **never** be written to flash or logged
- All decrypted record data lives only in `VaultState` — never cached elsewhere
- `VaultState::lock()` must be called on timeout, failed auth, and before any reset
- Use `secureClear()` (wraps `mbedtls_platform_zeroize`) everywhere sensitive buffers are wiped
- `decryptGCM()` returns `ERR_AUTH` without exposing any partial plaintext on tag mismatch
- `gMatchedRecord.password` is `secureClear()`ed immediately after HID output or QR dismiss
- PRESHARE_KEY and BACKUP_KEY hex passwords are decoded to raw bytes, used, then `secureClear()`ed

### Coding conventions
- Module headers go in `src/` alongside their `.cpp`; only shared constants go in `include/`
- Enums use `enum class` with explicit underlying types
- Error returns use typed enums (`CryptoResult`, `HidResult`) — no raw int error codes
  (exception: `backupImport()` returns `int` with defined negative sentinel values)
- No dynamic allocation in interrupt context; `std::vector` / `std::string` acceptable in
  `loop()` context given 8 MB PSRAM on the Plus variant
- No comments explaining what code does — only comments explaining non-obvious WHY

## mbedTLS availability

mbedTLS is bundled with ESP-IDF (via PlatformIO's espressif32 platform). No `lib_deps` entry needed.
```cpp
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
```

## Companion app (next: Step 11)

- Desktop: Electron.js/Node.js — generates query QR codes, manages backup files
- Browser extension: Chrome/Firefox/Safari/Edge — extracts domain from active tab
- Query format the device expects: `PPQ::<queryValue>` (pipe-delimited for multi-account)
- Companion reads/writes DMZ `backup.bin` for vault backup management
- Lives in `companion/` and `extension/` directories (to be created)
