I am building an open-source, fully air-gapped, physical hardware password manager and credential vault. I need you to act as an expert embedded systems engineer and full-stack software architect to help me develop the codebase. 

Below is the architectural breakdown, hardware stack, software workflow, and core design requirements for the project.

---

### 🛠️ 1. The Hardware Stack
* **Microcontroller:** Seeed Studio XIAO ESP32S3 Plus (Native USB support).
* **Display:** Goodisplay 4.2-inch E-Ink Touchscreen (GDEY042T81-T02) driven via an SPI adapter board; touchscreen digitizer operates over I2C.
* **Optical Input:** Elecbee GM60 1D/2D Barcode/QR Code Reader Module (communicating via TTL UART).
* **Power:** Lithium-polymer battery utilizing the XIAO's built-in charging management circuit. 
* **Enclosure:** Enclosed in a heavy, custom CNC-machined metal shell (aluminum/brass) acting as a physical shield.

### 🔒 2. Security Protocol & "Zero-Wireless" Constraints
* **Absolute Air-Gap:** Wi-Fi and Bluetooth radios are permanently software-disabled (`WiFi.mode(WIFI_OFF); btStop();`) in the firmware. 
* **No Persistent Master Keys:** The master decryption key and decrypted vault data must *never* be committed to non-volatile flash storage. They must stay strictly in volatile RAM, governed by an idle timeout window that securely wipes (`memset`) memory.
* **Multi-Factor Unlock Sequence (No PC Interaction):**
    1.  *Possession:* The user scans a high-entropy static token (printed or kept physically separate) via the GM60 barcode scanner. This acts as a cryptographic salt.
    2.  *Knowledge:* The user types a memorable passphrase directly onto the device's E-Ink touchscreen.
    3.  *Derivation:* The firmware uses `mbedTLS` to run PBKDF2 (HMAC-SHA256, 10,000+ iterations) combining the passphrase and token to derive a 256-bit AES Master Key.

### 📂 3. Storage & Data Exchange Mechanics
* **On-Chip Storage:** The encrypted vault payload lives locally on the ESP32S3's internal flash partition using `LittleFS` (saved as a binary file, e.g., `vault.bin`).
* **Querying Records:** The companion desktop app on the PC displays a 2D barcode containing an account query (e.g., `PhotonPass::https://github.com/|PhotoPass::https://accounts.google.com/`). The user scans this screen with the GM60. The hardware manager looks up the matching identifier, decrypts the record using the RAM-resident AES key, and fetches the password.
* **Dual-Mode User Choice Output:**
    * *USB HID Keyboard:* The device behaves as a native USB keyboard using the `USBHIDKeyboard` library, securely typing the password string directly into the cursor target on the host PC (bypassing the OS clipboard entirely).
    * *Optical Display (Fallback):* The device renders a QR code of the password on the 4.2-inch E-Ink display to allow physical scanning into isolated, external devices.
* **Sync Strategies (Open to expansion):** Data synchronization from the companion app happens either visually via optical sequential QR code streams scanned by the GM60, or through a temporary, user-triggered restricted USB serial/mass storage mount sequence.

### 🔣 4. Password Generation Engine
* **Entropy Source:** Employs the ESP32S3's internal Hardware True Random Number Generator (`esp_random()`), which stays dynamically seeded by physical hardware noise registers despite radios being off.
* **Character Set Character Mask Profiles:** Must generate custom lengths matching individual platform rules:
    * *Full ASCII:* Every readable glyph from space (`0x20`) to tilde (`0x7E`).
    * *Standard Safe:* Dynamic alphanumeric plus common safe special symbols (`!@#$%^&*`).
    * *Hexadecimal:* Strict Base16 hex values (`0-9`, `A-F`) for token keys.
    * *Base64:* Standard web-safe string encoding limits (`A-Z`, `a-z`, `0-9`, `+`, `/`).
* **Psuedo words passphrase Generator:** 
* **Pseudo-words Passphrase Generator:**
Employs a dynamic phonotactic template engine utilizing static arrays of valid English Onsets, Vowels, and Codas.
Every generation cycle randomly switches between distinct syllable architectures ranging from short 3-letter CVC strings (e.g., *Bop*) to grandiose 3-syllable compounds (e.g., *Blis-tray-cho-gump*)—to maximize structural randomness.
Designed to achieve a single-word pool lower bound exceeding 500,000 combinations without relying on memory-heavy dictionary files or heap-fragmenting dynamic allocations.

### 📦 5. Development Workspace & Repository Target
* **Development Environment:** VS Code using the **PlatformIO IDE** extension (Arduino framework target, configured for native USB CDC on boot).
* **Open Source Target:** The project repository (Firmware + Desktop Companion App) will be licensed under the **GNU General Public License v3 (GPLv3)** to maintain strict copyleft and anti-tivoization protection, while physical CAD/STEP enclosure assets will use **CC BY-SA 4.0**.
* **Companion App Stack:** To be built primarily using an Electron.js/Node.js desktop app framework and Chrome/Firefox/Safari/Edge extention.

---

### Current Objective:
I have a baseline PlatformIO workspace testing the LittleFS operations and the native USB HID keyboard throughput. 

Please analyze this entire system architecture. Let's begin by designing the structural database schema for `vault.bin` (e.g., JSON vs compressed binary payload) and writing the helper functions for the `mbedTLS` **AES-256-GCM** encryption and decryption engine that maps directly to this architecture.


### Design/Workflow Goals and Features:
  * Passwords can be updated between devices by scanning encrypted qr codes by using preshared encryption keys between devices (mutliple devices can be supported up to 50).
  * Encrypted backups can be exported and imported with merges done with duplicate accounts the newest password is accepted, each backup has it's own unique encryption key that is stored in the vault until user deletes it. these can be stored outside encrypted vault accessible via USB storage with a 5 MB DMZ partition.
  * stored passwords in the vault should be stored with the following data structure to make mananging the vault easier
    -uuid
    -type (web service, pc account, API token, encryption key, preshare key, backup key, etc)
    -domain (gihub.com,google.com,microsoft.com,windows login, linux login, mac login, putty key password, other)
    -username
    -encrypted password
    -last changed
    -historical passwords list {oldPassword, changed} (last 10 most recent passwords, pc accounts last 40) 
    -query value (the value the companion app/web extention)
  * 