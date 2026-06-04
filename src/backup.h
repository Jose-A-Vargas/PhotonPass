#pragma once
#include <stdint.h>
#include <stddef.h>
#include "vault.h"

// ============================================================
// PhotonPass encrypted backup — DMZ FAT partition (5 MB, label "dmz")
//
// Backup filenames: {CHIPID}-{YYYYMMDD}-{N}.BKP
//   CHIPID   — 4 uppercase hex chars from the chip's efuse MAC (e.g. F898)
//   YYYYMMDD — date the backup was taken; "00000000" if no date configured
//   N        — 1-based counter, resets per chip+date (multiple backups same day)
//   Example:  F898-20260604-1.BKP, F898-20260604-2.BKP
//
// The file is AES-256-GCM encrypted using a per-vault BACKUP_KEY record.
// The BACKUP_KEY's 64-char hex value must be noted for disaster recovery
// (vault wipe + re-init loses the BACKUP_KEY record).
// ============================================================

#define BACKUP_FILENAME_MAX  28   // "XXXX-YYYYMMDD-NNN.BKP" + null + padding
#define BACKUP_EXT           ".BKP"
#define BACKUP_VERSION       1
#define BACKUP_MAGIC_0       0x50   // 'P'
#define BACKUP_MAGIC_1       0x50   // 'P'
#define BACKUP_MAGIC_2       0x42   // 'B'
#define BACKUP_MAGIC_3       0x4B   // 'K'

// Mount / unmount the DMZ FAT partition explicitly.
// dmzMount() is idempotent; dmzUnmount() is a no-op if not mounted.
bool dmzMount();
void dmzUnmount();

// Fill out[5] with 4 uppercase hex chars identifying this chip (efuse MAC).
void backupChipId(char out[5]);

// Encrypt all vault records and write a uniquely-named backup file to DMZ.
// Auto-generates a BACKUP_KEY record in vault if none exists.
// dateTs: unix midnight of the current date (from NVS date tracking); 0 = no date.
// Writes the generated filename (without leading '/') into outName if provided.
// Returns false on any error.
bool backupExport(VaultState& vault,
                  uint32_t    dateTs    = 0,
                  char*       outName   = nullptr,
                  size_t      outNameLen = 0);

// List .BKP files on DMZ. Calls cb(index, bare_filename) for each.
// Returns total count found. DMZ is mounted/unmounted internally.
int backupListFiles(void (*cb)(int idx, const char* name));

// Decrypt and merge a named backup file into vault.
//   outFound — receives total record count from backup (optional)
//   hexKey   — 64-char hex key for disaster recovery when BACKUP_KEY is gone (optional)
// Return: merged count (>=0) or error code.
//   -1  mount or I/O error      -3  authentication failed (wrong key)
//   -2  bad backup format       -4  BACKUP_KEY not found / invalid
//                               -5  out of memory
int backupImport(VaultState&  vault,
                 const char*  filename,
                 int*         outFound = nullptr,
                 const char*  hexKey   = nullptr);

// Read a backup file from DMZ into caller-provided buffer.
// Returns bytes read (0 on failure).
size_t backupReadFile(const char* filename, uint8_t* buf, size_t maxLen);

// Delete a backup file from DMZ. Returns true on success.
bool backupDeleteFile(const char* filename);
