# PhotonPass

An open-source, fully air-gapped, physical hardware password manager and credential vault built on the Seeed Studio XIAO ESP32S3 Plus.

---

## Overview

PhotonPass is a standalone hardware device that stores, generates, and delivers passwords with zero wireless attack surface. Wi-Fi and Bluetooth are permanently disabled in firmware. Credentials never touch a PC's clipboard or operating system — they are either typed directly via USB HID keyboard emulation or displayed as a QR code on the built-in E-Ink touchscreen.

Unlocking the vault requires two physical factors:

1. **Possession** — scan a high-entropy static token (a QR code kept physically separate) via the onboard barcode reader
2. **Knowledge** — type a passphrase on the E-Ink touchscreen

These are combined via PBKDF2-HMAC-SHA256 (10,000 iterations) to derive a 256-bit AES master key that exists only in volatile RAM. The key and all decrypted data are wiped from memory on lock, timeout, or power loss.

---

## Hardware

| Component | Part |
|---|---|
| Microcontroller | Seeed Studio XIAO ESP32S3 Plus |
| Display | Goodisplay GDEY042T81-T02 (4.2" E-Ink touchscreen, 400×300) |
| Scanner | Elecbee GM60 1D/2D barcode/QR reader (TTL UART) |
| Power | LiPo battery via XIAO onboard charging circuit |
| Enclosure | Custom CNC-machined aluminum/brass shell (planned) |

### Pin Assignment

| Signal | Pin | Notes |
|---|---|---|
| E-Ink RST | D0 | |
| E-Ink CS | D1 | |
| E-Ink DC | D2 | |
| E-Ink BUSY | D3 | |
| Touch SDA | D4 | GT911 I2C |
| Touch SCL | D5 | GT911 I2C |
| Scanner TX | D6 | UART1 to GM60 RX |
| Scanner RX | D7 | UART1 from GM60 TX |
| SPI SCK | D8 | Shared E-Ink |
| Touch INT | D9 | Repurposed MISO — safe, E-Ink is write-only |
| SPI MOSI | D10 | Shared E-Ink |
| Scan button | GPIO 12 | Active-low, wired in parallel with GM60 trigger |

---

## Security Design

- **Zero wireless** — `WiFi.mode(WIFI_OFF)` and `btStop()` called at boot, before any user interaction
- **No persistent plaintext** — master key and decrypted vault records live only in RAM (`VaultState`); `VaultState::lock()` uses `mbedtls_platform_zeroize` to wipe on lock, timeout, or power loss
- **AES-256-GCM** — authenticated encryption; any tampering with `vault.bin` is detected at decrypt time
- **GCM AAD** — the vault file header (magic, version, salt, nonce) is authenticated alongside the ciphertext, preventing header substitution attacks
- **PBKDF2 salt composition** — device salt concatenated with the scanned QR token, so both physical factors genuinely contribute to the derived key
- **Session timeout** — vault auto-locks after 120 seconds of inactivity
- **Unlock attempt cap** — device enters permanent lockout after 5 failed unlock attempts (reboot required)
- **Password QR security** — displayed at minimum QR module size (~1 mm), unresolvable by cameras beyond ~50 cm; auto-clears after 30 seconds
- **Scan button** — a single active-low button wired in parallel with the GM60 trigger fires both the MCU state machine and the scanner hardware simultaneously

---

## Vault Record Types

| Type | Description |
|---|---|
| `WEB_SERVICE` | Website login (10-entry password history) |
| `PC_ACCOUNT` | Local machine account (40-entry password history) |
| `API_TOKEN` | API key or token |
| `ENCRYPT_KEY` | Symmetric encryption key |
| `PRESHARE_KEY` | Pre-shared key for device-to-device sync |
| `BACKUP_KEY` | Key for encrypted vault backup |

Each record stores: UUID (RFC 4122 v4), type, domain, username, password, query value, last-changed timestamp, and password history.

---

## Password Generation

The onboard generator uses the ESP32S3 hardware TRNG (`esp_random()`) with rejection sampling to avoid modulo bias:

| Profile | Character set |
|---|---|
| Full ASCII | `0x20`–`0x7E` (all printable) |
| Standard | Alphanumeric + `!@#$%^&*` |
| Hexadecimal | `0–9`, `A–F` |
| Base64 | `A–Z`, `a–z`, `0–9`, `+`, `/` |

A phonotactic pseudo-word passphrase generator produces pronounceable words from static Onset/Vowel/Coda arrays — over 500,000 unique single-word combinations, no dictionary file required, no heap allocation.

---

## Sync & Backup

### Device-to-device sync
Encrypted QR code streams using per-peer pre-shared 256-bit keys stored as `PRESHARE_KEY` records (up to 50 peers). Frame format: `PPSS:<session>:<index>/<total>:<hexbytes>` — all uppercase, fits QR alphanumeric mode at v10 ECC_L.

### Encrypted backup
A `BACKUP_KEY` record (auto-generated on first export) holds a 256-bit key stored as 64-char hex. Export writes an AES-256-GCM encrypted `backup.bin` to the dedicated 5 MB DMZ flash partition. Import reads and decrypts using the key UUID stored in the backup header — no brute-force trial needed. The DMZ partition is mounted only during the export/import operation.

### Merge policy
On both sync and import, duplicate record UUIDs resolve by keeping the record with the newer `lastChanged` timestamp.

---

## Repository Structure

```
PhotonPass/
├── include/
│   └── config.h           # pins, crypto constants, vault limits
├── src/
│   ├── main.cpp            # 18-state UI state machine, session management
│   ├── crypto.h/.cpp       # PBKDF2-HMAC-SHA256, AES-256-GCM, secureClear()
│   ├── vault.h/.cpp        # record schema, binary serializer, LittleFS I/O
│   ├── passgen.h/.cpp      # password + passphrase generation
│   ├── scanner.h/.cpp      # GM60 UART driver + scan button debounce
│   ├── display.h/.cpp      # E-Ink + touch driver + soft keyboard
│   ├── hid.h/.cpp          # USB HID keyboard output
│   ├── sync.h/.cpp         # device-to-device QR frame sync
│   └── backup.h/.cpp       # encrypted DMZ backup export / import
├── companion/              # Electron.js desktop app (Step 11 — planned)
├── extension/              # Browser extension (Step 12 — planned)
├── partitions.csv          # custom 8 MB flash partition layout
├── platformio.ini
├── steps.txt               # development roadmap with session notes
└── LICENSE                 # GPLv3
```

### Flash Partition Layout

| Partition | Size | Purpose |
|---|---|---|
| `nvs` | 20 KB | ESP-IDF non-volatile storage |
| `otadata` | 8 KB | OTA slot selector |
| `app0` | 2 MB | Firmware |
| `spiffs` | 512 KB | Vault LittleFS (`vault.bin`) |
| `dmz` | 5 MB | Backup LittleFS (`backup.bin`) |

---

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
# Build firmware
pio run

# Flash firmware
pio run --target upload

# Flash vault filesystem image (spiffs partition)
pio run --target uploadfs

# Serial monitor
pio device monitor
```

The vault filesystem image (built from the `data/` directory if present) is uploaded to the `spiffs` partition. The `dmz` partition is managed entirely at runtime by the firmware — do not flash it separately.

---

## Companion App

A desktop companion app (Electron.js/Node.js) and browser extension (Chrome/Firefox/Safari/Edge) are in development (Steps 11–12). The companion app will:
- Generate `PPQ::<queryValue>` query QR codes for credential lookup
- Support multi-account batch queries (pipe-delimited)
- Read and write the DMZ `backup.bin` for vault backup management

No credentials are ever stored on the host machine.

---

## License

- Firmware and software: **GNU General Public License v3.0** — see `LICENSE`
- Hardware CAD/STEP enclosure files: **CC BY-SA 4.0**
