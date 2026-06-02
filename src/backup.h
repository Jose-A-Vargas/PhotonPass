#pragma once
#include <stdint.h>
#include "vault.h"

// ============================================================
// PhotonPass encrypted backup — DMZ LittleFS partition
//
// File: /backup.bin on the "dmz" partition (5 MB, label "dmz").
// The "dmz" partition is mounted only on explicit user request
// and unmounted immediately after use.
//
// Backup file layout:
//   BackupHeader (56 bytes, partially unencrypted):
//     magic[4]       = "PPBK"
//     version[1]     = BACKUP_VERSION
//     flags[1]       = 0
//     recordCount[2]
//     keyUuid[16]    = UUID of the BACKUP_KEY vault record used
//     nonce[12]      = fresh random on every export
//     ---- AAD boundary (offset 36) ----
//     tag[16]        = AES-256-GCM auth tag
//     payloadLen[4]  = encrypted payload byte length
//   AES-256-GCM ciphertext (serialized vault records)
//
// Key convention: BACKUP_KEY vault records store the 32-byte
// random key as a 64-char uppercase hex string in the password
// field (identical convention to PRESHARE_KEY for sync).
//
// If no BACKUP_KEY record exists when backupExport() is called,
// one is auto-generated and saved to the vault.
// ============================================================

#define BACKUP_FILE_PATH  "/backup.bin"
#define BACKUP_VERSION    1
#define BACKUP_MAGIC_0    0x50   // 'P'
#define BACKUP_MAGIC_1    0x50   // 'P'
#define BACKUP_MAGIC_2    0x42   // 'B'
#define BACKUP_MAGIC_3    0x4B   // 'K'

// Mount / unmount the 5 MB DMZ LittleFS partition.
// dmzMount() is idempotent if already mounted.
bool dmzMount();
void dmzUnmount();

// Encrypt all vault records with the BACKUP_KEY and write to DMZ /backup.bin.
// Auto-generates a BACKUP_KEY vault record if none exists.
// Returns false on any error (mount failure, I/O, crypto).
bool backupExport(VaultState& vault);

// Read DMZ /backup.bin, decrypt with the matching BACKUP_KEY (found by UUID),
// and merge records into vault (keep newest lastChanged on UUID collision).
//
// Return value:
//   >= 0  : success — number of records updated (0 = already up to date)
//     -1  : mount or I/O error (includes file-not-found)
//     -2  : invalid backup format (bad magic / version)
//     -3  : authentication failed (GCM tag mismatch)
//     -4  : no matching BACKUP_KEY in current vault
//     -5  : out of memory
int backupImport(VaultState& vault);
