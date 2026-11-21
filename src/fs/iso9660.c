#include <fs/iso9660.h>
#include <drivers/ahci.h>
#include <kernel/mem.h>
#include <drivers/monitor.h>

// ISO 9660 Primary Volume Descriptor is at sector 16 (LBA 16).
// The PVD has signature "CD001" at offset 1.
// Root directory record is at offset 156 (34 bytes).
// Root directory record contains the LBA and size of the root directory.

#define PVD_SECTOR    16
#define PVD_SIGNATURE_OFFSET 1
#define PVD_ROOT_DIR_OFFSET  156
#define PVD_VOLUME_ID_OFFSET 40
#define PVD_VOLUME_ID_LEN    32

// Directory record layout (variable length)
// Offset 0:  uint8  length           (record length)
// Offset 1:  uint8  ext_attr_length
// Offset 2:  uint32 extent_le        (LBA of data, little-endian)
// Offset 6:  uint32 extent_be        (LBA of data, big-endian)
// Offset 10: uint32 size_le          (data length, little-endian)
// Offset 14: uint32 size_be
// Offset 25: uint8  flags            (bit 1 = directory)
// Offset 32: uint8  name_len
// Offset 33: char   name[name_len]

#define DIR_FLAG_DIRECTORY (1 << 1)

// Mounted state
static int mounted = 0;
static uint32 mount_dev = 0;            // AHCI device index
static uint32 root_lba = 0;            // root directory LBA
static uint32 root_size = 0;           // root directory size in bytes
static char volume_id[PVD_VOLUME_ID_LEN + 1];

// One-sector scratch buffer for the PVD, directory sectors and the
// partial last sector of a file read. BSP-only, not reentrant.
static uint8 sector_buf[ISO_SECTOR_SIZE];

// Read ISO sectors into buf via AHCI
static int read_sectors(uint64 lba, uint32 count, void *buf) {
    return ahci_read_sectors(mount_dev, lba, count, buf);
}

// Read the Primary Volume Descriptor from sector 16
static int read_pvd(uint8 *buf) {
    if (read_sectors(PVD_SECTOR, 1, buf) != 0)
        return -1;

    // Check PVD type (1) and signature "CD001"
    if (buf[0] != 1) return -1;
    if (buf[1] != 'C' || buf[2] != 'D' || buf[3] != '0' ||
        buf[4] != '0' || buf[5] != '1')
        return -1;

    return 0;
}

// Case-insensitive compare (ISO 9660 filenames are uppercase)
static int name_match(const char *iso_name, uint8 iso_len,
                      const char *target) {
    uint8 tlen = 0;
    while (target[tlen]) tlen++;

    // ISO names may have ";1" version suffix, we strip it
    uint8 ilen = iso_len;
    if (ilen > 1 && iso_name[ilen - 1] >= '0' && iso_name[ilen - 1] <= '9' &&
        iso_name[ilen - 2] == ';')
        ilen -= 2;
    // Strip trailing dot (ISO 9660 Level 1: files without extension get "NAME.")
    while (ilen > 0 && iso_name[ilen - 1] == '.') ilen--;

    if (ilen != tlen) return 0;

    for (uint8 i = 0; i < ilen; i++) {
        char a = iso_name[i];
        char b = target[i];
        // Uppercase both for comparison
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

// Called for each directory record except "." and "..".
// Return non-zero to stop the walk.
typedef int (*dir_visit_fn)(const uint8 *rec, void *ctx);

// Walk a directory extent one sector at a time through sector_buf.
// ISO 9660 directory records never cross a sector boundary (a zero
// length byte pads the rest of the sector), so one sector is enough.
// Returns 1 if fn stopped the walk, 0 at the end, -1 on read error.
static int dir_walk(uint32 dir_lba, uint32 dir_size, dir_visit_fn fn, void *ctx) {
    uint32 sectors = (dir_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

    for (uint32 s = 0; s < sectors; s++) {
        if (read_sectors(dir_lba + s, 1, sector_buf) != 0)
            return -1;

        uint32 limit = dir_size - s * ISO_SECTOR_SIZE;
        if (limit > ISO_SECTOR_SIZE) limit = ISO_SECTOR_SIZE;

        uint32 offset = 0;
        while (offset < limit) {
            uint8 rec_len = sector_buf[offset];
            if (rec_len == 0) break;                        // sector padding
            if (rec_len < 34 || offset + rec_len > limit) break;  // malformed
            uint8 name_len = sector_buf[offset + 32];
            if (33 + (uint32)name_len > rec_len) break;     // malformed

            const uint8 *rec = &sector_buf[offset];
            // Skip "." and ".." entries (name_len == 1, name = 0x00 or 0x01)
            int dot = name_len == 1 && (rec[33] == 0 || rec[33] == 1);
            if (!dot && fn(rec, ctx))
                return 1;

            offset += rec_len;
        }
    }
    return 0;
}

typedef struct {
    const char *name;
    uint32 lba, size;
    uint8 flags;
} FindCtx;

static int find_visit(const uint8 *rec, void *ctx) {
    FindCtx *f = (FindCtx *)ctx;
    if (!name_match((const char *)&rec[33], rec[32], f->name))
        return 0;
    f->lba = *(const uint32 *)&rec[2];
    f->size = *(const uint32 *)&rec[10];
    f->flags = rec[25];
    return 1;
}

// Find a directory entry by name in a directory extent.
// Returns the entry's LBA and size via out params.
// Returns 0 on success, -1 if not found.
static int find_entry(uint32 dir_lba, uint32 dir_size,
                      const char *name,
                      uint32 *out_lba, uint32 *out_size, uint8 *out_flags) {
    FindCtx f = { name, 0, 0, 0 };
    if (dir_walk(dir_lba, dir_size, find_visit, &f) != 1)
        return -1;
    *out_lba = f.lba;
    *out_size = f.size;
    *out_flags = f.flags;
    return 0;
}

// Walk a path like "/bin/demo_app" by splitting on '/' and resolving
// each component through the directory tree.
static int resolve_path(const char *path, uint32 *out_lba,
                        uint32 *out_size, uint8 *out_flags) {
    uint32 cur_lba = root_lba;
    uint32 cur_size = root_size;

    // Skip leading '/'
    while (*path == '/') path++;
    if (*path == '\0') {
        // Root directory itself
        *out_lba = root_lba;
        *out_size = root_size;
        *out_flags = DIR_FLAG_DIRECTORY;
        return 0;
    }

    while (*path) {
        // Extract next path component
        char component[128];
        int i = 0;
        while (*path && *path != '/' && i < 127) {
            component[i++] = *path++;
        }
        component[i] = '\0';
        while (*path == '/') path++;

        uint32 entry_lba, entry_size;
        uint8 entry_flags;
        if (find_entry(cur_lba, cur_size, component,
                        &entry_lba, &entry_size, &entry_flags) != 0)
            return -1;

        if (*path != '\0' && !(entry_flags & DIR_FLAG_DIRECTORY))
            return -1;  // not a directory but path continues

        cur_lba = entry_lba;
        cur_size = entry_size;

        if (*path == '\0') {
            *out_lba = entry_lba;
            *out_size = entry_size;
            *out_flags = entry_flags;
            return 0;
        }
    }

    return -1;
}

int iso9660_mount(uint32 ahci_dev_idx) {
    mount_dev = ahci_dev_idx;

    uint8 *pvd = sector_buf;
    if (read_pvd(pvd) != 0) {
        kprint("ISO9660: PVD read failed\n");
        return -1;
    }

    // Extract root directory record (34 bytes at offset 156)
    uint8 *root_rec = pvd + PVD_ROOT_DIR_OFFSET;
    root_lba = *(uint32 *)(root_rec + 2);
    root_size = *(uint32 *)(root_rec + 10);

    // Extract volume ID
    memcpy(volume_id, pvd + PVD_VOLUME_ID_OFFSET, PVD_VOLUME_ID_LEN);
    volume_id[PVD_VOLUME_ID_LEN] = '\0';
    // Trim trailing spaces
    for (int i = PVD_VOLUME_ID_LEN - 1; i >= 0 && volume_id[i] == ' '; i--)
        volume_id[i] = '\0';

    mounted = 1;

    kprint("ISO9660: mounted \"");
    kprint(volume_id);
    kprint("\" root at LBA ");
    kprint_dec(root_lba);
    kprint(" (");
    kprint_dec(root_size);
    kprint(" bytes)\n");

    return 0;
}

int iso9660_mounted(void) {
    return mounted;
}

static int ls_visit(const uint8 *rec, void *ctx) {
    (void)ctx;
    uint8 name_len = rec[32];
    const char *entry_name = (const char *)&rec[33];

    if (rec[25] & DIR_FLAG_DIRECTORY)
        kprint("[DIR]  ");
    else {
        kprint_dec(*(const uint32 *)&rec[10]);
        kprint("  ");
    }

    // Print name (strip ";1" version suffix)
    for (uint8 i = 0; i < name_len; i++) {
        if (entry_name[i] == ';') break;
        putc(entry_name[i]);
    }
    putc('\n');
    return 0;
}

void iso9660_ls(const char *path) {
    if (!mounted) {
        kprint("ISO9660: not mounted\n");
        return;
    }

    uint32 dir_lba, dir_size;
    uint8 dir_flags;

    if (resolve_path(path, &dir_lba, &dir_size, &dir_flags) != 0) {
        kprint("ISO9660: path not found\n");
        return;
    }

    if (!(dir_flags & DIR_FLAG_DIRECTORY)) {
        kprint("ISO9660: not a directory\n");
        return;
    }

    if (dir_walk(dir_lba, dir_size, ls_visit, 0) < 0)
        kprint("ISO9660: read error\n");
}

uint32 iso9660_file_size(const char *path) {
    if (!mounted) return 0;
    uint32 lba, size;
    uint8 flags;
    if (resolve_path(path, &lba, &size, &flags) != 0) return 0;
    if (flags & DIR_FLAG_DIRECTORY) return 0;
    return size;
}

int iso9660_read_file(const char *path, void *buf, uint32 max_size) {
    if (!mounted) return -1;

    uint32 file_lba, file_size;
    uint8 flags;
    if (resolve_path(path, &file_lba, &file_size, &flags) != 0) return -1;
    if (flags & DIR_FLAG_DIRECTORY) return -1;

    uint32 to_read = file_size;
    if (to_read > max_size) to_read = max_size;

    // Whole sectors go straight to the caller's buffer; only the partial
    // last sector goes through sector_buf, so nothing past to_read is
    // written into buf.
    uint32 full = to_read / ISO_SECTOR_SIZE;
    uint32 tail = to_read % ISO_SECTOR_SIZE;

    if (full > 0 && read_sectors(file_lba, full, buf) != 0)
        return -1;

    if (tail > 0) {
        if (read_sectors(file_lba + full, 1, sector_buf) != 0)
            return -1;
        memcpy((uint8 *)buf + (uint64)full * ISO_SECTOR_SIZE, sector_buf, tail);
    }
    return (int)to_read;
}
