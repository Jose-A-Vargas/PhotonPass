// PhotonPass Vault CLI — interactive serial terminal with HID output
// Build:   pio run -e vault_cli
// Flash:   pio run -e vault_cli --target upload
// Monitor: pio device monitor -e vault_cli
//
// Commands:
//   init                   create a new vault
//   unlock                 unlock existing vault (prompts passphrase)
//   lock                   lock vault, wipe master key from RAM
//   save                   force-save vault to flash
//   status                 vault status and record count
//   wipe                   delete vault.bin (no unlock needed)
//   ls / list [page]       list records, 10 per page
//   show <n>               full record details + history
//   add                    add a new record (interactive)
//   del <n>                delete record (with confirmation)
//   rotate <n>             rotate password (saves old to history)
//   type <n>               type password via HID keyboard (3 s countdown)
//   typeu <n>              type username via HID keyboard (3 s countdown)
//   typea <n>              username + Tab + password + Enter (3 s countdown)
//   gen [len] [A|S|H|B]    generate a password without storing it
//   backup                 export encrypted backup to DMZ FAT partition
//   restore                import backup from DMZ into vault
//   usbdrive               expose DMZ as USB drive for 60 s then disconnect
//   date                   show/correct stored date (y / +days / YYYY-MM-DD)
//   help / ?               show this menu

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <USBMSC.h>
#include <FFat.h>
#include <Preferences.h>
#include <stdarg.h>

#include "config.h"
#include "crypto.h"
#include "vault.h"
#include "passgen.h"
#include "hid.h"
#include "backup.h"

// ============================================================
// USB Mass Storage — synthetic FAT12 image in PSRAM
//
// The DMZ uses FFat with ESP32 wear-levelling, so raw flash blocks
// look like WL-wrapped data, not plain FAT. Windows can't read that.
// Instead we read backup.bin into PSRAM and build a clean FAT12
// image there. MSC exposes the PSRAM image — Windows sees real FAT12.
// ============================================================

// Image layout (all sectors = 512 bytes):
//   0        boot sector (BPB)
//   1–3      FAT1  (3 sectors covers up to 1023 FAT12 clusters)
//   4–6      FAT2  (copy)
//   7–9      root directory (3 sectors = 48 entries — supports LFN)
//   10–1023  data clusters (file data)
#define FAT_SECTOR_SIZE     512
#define FAT_IMAGE_SECTORS   1024          // 512 KB image in PSRAM
#define FAT_ROOT_SECTOR     7             // first root directory sector
#define FAT_ROOT_SECTORS    3             // 3 sectors × 16 entries = 48 entries
#define FAT_ROOT_ENTRIES    (FAT_ROOT_SECTORS * FAT_SECTOR_SIZE / 32)
#define FAT_DATA_START      (FAT_ROOT_SECTOR + FAT_ROOT_SECTORS)  // = 10

static USBMSC  sMSC;
static uint8_t* sFatImage = nullptr;     // PSRAM FAT12 image, built on demand

// ---- FAT12 helpers ----

static void fatW16(uint8_t* b, uint16_t v) { b[0] = v & 0xFF; b[1] = v >> 8; }
static void fatW32(uint8_t* b, uint32_t v) {
    b[0] = v & 0xFF; b[1] = (v>>8)&0xFF; b[2] = (v>>16)&0xFF; b[3] = v>>24;
}

static void fat12Set(uint8_t* fat, uint16_t cluster, uint16_t val) {
    uint32_t off = cluster * 3 / 2;
    if (cluster & 1) {
        fat[off]     = (fat[off] & 0x0F) | (uint8_t)(val << 4);
        fat[off + 1] = (uint8_t)(val >> 4);
    } else {
        fat[off]     = (uint8_t)(val & 0xFF);
        fat[off + 1] = (fat[off + 1] & 0xF0) | (uint8_t)(val >> 8);
    }
}

// ---- FAT12 multi-file entry (supports LFN via nameLfn) ----

struct FatEntry {
    char     name83[12];    // synthetic 8.3 alias (space-padded, 11 chars + null)
    char     nameLfn[32];   // full long filename for VFAT LFN entries
    const uint8_t* data;
    uint32_t size;
};

// Synthetic 8.3 alias: "BKPnnnnn.BKP" (idx is 0-based)
static void to83Name(int idx, char out[12]) {
    memset(out, ' ', 11); out[11] = '\0';
    char base[9];
    snprintf(base, sizeof(base), "BKP%05d", idx + 1);
    memcpy(out, base, 8);
    out[8] = 'B'; out[9] = 'K'; out[10] = 'P';
}

// ---- VFAT LFN helpers ----

// Byte offsets of the 13 UTF-16LE chars within a 32-byte LFN directory entry
static const int kLfnCharOff[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};

static uint8_t lfnChecksum(const char name83[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) ? 0x80u : 0u) + (s >> 1) + (uint8_t)name83[i]);
    return s;
}

// Write VFAT LFN entries into de[] for longName, using name83 for the checksum.
// LFN entries are written highest-sequence-first (as required by VFAT spec).
// Returns the number of 32-byte entries written (= ceil((len+13)/13) chunks).
static int writeLfnEntries(uint8_t* de, const char* longName, const char name83[11]) {
    int     len       = (int)strlen(longName);
    int     numChunks = (len + 13) / 13;
    uint8_t chk       = lfnChecksum(name83);

    for (int chunk = numChunks - 1; chunk >= 0; chunk--) {
        int     slot  = numChunks - 1 - chunk;   // directory slot (0 = first written)
        uint8_t seq   = (uint8_t)(chunk + 1);    // 1-based
        if (chunk == numChunks - 1) seq |= 0x40; // "last chunk" marker on highest seq

        uint8_t* entry = de + slot * 32;
        memset(entry, 0xFF, 32);
        entry[0]  = seq;
        entry[11] = 0x0F; // LFN attribute
        entry[12] = 0;
        entry[13] = chk;
        entry[26] = 0; entry[27] = 0; // cluster must be 0

        for (int ci = 0; ci < 13; ci++) {
            int charIdx = chunk * 13 + ci;
            uint16_t uc;
            if      (charIdx <  len) uc = (uint8_t)longName[charIdx];
            else if (charIdx == len) uc = 0x0000;
            else                     uc = 0xFFFF;
            entry[kLfnCharOff[ci]]     = (uint8_t)(uc & 0xFF);
            entry[kLfnCharOff[ci] + 1] = (uint8_t)(uc >> 8);
        }
    }
    return numChunks;
}

static bool buildFat12Image(const FatEntry* files, int count) {
    sFatImage = (uint8_t*)ps_malloc((size_t)FAT_IMAGE_SECTORS * FAT_SECTOR_SIZE);
    if (!sFatImage) return false;
    memset(sFatImage, 0, (size_t)FAT_IMAGE_SECTORS * FAT_SECTOR_SIZE);

    // Boot sector
    uint8_t* b = sFatImage;
    b[0]=0xEB; b[1]=0x3C; b[2]=0x90;
    memcpy(b+3, "PHOTONPS", 8);
    fatW16(b+11, FAT_SECTOR_SIZE); b[13]=1;
    fatW16(b+14, 1); b[16]=2;
    fatW16(b+17, FAT_ROOT_ENTRIES); fatW16(b+19, FAT_IMAGE_SECTORS);
    b[21]=0xF8; fatW16(b+22, 3);
    fatW16(b+24, 63); fatW16(b+26, 255);
    b[38]=0x29; fatW32(b+39, 0x20260603);
    memcpy(b+43, "PHOTON PASS ", 11); memcpy(b+54, "FAT12   ", 8);
    b[510]=0x55; b[511]=0xAA;

    // FAT1 (sector 1)
    uint8_t* fat1 = sFatImage + FAT_SECTOR_SIZE;
    fat1[0]=0xF8; fat1[1]=0xFF; fat1[2]=0xFF;

    // Root dir
    uint8_t* root = sFatImage + FAT_ROOT_SECTOR * FAT_SECTOR_SIZE;

    uint16_t nextCluster = 2;
    int      dirEntry    = 0;

    for (int fi = 0; fi < count; fi++) {
        if (!files[fi].data || files[fi].size == 0) continue;

        // Each file needs LFN entries + 1 8.3 entry; check capacity
        int lfnNeeded = files[fi].nameLfn[0] ? (int)((strlen(files[fi].nameLfn) + 13) / 13) : 0;
        if (dirEntry + lfnNeeded + 1 > FAT_ROOT_ENTRIES) break;

        uint32_t clusters     = (files[fi].size + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
        uint16_t startCluster = nextCluster;

        for (uint32_t i = 0; i < clusters; i++) {
            uint16_t next = (i == clusters - 1) ? 0xFFF : (uint16_t)(nextCluster + 1);
            fat12Set(fat1, nextCluster, next);
            nextCluster++;
        }

        // Write LFN entries (if long name present)
        if (lfnNeeded > 0) {
            dirEntry += writeLfnEntries(root + dirEntry * 32,
                                        files[fi].nameLfn, files[fi].name83);
        }

        // Write 8.3 directory entry
        uint8_t* de = root + dirEntry * 32;
        memcpy(de, files[fi].name83, 11);
        de[11] = 0x20;
        fatW16(de+22, 0x0000); fatW16(de+24, 0x5CC3);
        fatW16(de+26, startCluster);
        fatW32(de+28, files[fi].size);

        uint8_t* dst = sFatImage + (FAT_DATA_START + (startCluster - 2)) * FAT_SECTOR_SIZE;
        uint32_t avail = (uint32_t)(FAT_IMAGE_SECTORS - FAT_DATA_START - (startCluster - 2))
                         * FAT_SECTOR_SIZE;
        uint32_t copy = files[fi].size < avail ? files[fi].size : avail;
        memcpy(dst, files[fi].data, copy);

        dirEntry++;
    }

    // FAT2 = copy of FAT1
    memcpy(sFatImage + 4 * FAT_SECTOR_SIZE, fat1, 3 * FAT_SECTOR_SIZE);
    return true;
}

// ---- MSC callbacks ----

static int32_t mscRead(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
    if (!sFatImage) return -1;
    uint32_t addr = lba * FAT_SECTOR_SIZE + offset;
    if (addr + bufsize > (uint32_t)FAT_IMAGE_SECTORS * FAT_SECTOR_SIZE) return -1;
    memcpy(buf, sFatImage + addr, bufsize);
    return (int32_t)bufsize;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    if (sFatImage) {
        uint32_t addr = lba * FAT_SECTOR_SIZE + offset;
        if (addr + bufsize <= (uint32_t)FAT_IMAGE_SECTORS * FAT_SECTOR_SIZE)
            memcpy(sFatImage + addr, buf, bufsize);
    }
    return (int32_t)bufsize;
}

static bool mscStartStop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition; (void)start; (void)load_eject;
    return true;
}

// ---- Backup file listing cache (populated before each USB session) ----
#define BACKUP_MAX_FILES 20
static char gBkpList[BACKUP_MAX_FILES][BACKUP_FILENAME_MAX];
static int  gBkpCount = 0;

static void bkpListCb(int idx, const char* name) {
    if (idx < BACKUP_MAX_FILES) {
        strncpy(gBkpList[idx], name, BACKUP_FILENAME_MAX - 1);
        gBkpList[idx][BACKUP_FILENAME_MAX - 1] = '\0';
        gBkpCount = idx + 1;
    }
}

// Read a FAT12 cluster-chain entry
static uint16_t fat12Get(const uint8_t* fat, uint16_t cluster) {
    uint32_t off = cluster * 3 / 2;
    if (cluster & 1)
        return ((fat[off] >> 4) | ((uint16_t)fat[off + 1] << 4)) & 0x0FFF;
    else
        return (fat[off]       | ((uint16_t)fat[off + 1] << 8)) & 0x0FFF;
}

// After a USB drive session, parse the PSRAM FAT12 image and write any .BKP
// files that Windows copied onto the drive back to the DMZ FAT partition.
// Supports VFAT LFN so long filenames survive the round-trip through Windows.
static void syncPsramToFlash() {
    if (!sFatImage) return;

    const uint8_t* fat  = sFatImage + 1 * FAT_SECTOR_SIZE;
    const uint8_t* root = sFatImage + FAT_ROOT_SECTOR * FAT_SECTOR_SIZE;

    int  imported = 0;
    char lfnBuf[256] = {};   // assembled LFN name; cleared between files
    bool hasLfn = false;

    for (int i = 0; i < FAT_ROOT_ENTRIES; i++) {
        const uint8_t* de = root + i * 32;
        if (de[0] == 0x00) break;    // end of directory

        if (de[0] == 0xE5) {         // deleted entry
            memset(lfnBuf, 0, sizeof(lfnBuf)); hasLfn = false;
            continue;
        }

        uint8_t attr = de[11];

        if (attr == 0x0F) {
            // VFAT LFN entry — accumulate chars into lfnBuf by chunk position
            uint8_t seq = de[0] & 0x3F;
            if (seq == 0 || seq > 20) {
                memset(lfnBuf, 0, sizeof(lfnBuf)); hasLfn = false;
                continue;
            }
            int base = (seq - 1) * 13;
            for (int ci = 0; ci < 13; ci++) {
                int pos = base + ci;
                if (pos >= (int)sizeof(lfnBuf) - 1) break;
                uint16_t uc = de[kLfnCharOff[ci]] | ((uint16_t)de[kLfnCharOff[ci]+1] << 8);
                if (uc == 0x0000 || uc == 0xFFFF) break;
                lfnBuf[pos] = (char)(uc & 0x7F);
            }
            hasLfn = true;
            continue;
        }

        if (attr & 0x18) {           // volume label or subdirectory
            memset(lfnBuf, 0, sizeof(lfnBuf)); hasLfn = false;
            continue;
        }

        // Regular file entry — resolve filename from LFN or 8.3
        char fname[256];
        if (hasLfn && lfnBuf[0] != '\0') {
            strncpy(fname, lfnBuf, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
        } else {
            if (de[8] != 'B' || de[9] != 'K' || de[10] != 'P') {
                memset(lfnBuf, 0, sizeof(lfnBuf)); hasLfn = false;
                continue;
            }
            int ni = 0;
            for (int j = 0; j < 8 && de[j] != ' '; j++) fname[ni++] = (char)de[j];
            fname[ni++] = '.';
            fname[ni++] = 'B'; fname[ni++] = 'K'; fname[ni++] = 'P';
            fname[ni]   = '\0';
        }
        memset(lfnBuf, 0, sizeof(lfnBuf)); hasLfn = false;

        // Must end in .BKP
        size_t flen = strlen(fname);
        if (flen < 5 || strcasecmp(fname + flen - 4, ".bkp") != 0) continue;

        // Skip files that existed before the session opened
        bool preExisting = false;
        for (int k = 0; k < gBkpCount; k++)
            if (strcasecmp(gBkpList[k], fname) == 0) { preExisting = true; break; }
        if (preExisting) continue;

        // Read cluster chain from PSRAM image
        uint16_t startCluster = (uint16_t)(de[26] | (de[27] << 8));
        uint32_t fileSize     = (uint32_t)(de[28] | (de[29]<<8) | (de[30]<<16) | (de[31]<<24));

        if (fileSize == 0 || startCluster < 2) continue;

        uint8_t* fileData = (uint8_t*)malloc(fileSize);
        if (!fileData) { Serial.printf("  OOM reading %s\n", fname); continue; }

        uint32_t written = 0;
        uint16_t cluster = startCluster;
        while (cluster >= 2 && cluster < 0xFF8 && written < fileSize) {
            uint32_t dataOff = (uint32_t)(FAT_DATA_START + cluster - 2) * FAT_SECTOR_SIZE;
            uint32_t chunk   = fileSize - written;
            if (chunk > FAT_SECTOR_SIZE) chunk = FAT_SECTOR_SIZE;
            memcpy(fileData + written, sFatImage + dataOff, chunk);
            written += chunk;
            cluster  = fat12Get(fat, cluster);
        }

        // Write to DMZ via FFat
        if (dmzMount()) {
            char path[BACKUP_FILENAME_MAX + 2];
            snprintf(path, sizeof(path), "/%s", fname);
            File f = FFat.open(path, FILE_WRITE);
            if (f) {
                f.write(fileData, fileSize);
                f.close();
                Serial.printf("  Imported from drive: %s (%lu bytes)\n",
                              fname, (unsigned long)fileSize);
                imported++;
            } else {
                Serial.printf("  Failed to write %s to DMZ.\n", fname);
            }
            dmzUnmount();
        }
        free(fileData);
    }

    if (imported == 0)
        Serial.println("  No new files written to drive.");
    else
        Serial.printf("  %d file(s) saved to DMZ. Use 'restore' to merge.\n", imported);
}


// ============================================================
// Vault state
// ============================================================

static VaultState gVault;

// ============================================================
// Utilities
// ============================================================

static const char* typeName(RecordType t) {
    switch (t) {
        case RecordType::WEB_SERVICE:  return "WEB";
        case RecordType::PC_ACCOUNT:   return "PC";
        case RecordType::API_TOKEN:    return "API";
        case RecordType::ENCRYPT_KEY:  return "ENC_KEY";
        case RecordType::PRESHARE_KEY: return "PRESHARE";
        case RecordType::BACKUP_KEY:   return "BACKUP";
        default:                       return "OTHER";
    }
}

// Minimal date formatter for unix timestamps
static void fmtDate(char* out, size_t len, uint32_t ts) {
    if (ts == 0) { snprintf(out, len, "never"); return; }
    uint32_t days = ts / 86400;
    uint32_t y = 1970;
    for (;;) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        uint32_t diy = leap ? 366 : 365;
        if (days < diy) break;
        days -= diy; y++;
    }
    static const uint8_t dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t m = 1, d = 1;
    for (m = 1; m <= 12; m++) {
        uint8_t dm = dim[m - 1];
        if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) dm++;
        if (days < dm) { d = days + 1; break; }
        days -= dm;
    }
    snprintf(out, len, "%04lu-%02lu-%02lu", (unsigned long)y, (unsigned long)m, (unsigned long)d);
}

// Forward declaration — defined later in the Utilities section.
static void promptLine(const char* prompt, char* buf, size_t maxlen, bool masked = false);

// ============================================================
// Date tracking (NVS — survives reboots, no RTC)
// ============================================================

static uint32_t sDateBase      = 0;   // unix midnight of last confirmed date
static uint32_t sDateBaseMills = 0;   // millis() when sDateBase was set

static void loadStoredDate() {
    Preferences prefs;
    prefs.begin("ppass", true);
    sDateBase = prefs.getUInt("datebase", 0);
    prefs.end();
    sDateBaseMills = millis();
}

static void saveStoredDate(uint32_t midnight) {
    Preferences prefs;
    prefs.begin("ppass", false);
    prefs.putUInt("datebase", midnight);
    prefs.end();
}

// Calendar date → unix midnight (UTC)
static uint32_t dateToUnix(uint16_t y, uint8_t m, uint8_t d) {
    uint32_t days = 0;
    for (uint16_t yr = 1970; yr < y; yr++) {
        bool leap = (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0));
        days += leap ? 366 : 365;
    }
    static const uint8_t dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (uint8_t mo = 1; mo < m; mo++) {
        uint8_t dm = dim[mo - 1];
        if (mo == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) dm++;
        days += dm;
    }
    days += (uint32_t)(d - 1);
    return days * 86400UL;
}

// Parse "YYYY-MM-DD" → unix midnight; returns 0 on failure
static uint32_t parseDateStr(const char* s) {
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return 0;
    char tmp[11]; memcpy(tmp, s, 10); tmp[10] = '\0';
    tmp[4] = '\0'; tmp[7] = '\0';
    int y = atoi(tmp), m = atoi(tmp + 5), d = atoi(tmp + 8);
    if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    return dateToUnix((uint16_t)y, (uint8_t)m, (uint8_t)d);
}

// Prompt user to confirm or correct the stored date.
// Called on every serial connect and also available as the 'date' command.
static void cmdDateCheck() {
    char dateBuf[16];

    if (sDateBase == 0) {
        Serial.print("  No date stored. Enter today's date [YYYY-MM-DD, Enter to skip]: ");
        char input[16] = {};
        promptLine("", input, sizeof(input));
        if (input[0] == '\0') { Serial.println("  Date tracking disabled."); return; }
        uint32_t ts = parseDateStr(input);
        if (ts == 0) { Serial.println("  Invalid — skipping."); return; }
        sDateBase      = ts;
        sDateBaseMills = millis();
        saveStoredDate(ts);
        fmtDate(dateBuf, sizeof(dateBuf), ts);
        Serial.printf("  Date set: %s\n", dateBuf);
        return;
    }

    // Compute current date from elapsed time since last confirmation
    uint32_t curTs       = sDateBase + (millis() - sDateBaseMills) / 1000;
    uint32_t curMidnight = (curTs / 86400UL) * 86400UL;
    fmtDate(dateBuf, sizeof(dateBuf), curTs);

    Serial.printf("  Date: %s  [y / +days / YYYY-MM-DD]: ", dateBuf);
    char input[16] = {};
    promptLine("", input, sizeof(input));

    const char* p = input;
    while (*p == ' ') p++;

    if (*p == '\0' || toupper((unsigned char)*p) == 'Y') {
        sDateBaseMills = millis();   // re-anchor so elapsed drift resets
        return;
    }

    uint32_t newMidnight = 0;
    char* ep;
    long offset = strtol(p, &ep, 10);
    if (*ep == '\0' && offset >= 0) {
        newMidnight = curMidnight + (uint32_t)offset * 86400UL;
    } else {
        newMidnight = parseDateStr(p);
    }

    if (newMidnight == 0) {
        Serial.println("  Invalid — date unchanged.");
        sDateBaseMills = millis();
        return;
    }

    sDateBase      = newMidnight;
    sDateBaseMills = millis();
    saveStoredDate(newMidnight);
    fmtDate(dateBuf, sizeof(dateBuf), newMidnight);
    Serial.printf("  Date updated: %s\n", dateBuf);
}

// Current unix-ish timestamp.  Uses calibrated date if set, otherwise falls
// back to a fixed epoch so records sort after old test data.
static uint32_t now() {
    if (sDateBase == 0) return 1720000000UL + millis() / 1000;
    return sDateBase + (millis() - sDateBaseMills) / 1000;
}

// Blocking prompt — waits for a line. masked=true prints '*' for each char.
// Accepts \r, \n, or \r\n as line terminator (works with PuTTY and Arduino IDE).
static void promptLine(const char* prompt, char* buf, size_t maxlen, bool masked) {
    Serial.print(prompt);
    size_t len = 0;
    char lastEnd = 0;
    for (;;) {
        while (!Serial.available()) delay(5);
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            // skip the paired character in \r\n or \n\r
            if ((c == '\n' && lastEnd == '\r') || (c == '\r' && lastEnd == '\n'))
                { lastEnd = 0; continue; }
            lastEnd = c;
            buf[len] = '\0'; Serial.println(); return;
        }
        lastEnd = 0;
        if ((c == 8 || c == 127) && len > 0) {
            len--;
            Serial.print("\b \b");
            continue;
        }
        if (len < maxlen - 1) {
            buf[len++] = c;
            Serial.print(masked ? '*' : c);
        }
    }
}

// Read just the salt from an existing vault file without decrypting.
static bool readVaultSalt(uint8_t* salt, const char* path = VAULT_FILE_PATH) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return false;
    VaultHeader hdr;
    bool ok = (f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr));
    f.close();
    if (ok) memcpy(salt, hdr.salt, PBKDF2_SALT_LEN);
    return ok;
}

// ============================================================
// Vault lifecycle commands
// ============================================================

static void cmdInit() {
    if (gVault.unlocked) {
        Serial.println("  Vault already unlocked. Lock first.");
        return;
    }
    if (LittleFS.exists(VAULT_FILE_PATH)) {
        Serial.println("  vault.bin already exists. Delete it first to re-init.");
        return;
    }

    char pass1[128], pass2[128];
    promptLine("  Set passphrase:  ", pass1, sizeof(pass1), true);
    promptLine("  Confirm:         ", pass2, sizeof(pass2), true);

    if (strcmp(pass1, pass2) != 0) {
        Serial.println("  Passphrases do not match.");
        secureClear(pass1, sizeof(pass1));
        secureClear(pass2, sizeof(pass2));
        return;
    }

    generateSalt(gVault.salt);
    // Use a fixed 1-byte token so deriveKey never receives a null pointer.
    const uint8_t cliToken[] = { 0x00 };
    CryptoResult cr = deriveKey(pass1, strlen(pass1), cliToken, sizeof(cliToken), gVault.salt, gVault.masterKey);
    secureClear(pass1, sizeof(pass1));
    secureClear(pass2, sizeof(pass2));

    if (cr != CryptoResult::OK) { Serial.println("  Key derivation failed."); return; }

    gVault.unlocked   = true;
    gVault.unlockedAt = millis();

    if (!saveVault(gVault)) {
        Serial.println("  Failed to write vault.bin.");
        gVault.lock();
        return;
    }
    Serial.println("  Vault created and unlocked.");
}

static void cmdUnlock() {
    if (gVault.unlocked) { Serial.println("  Already unlocked."); return; }

    if (!LittleFS.exists(VAULT_FILE_PATH)) {
        Serial.println("  No vault found. Type 'init' to create one.");
        return;
    }

    uint8_t salt[PBKDF2_SALT_LEN];
    if (!readVaultSalt(salt)) {
        Serial.println("  Failed to read vault header.");
        return;
    }

    char pass[128];
    promptLine("  Passphrase: ", pass, sizeof(pass), true);

    const uint8_t cliToken[] = { 0x00 };
    CryptoResult cr = deriveKey(pass, strlen(pass), cliToken, sizeof(cliToken), salt, gVault.masterKey);
    secureClear(pass, sizeof(pass));

    if (cr != CryptoResult::OK) { Serial.println("  Key derivation failed."); return; }

    memcpy(gVault.salt, salt, PBKDF2_SALT_LEN);
    gVault.unlocked = true;

    if (!loadVault(gVault)) {
        gVault.lock();
        Serial.println("  Wrong passphrase or corrupted vault.");
        return;
    }
    Serial.printf("  Unlocked — %u record(s) loaded.\n", (unsigned)gVault.records.size());
}

static void cmdLock() {
    gVault.lock();
    Serial.println("  Vault locked. Master key wiped.");
}

static void cmdSave() {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (saveVault(gVault)) Serial.println("  Saved.");
    else                   Serial.println("  Save failed.");
}

static void cmdWipe() {
    char confirm[4];
    promptLine("  Delete vault.bin? This cannot be undone. [y/N]: ", confirm, sizeof(confirm));
    if (toupper((unsigned char)confirm[0]) != 'Y') {
        Serial.println("  Cancelled."); return;
    }
    if (gVault.unlocked) gVault.lock();
    if (LittleFS.remove(VAULT_FILE_PATH))
        Serial.println("  vault.bin deleted. Type 'init' to create a new vault.");
    else
        Serial.println("  Failed — file may not exist.");
}

static void cmdStatus() {
    Serial.println();
    Serial.printf("  Vault:     %s\n", gVault.unlocked ? "UNLOCKED" : "LOCKED");
    if (gVault.unlocked) {
        Serial.printf("  Records:   %u / %u\n",
                      (unsigned)gVault.records.size(), VAULT_MAX_RECORDS);
        uint32_t upSec = (millis() - gVault.unlockedAt) / 1000;
        Serial.printf("  Unlocked:  %lu s ago\n", (unsigned long)upSec);
    }
    Serial.printf("  File:      %s\n",
                  LittleFS.exists(VAULT_FILE_PATH) ? "vault.bin exists" : "no vault.bin");
    Serial.println();
}

// ============================================================
// Record listing
// ============================================================

#define PAGE_SIZE 10

static void cmdList(int page) {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }

    int total = (int)gVault.records.size();
    if (total == 0) { Serial.println("  Vault is empty."); return; }

    int pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    if (page < 1) page = 1;
    if (page > pages) page = pages;

    int start = (page - 1) * PAGE_SIZE;
    int end   = start + PAGE_SIZE;
    if (end > total) end = total;

    Serial.println();
    Serial.printf("  %-4s  %-8s  %-28s  %s\n", "#", "Type", "Domain", "Username");
    Serial.println("  ----  --------  ----------------------------  --------------------");
    for (int i = start; i < end; i++) {
        auto& r = gVault.records[i];
        Serial.printf("  %-4d  %-8s  %-28s  %s\n",
                      i + 1, typeName(r.type),
                      r.domain.c_str(), r.username.c_str());
    }

    if (pages > 1) {
        Serial.printf("\n  Page %d / %d", page, pages);
        if (page < pages) Serial.printf("  —  type 'ls %d' for next", page + 1);
        Serial.println();
    }
    Serial.println();
}

// ============================================================
// Record detail
// ============================================================

static void cmdShow(int n) {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (n < 1 || n > (int)gVault.records.size()) {
        Serial.printf("  No record #%d.\n", n); return;
    }

    auto& r = gVault.records[n - 1];
    char dateBuf[16];
    fmtDate(dateBuf, sizeof(dateBuf), r.lastChanged);

    Serial.println();
    Serial.printf("  Record #%d\n", n);
    Serial.printf("  Type:      %s\n", typeName(r.type));
    Serial.printf("  Domain:    %s\n", r.domain.c_str());
    Serial.printf("  Username:  %s\n", r.username.c_str());
    Serial.printf("  Password:  %s\n", r.password.c_str());
    if (!r.queryValue.empty())
        Serial.printf("  Query:     %s\n", r.queryValue.c_str());
    Serial.printf("  Changed:   %s\n", dateBuf);

    if (!r.history.empty()) {
        Serial.printf("  History:   %u entr%s\n",
                      (unsigned)r.history.size(),
                      r.history.size() == 1 ? "y" : "ies");
        for (size_t i = 0; i < r.history.size(); i++) {
            char hd[16];
            fmtDate(hd, sizeof(hd), r.history[i].changedAt);
            Serial.printf("    [%u] %s  (%s)\n",
                          (unsigned)(i + 1),
                          r.history[i].password.c_str(), hd);
        }
    }

    Serial.println();
    Serial.printf("  type  %-3d — type password via HID\n", n);
    Serial.printf("  typeu %-3d — type username via HID\n", n);
    Serial.printf("  typea %-3d — username + Tab + password + Enter\n", n);
    Serial.println();
}

// ============================================================
// HID output
// ============================================================

static void hidCountdown(const char* what, const char* domain) {
    Serial.printf("\n  Typing %s for \"%s\"\n", what, domain);
    Serial.println("  Focus your target window!");
    for (int i = 3; i > 0; i--) {
        Serial.printf("  %d...", i);
        delay(1000);
    }
    Serial.print("  Go!  ");
}

static void cmdType(int n) {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (n < 1 || n > (int)gVault.records.size()) {
        Serial.printf("  No record #%d.\n", n); return;
    }
    auto& r = gVault.records[n - 1];
    if (r.password.empty()) { Serial.println("  Password field is empty."); return; }

    hidCountdown("password", r.domain.c_str());
    HidResult hr = typePassword(r.password.c_str(), false);
    switch (hr) {
        case HidResult::OK:        Serial.println("Sent!"); break;
        case HidResult::NOT_READY: Serial.println("\n  HID not ready — is USB HID enumerated?"); break;
        case HidResult::EMPTY_PW:  Serial.println("\n  Empty password."); break;
    }
}

static void cmdTypeU(int n) {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (n < 1 || n > (int)gVault.records.size()) {
        Serial.printf("  No record #%d.\n", n); return;
    }
    auto& r = gVault.records[n - 1];
    if (r.username.empty()) { Serial.println("  Username field is empty."); return; }

    hidCountdown("username", r.domain.c_str());
    HidResult hr = typePassword(r.username.c_str(), false);
    switch (hr) {
        case HidResult::OK:        Serial.println("Sent!"); break;
        case HidResult::NOT_READY: Serial.println("\n  HID not ready — is USB HID enumerated?"); break;
        case HidResult::EMPTY_PW:  Serial.println("\n  Empty username."); break;
    }
}

static void cmdTypeA(int n) {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (n < 1 || n > (int)gVault.records.size()) {
        Serial.printf("  No record #%d.\n", n); return;
    }
    auto& r = gVault.records[n - 1];

    hidCountdown("username+Tab+password+Enter", r.domain.c_str());

    HidResult hr = HidResult::OK;
    if (!r.username.empty())
        hr = typePassword(r.username.c_str(), false);
    if (hr == HidResult::OK)
        hr = hidTypeChar('\t');
    if (hr == HidResult::OK && !r.password.empty())
        hr = typePassword(r.password.c_str(), true);

    switch (hr) {
        case HidResult::OK:        Serial.println("Sent!"); break;
        case HidResult::NOT_READY: Serial.println("\n  HID not ready — is USB HID enumerated?"); break;
        case HidResult::EMPTY_PW:  Serial.println("\n  Empty field."); break;
    }
}

// ============================================================
// Record mutation
// ============================================================

static void cmdAdd() {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (gVault.records.size() >= VAULT_MAX_RECORDS) {
        Serial.println("  Vault is full."); return;
    }

    Serial.println();

    char typeCh[4];
    promptLine("  Type [W=web P=pc A=api K=key S=preshare O=other]: ", typeCh, sizeof(typeCh));
    RecordType rt = RecordType::OTHER;
    switch (toupper((unsigned char)typeCh[0])) {
        case 'W': rt = RecordType::WEB_SERVICE;  break;
        case 'P': rt = RecordType::PC_ACCOUNT;   break;
        case 'A': rt = RecordType::API_TOKEN;    break;
        case 'K': rt = RecordType::ENCRYPT_KEY;  break;
        case 'S': rt = RecordType::PRESHARE_KEY; break;
    }

    char domain[128], username[128], password[256], query[128];
    promptLine("  Domain:              ", domain,   sizeof(domain));
    promptLine("  Username:            ", username, sizeof(username));
    promptLine("  Password [blank=gen]: ", password, sizeof(password), true);

    if (strlen(password) == 0) {
        if (!generatePassword(password, 20, CharProfile::STANDARD)) {
            Serial.println("  Password generation failed."); return;
        }
        Serial.printf("  Generated: %s\n", password);
    }

    promptLine("  Query value:         ", query, sizeof(query));

    char confirm[4];
    promptLine("  Save? [y/N]: ", confirm, sizeof(confirm));
    if (toupper((unsigned char)confirm[0]) != 'Y') {
        Serial.println("  Cancelled.");
        secureClear(password, sizeof(password));
        return;
    }

    VaultRecord rec;
    generateUUID(rec.uuid);
    rec.type        = rt;
    rec.domain      = domain;
    rec.username    = username;
    rec.password    = std::string(password, strlen(password));
    rec.queryValue  = query;
    rec.lastChanged = now();
    secureClear(password, sizeof(password));

    gVault.records.push_back(rec);

    if (!saveVault(gVault)) {
        Serial.println("  Save failed — record not added.");
        gVault.records.pop_back();
        return;
    }
    Serial.printf("  Record #%u added and saved.\n", (unsigned)gVault.records.size());
}

static void cmdDel(int n) {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (n < 1 || n > (int)gVault.records.size()) {
        Serial.printf("  No record #%d.\n", n); return;
    }

    auto& r = gVault.records[n - 1];
    char confirm[4];
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "  Delete '%s' (%s)? [y/N]: ",
             r.domain.c_str(), typeName(r.type));
    promptLine(prompt, confirm, sizeof(confirm));

    if (toupper((unsigned char)confirm[0]) != 'Y') {
        Serial.println("  Cancelled."); return;
    }

    secureClear(gVault.records[n - 1].password.data(),
                gVault.records[n - 1].password.size());
    gVault.records.erase(gVault.records.begin() + (n - 1));

    if (saveVault(gVault)) Serial.println("  Deleted and saved.");
    else                   Serial.println("  Deleted (warning: save failed).");
}

static void cmdRotate(int n) {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }
    if (n < 1 || n > (int)gVault.records.size()) {
        Serial.printf("  No record #%d.\n", n); return;
    }

    auto& r = gVault.records[n - 1];

    char newPw[256];
    promptLine("  New password [blank=gen]: ", newPw, sizeof(newPw), true);

    if (strlen(newPw) == 0) {
        if (!generatePassword(newPw, 20, CharProfile::STANDARD)) {
            Serial.println("  Generation failed."); return;
        }
        Serial.printf("  Generated: %s\n", newPw);
    }

    r.rotatePassword(std::string(newPw, strlen(newPw)), now());
    secureClear(newPw, sizeof(newPw));

    if (saveVault(gVault))
        Serial.printf("  Rotated and saved. History now has %u entr%s.\n",
                      (unsigned)r.history.size(),
                      r.history.size() == 1 ? "y" : "ies");
    else
        Serial.println("  Rotated (warning: save failed).");
}

// ============================================================
// Backup / restore / USB drive
// ============================================================

static void cmdBackup() {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }

    char filename[BACKUP_FILENAME_MAX] = {};
    Serial.println("  Exporting encrypted backup to DMZ...");
    if (!backupExport(gVault, sDateBase, filename, sizeof(filename))) {
        Serial.println("  Backup failed."); return;
    }

    Serial.printf("  Written: %s\n", filename);
    for (auto& r : gVault.records) {
        if (r.type == RecordType::BACKUP_KEY) {
            Serial.println("  BACKUP_KEY (write this down for disaster recovery):");
            Serial.printf("    %s\n", r.password.c_str());
            break;
        }
    }
}

static void cmdRestore() {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }

    // List available backups
    gBkpCount = 0;
    backupListFiles(bkpListCb);

    if (gBkpCount == 0) {
        Serial.println("  No backup files found on DMZ."); return;
    }

    Serial.println();
    Serial.println("  Available backups:");
    for (int i = 0; i < gBkpCount; i++)
        Serial.printf("    %d.  %s\n", i + 1, gBkpList[i]);
    Serial.println("    0.  Cancel");

    char selBuf[8];
    promptLine("\n  Select: ", selBuf, sizeof(selBuf));
    int sel = atoi(selBuf);
    if (sel < 1 || sel > gBkpCount) { Serial.println("  Cancelled."); return; }

    const char* chosen = gBkpList[sel - 1];
    Serial.printf("  Selected: %s\n", chosen);

    // Resolve decryption key
    bool hasKey = false;
    for (auto& r : gVault.records)
        if (r.type == RecordType::BACKUP_KEY) { hasKey = true; break; }

    char hexKeyBuf[68] = {};
    const char* hexKeyPtr = nullptr;

    if (hasKey) {
        Serial.println("  BACKUP_KEY found in vault — decrypting automatically.");
    } else {
        Serial.println("  No BACKUP_KEY in this vault (vault was wiped/re-created).");
        Serial.println("  Paste the 64-char hex key shown when the backup was made:");
        promptLine("  Key: ", hexKeyBuf, sizeof(hexKeyBuf));
        if (strlen(hexKeyBuf) != 64) {
            Serial.println("  Invalid — must be exactly 64 hex chars."); return;
        }
        hexKeyPtr = hexKeyBuf;
    }

    char confirm[4];
    promptLine("  Merge into vault? [y/N]: ", confirm, sizeof(confirm));
    if (toupper((unsigned char)confirm[0]) != 'Y') {
        Serial.println("  Cancelled.");
        secureClear(hexKeyBuf, sizeof(hexKeyBuf));
        return;
    }

    int found = 0;
    int result = backupImport(gVault, chosen, &found, hexKeyPtr);
    secureClear(hexKeyBuf, sizeof(hexKeyBuf));

    if (result >= 0) {
        Serial.printf("  Backup contained %d record(s).\n", found);
        if (result == 0)
            Serial.println("  0 merged — all records already up-to-date in vault.");
        else
            Serial.printf("  %d record(s) added or updated.\n", result);

        // Delete the backup file after successful merge
        if (backupDeleteFile(chosen))
            Serial.printf("  %s deleted from DMZ.\n", chosen);
        else
            Serial.println("  Warning: could not delete backup file.");
    } else {
        const char* reason = "unknown error";
        switch (result) {
            case -1: reason = "mount or I/O error (file missing?)"; break;
            case -2: reason = "invalid backup format";               break;
            case -3: reason = "authentication failed — wrong key";   break;
            case -4: reason = "BACKUP_KEY not found or invalid hex"; break;
            case -5: reason = "out of memory";                       break;
        }
        Serial.printf("  Restore failed: %s\n", reason);
    }
}

static void cmdDelBkp() {
    if (!gVault.unlocked) { Serial.println("  Vault is locked."); return; }

    gBkpCount = 0;
    backupListFiles(bkpListCb);

    if (gBkpCount == 0) { Serial.println("  No backup files on DMZ."); return; }

    Serial.println();
    Serial.println("  Backups on DMZ:");
    for (int i = 0; i < gBkpCount; i++)
        Serial.printf("    %d.  %s\n", i + 1, gBkpList[i]);
    Serial.println("    0.  Cancel");

    char selBuf[8];
    promptLine("\n  Select: ", selBuf, sizeof(selBuf));
    int sel = atoi(selBuf);
    if (sel < 1 || sel > gBkpCount) { Serial.println("  Cancelled."); return; }

    const char* chosen = gBkpList[sel - 1];

    char confirm[4];
    char prompt[48];
    snprintf(prompt, sizeof(prompt), "  Delete %s and its key? [y/N]: ", chosen);
    promptLine(prompt, confirm, sizeof(confirm));
    if (toupper((unsigned char)confirm[0]) != 'Y') {
        Serial.println("  Cancelled."); return;
    }

    // Delete the backup file from DMZ
    bool fileGone = backupDeleteFile(chosen);

    // Delete the matching BACKUP_KEY record from vault
    bool keyGone = false;
    for (size_t i = 0; i < gVault.records.size(); i++) {
        auto& r = gVault.records[i];
        if (r.type == RecordType::BACKUP_KEY &&
            strcasecmp(r.username.c_str(), chosen) == 0) {
            secureClear(r.password.data(), r.password.size());
            gVault.records.erase(gVault.records.begin() + (int)i);
            saveVault(gVault);
            keyGone = true;
            break;
        }
    }

    if (fileGone) Serial.printf("  %s deleted from DMZ.\n", chosen);
    else          Serial.printf("  Warning: could not delete %s from DMZ.\n", chosen);

    if (keyGone)  Serial.println("  BACKUP_KEY record removed from vault.");
    else          Serial.println("  Note: no matching BACKUP_KEY record found in vault.");
}

static void cmdUsbDrive() {
    // No vault lock check — drive must be usable to restore a backup to a new chip.

    // Enumerate existing .BKP files on DMZ
    gBkpCount = 0;
    backupListFiles(bkpListCb);

    bool imageOk = false;

    if (gBkpCount == 0) {
        Serial.println("  No backups on DMZ — exposing empty drive.");
        Serial.println("  Copy a .BKP file onto the drive, then disconnect to import it.");
        imageOk = buildFat12Image(nullptr, 0);
    } else {
        Serial.printf("  Building FAT12 image with %d file(s)...\n", gBkpCount);

        uint32_t maxPerFile = (uint32_t)(FAT_IMAGE_SECTORS - FAT_DATA_START)
                              * FAT_SECTOR_SIZE / (uint32_t)gBkpCount;

        FatEntry* entries = (FatEntry*)malloc(sizeof(FatEntry) * gBkpCount);
        if (!entries) { Serial.println("  Allocation failed."); return; }

        bool ok = true;
        for (int i = 0; i < gBkpCount && ok; i++) {
            uint8_t* buf = (uint8_t*)ps_malloc(maxPerFile);
            if (!buf) { ok = false; break; }
            size_t n = backupReadFile(gBkpList[i], buf, maxPerFile);
            if (n == 0) { free(buf); ok = false; break; }
            to83Name(i, entries[i].name83);
            strncpy(entries[i].nameLfn, gBkpList[i], sizeof(entries[i].nameLfn) - 1);
            entries[i].nameLfn[sizeof(entries[i].nameLfn) - 1] = '\0';
            entries[i].data = buf;
            entries[i].size = (uint32_t)n;
        }

        imageOk = ok && buildFat12Image(entries, gBkpCount);
        for (int i = 0; i < gBkpCount; i++) if (entries[i].data) free((void*)entries[i].data);
        free(entries);
    }

    if (!imageOk) { Serial.println("  Failed to build image."); return; }

    sMSC.mediaPresent(true);
    if (gBkpCount > 0)
        Serial.println("  USB drive active — copy .BKP files to your PC.");
    Serial.println("  Drive disconnects in 60 seconds (or press any key).");

    uint32_t deadline = millis() + 60000;
    while (millis() < deadline) {
        if (Serial.available()) { Serial.read(); break; }
        Serial.printf("\r  %2lu s remaining...  ", (unsigned long)((deadline - millis())/1000 + 1));
        delay(500);
    }

    sMSC.mediaPresent(false);
    Serial.println("\n  Drive disconnected. Checking for new files...");
    syncPsramToFlash();
    free(sFatImage); sFatImage = nullptr;
}

// ============================================================
// Password generator (no storage)
// ============================================================

static void cmdGen(int len, CharProfile profile) {
    if (len < 1 || len > 92) { Serial.println("  Length must be 1–92."); return; }
    char buf[256];
    if (generatePassword(buf, (uint8_t)len, profile))
        Serial.printf("  %s\n", buf);
    else
        Serial.println("  Generation failed.");
}

// ============================================================
// Help
// ============================================================

static void printHelp() {
    Serial.println();
    Serial.println("  Vault");
    Serial.println("    init                  create a new vault");
    Serial.println("    unlock                unlock vault (prompts passphrase)");
    Serial.println("    lock                  lock vault, wipe master key");
    Serial.println("    save                  force-save to flash");
    Serial.println("    status                vault info and record count");
    Serial.println("    wipe                  delete vault.bin (no unlock needed)");
    Serial.println();
    Serial.println("  Records");
    Serial.println("    ls / list [page]      list records (10 per page)");
    Serial.println("    show <n>              full record + password history");
    Serial.println("    add                   add new record (interactive)");
    Serial.println("    del <n>               delete record");
    Serial.println("    rotate <n>            rotate password, save old to history");
    Serial.println();
    Serial.println("  HID Output  (3 s countdown)");
    Serial.println("    type  <n>             type password (no Enter)");
    Serial.println("    typeu <n>             type username (no Enter)");
    Serial.println("    typea <n>             username + Tab + password + Enter");
    Serial.println();
    Serial.println("  Backup");
    Serial.println("    backup                export encrypted backup to DMZ (unique key each time)");
    Serial.println("    restore               merge a DMZ backup into vault, delete after");
    Serial.println("    delbkp               delete a backup file + its key from vault");
    Serial.println("    usbdrive              expose DMZ as USB drive for 60 s (no unlock needed)");
    Serial.println();
    Serial.println("  Utility");
    Serial.println("    date                  show/set date (y / +days / YYYY-MM-DD)");
    Serial.println("    gen [len] [A|S|H|B]   generate password (default: 20 S)");
    Serial.println("    help / ?              this menu");
    Serial.println();
}

// ============================================================
// Command parser
// ============================================================

static void processLine(const char* line) {
    while (*line == ' ') line++;
    if (line[0] == '\0') { Serial.print("> "); return; }

    char tmp[128];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char* tok  = strtok(tmp, " \t");
    if (!tok) { Serial.print("> "); return; }
    char* arg1 = strtok(nullptr, " \t");
    char* arg2 = strtok(nullptr, " \t");
    char* ep;

    if      (strcasecmp(tok, "date")   == 0) cmdDateCheck();
    else if (strcasecmp(tok, "init")   == 0) cmdInit();
    else if (strcasecmp(tok, "unlock") == 0) cmdUnlock();
    else if (strcasecmp(tok, "lock")   == 0) cmdLock();
    else if (strcasecmp(tok, "save")   == 0) cmdSave();
    else if (strcasecmp(tok, "status") == 0) cmdStatus();
    else if (strcasecmp(tok, "help")   == 0 || strcmp(tok, "?") == 0) printHelp();
    else if (strcasecmp(tok, "wipe")   == 0) cmdWipe();
    else if (strcasecmp(tok, "ls")     == 0 || strcasecmp(tok, "list") == 0)
        cmdList(arg1 ? (int)strtol(arg1, &ep, 10) : 1);
    else if (strcasecmp(tok, "show")   == 0) {
        if (!arg1) Serial.println("  Usage: show <n>");
        else cmdShow((int)strtol(arg1, &ep, 10));
    }
    else if (strcasecmp(tok, "add")    == 0) cmdAdd();
    else if (strcasecmp(tok, "del")    == 0) {
        if (!arg1) Serial.println("  Usage: del <n>");
        else cmdDel((int)strtol(arg1, &ep, 10));
    }
    else if (strcasecmp(tok, "rotate") == 0) {
        if (!arg1) Serial.println("  Usage: rotate <n>");
        else cmdRotate((int)strtol(arg1, &ep, 10));
    }
    else if (strcasecmp(tok, "type")   == 0) {
        if (!arg1) Serial.println("  Usage: type <n>");
        else cmdType((int)strtol(arg1, &ep, 10));
    }
    else if (strcasecmp(tok, "typeu")  == 0) {
        if (!arg1) Serial.println("  Usage: typeu <n>");
        else cmdTypeU((int)strtol(arg1, &ep, 10));
    }
    else if (strcasecmp(tok, "typea")  == 0) {
        if (!arg1) Serial.println("  Usage: typea <n>");
        else cmdTypeA((int)strtol(arg1, &ep, 10));
    }
    else if (strcasecmp(tok, "backup")   == 0) cmdBackup();
    else if (strcasecmp(tok, "restore")  == 0) cmdRestore();
    else if (strcasecmp(tok, "delbkp")   == 0) cmdDelBkp();
    else if (strcasecmp(tok, "usbdrive") == 0) cmdUsbDrive();
    else if (strcasecmp(tok, "gen")    == 0) {
        int len = arg1 ? (int)strtol(arg1, &ep, 10) : 20;
        CharProfile prof = CharProfile::STANDARD;
        if (arg2) {
            switch (toupper((unsigned char)arg2[0])) {
                case 'A': prof = CharProfile::FULL_ASCII;  break;
                case 'H': prof = CharProfile::HEXADECIMAL; break;
                case 'B': prof = CharProfile::BASE64;      break;
                default:  prof = CharProfile::STANDARD;    break;
            }
        }
        cmdGen(len, prof);
    }
    else Serial.printf("  Unknown: '%s'  (type help)\n", tok);

    Serial.print("> ");
}

// ============================================================
// Serial line reader
// ============================================================

static char    sLine[256];
static uint8_t sLineLen  = 0;
static char    sLastEnd  = 0;   // tracks \r\n pair detection across calls

static bool pollLine() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            // skip the paired character in a \r\n or \n\r sequence
            if ((c == '\n' && sLastEnd == '\r') || (c == '\r' && sLastEnd == '\n'))
                { sLastEnd = 0; continue; }
            sLastEnd = c;
            sLine[sLineLen] = '\0';
            sLineLen = 0;
            Serial.println();
            return true;
        }
        sLastEnd = 0;
        if ((c == 8 || c == 127) && sLineLen > 0) {
            sLineLen--;
            Serial.print("\b \b");
            continue;
        }
        if (sLineLen < sizeof(sLine) - 1) {
            sLine[sLineLen++] = c;
            Serial.print(c);
        }
    }
    return false;
}

// ============================================================
// Entry points
// ============================================================

static void printWelcome() {
    Serial.print("\r\n\r\nPhotonPass Vault CLI\r\n");
    Serial.print("====================\r\n");
    Serial.printf("Build: %s %s\r\n", __DATE__, __TIME__);
    Serial.print("\r\n");
    if (gVault.unlocked)
        Serial.printf("Vault unlocked -- %u record(s).\r\n", (unsigned)gVault.records.size());
    else if (LittleFS.exists(VAULT_FILE_PATH))
        Serial.print("Vault found. Type 'unlock' to unlock.\r\n");
    else
        Serial.print("No vault found. Type 'init' to create one.\r\n");
    Serial.print("Type 'help' for commands.\r\n");
}

void setup() {
    WiFi.mode(WIFI_OFF);
    btStop();

    sMSC.vendorID("PHOTON");
    sMSC.productID("PASS-BK");
    sMSC.productRevision("1.0");
    sMSC.onRead(mscRead);
    sMSC.onWrite(mscWrite);
    sMSC.onStartStop(mscStartStop);
    sMSC.mediaPresent(false);
    sMSC.begin(FAT_IMAGE_SECTORS, FAT_SECTOR_SIZE);

    hidBegin();
    delay(2000);

    Serial.begin(115200);

    if (!LittleFS.begin(true))
        while (true) delay(1000);  // LittleFS fatal — hang

    loadStoredDate();
}

void loop() {
    static bool sPrevConn = false;
    bool conn = (bool)Serial;

    if (conn && !sPrevConn) {
        delay(150);
        printWelcome();
        cmdDateCheck();
        Serial.print("\r\n> ");
    }
    sPrevConn = conn;

    if (conn && pollLine())
        processLine(sLine);
}
