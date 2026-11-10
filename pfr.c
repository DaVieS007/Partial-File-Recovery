/**
    Licensed under GNU/GPLv3
    Author: DaVieS at nPulse.net / Viktor Hlavaji
    Mail: davies@npulse.net
    Software is intended to recover files from a bad drive, written in pure C.
    Compile with: gcc -o pfr pfr.c | clang -o pfr pfr.c
    Windows:      x86_64-w64-mingw32-gcc -Wl,--stack,4194304 -o pfr64.exe pfr.c -static

    How it works (to keep the stress on a failing drive as low as possible):
      Pass 1  Copy:     Fast Forward in big chunks. On a read error fall back to
                        Block Mining: one sector at a time, ONE attempt each.
                        Consecutive bad sectors make it jump ahead (2, 4, 8 ...
                        sectors); the jumped area is noted as "skipped".
      Pass 2  Scrape:   every skipped area is read from its start forward and
                        from its end backward until the first error, then the
                        rest of it once, sector by sector.
      Pass 3  Retry:    only now are the remaining bad sectors retried, in rounds,
                        until [try] attempts per sector are used up.

    The state of every byte is kept in a map, saved as a GNU ddrescue style map
    file every 30 seconds and at the end. A run can be resumed from it: --map file
**/
#define _FILE_OFFSET_BITS 64      /* must come before any #include */

#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600   /* Vista+: GetTickCount64 */
    #endif
#else
    #define _POSIX_C_SOURCE 200809L   /* fseeko, clock_gettime, nanosleep, posix_memalign, fsync */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

typedef int64_t _long;

#ifdef _WIN32
    #include <windows.h>
    #include <winioctl.h>
    #include <io.h>
    #include <fcntl.h>
    #include <malloc.h>

    #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
        #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
    #endif

    #define FSEEK _fseeki64
    #define FTELL _ftelli64
#else
    #include <unistd.h>

    #define FSEEK fseeko
    #define FTELL ftello
#endif


#define EXIT_OK 0
#define EXIT_FAIL 1
#define VERSION "2.5r"
#define BUFFSZ 10485760           /* Fast Forward chunk size */
#define BUFFCAP (BUFFSZ * 2 + IO_ALIGN)
#define MBUFFSIZE 67108864        /* stdio buffer for file destinations */
#define IO_ALIGN 4096             /* buffer address alignment for raw device I/O */
#define CLEAN_BYTES 4096          /* good bytes in a row before returning to Fast Forward */
#define MAX_JUMP_SHIFT 11         /* largest skip-ahead: 2^11 = 2048 sectors */
#define MAX_LIST_RANGES 100       /* bad ranges printed in the final report */
#ifndef CKPT_MS
    #define CKPT_MS 30000         /* save the map file this often while running */
#endif

/* Test hooks (normal builds leave these at 0) */
#ifndef PFR_EMULATE_BAD
    #define PFR_EMULATE_BAD 0     /* emulate unreadable data starting at this offset */
#endif
#ifndef PFR_EMULATE_LEN
    #define PFR_EMULATE_LEN 262144
#endif
#ifndef PFR_EMULATE_FLAKY
    #define PFR_EMULATE_FLAKY 0   /* 1 = first half of the bad area becomes readable in Pass 3 */
#endif
#ifndef PFR_TEST_ALIGN
    #define PFR_TEST_ALIGN 0      /* force raw-device alignment rules on plain files */
#endif

/*
 *  State map: sorted list of [start, end) byte ranges that are NOT copied yet.
 *  Everything between the ranges is copied ('+' in the map file).
 */
#define RS_SKIPPED  1             /* jumped over in Pass 1, filled       map file: '*' */
#define RS_BAD      2             /* read failed, filled                 map file: '-' */
#define RS_NONTRIED 3             /* not copied yet                      map file: '?' */

typedef struct
{
    _long start;
    _long end;
    int state;
} range_t;

typedef struct
{
    range_t *r;
    size_t n;
    size_t cap;
} badmap_t;

typedef void (*pass_fn)(_long a, _long b, badmap_t *out);

#define PHASE_COPY   1
#define PHASE_SCRAPE 2
#define PHASE_RETRY  3

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

static FILE *src = NULL;
static FILE *dst = NULL;
static char *MBUFF = NULL;
static char *tmp = NULL;          /* read buffer */
static char *tmp2 = NULL;         /* differential compare buffer */
static char *buff = NULL;         /* Block Mining write-back buffer */
static _long buff_len = 0;
static _long buff_pos = 0;

static const char *src_path = NULL;
static int   src_is_device = 0;
static int   dst_is_device = 0;
static _long src_size = 0;        /* end position (endpos or source size) */
static _long unit = 512;          /* sector size = [skip] */
static _long align = 1;           /* I/O alignment: 1 for files, sector size for raw devices */
static bool  dst_raw = false;     /* destination needs whole-sector writes */
static char  fill_byte = 0x00;
static int   phase = PHASE_COPY;

/* Settings used by the passes */
static _long g_seek = 0;
static bool  g_differential = false;
static _long g_slimit = 0;
static _long g_rounds_left = 0;
static _long g_dst_old_size = 0;  /* differential: destination size before this run */

/* Statistics */
static _long st_written = 0;      /* bytes written to the destination */
static _long st_acquired = 0;     /* bytes read successfully */
static _long st_lost = 0;         /* bytes currently bad or skipped (filled) */
static _long st_recovered = 0;    /* bytes rescued by Pass 2 / Pass 3 */
static _long st_read_errors = 0;  /* failed read calls the drive had to endure */

/* Map file */
static int    g_argc = 0;
static char **g_argv = NULL;
static char   start_time[32];
static char   map_path[300];

/* Checkpoint: where the running pass is, so the map can be saved at any moment */
static badmap_t *g_pass_out = NULL;   /* new map being built by the pass */
static badmap_t *g_pass_src = NULL;   /* map the pass is working through */
static size_t    g_pass_idx = 0;      /* range of g_pass_src in progress */
static _long     g_ckpt_pos = 0;      /* everything in that range from here on is unfinished */
static _long     g_ckpt_end = 0;
static int       g_ckpt_state = 0;
static uint64_t  g_ckpt_last = 0;

/* Status line */
static time_t st_last = 0;
static int    st_lines = 0;
static time_t sp_time = 0;
static _long  sp_pos = 0;
static int    sp_mbps = 0;

static volatile sig_atomic_t stop_req = 0;

static void signalHandler(int signum)
{
    (void)signum;
    stop_req = 1;
}

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

/** Monotonic millisecond clock, used by the throttle and the checkpoints **/
static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

static void sleep_ms(uint64_t ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
#endif
}

/** Aligned buffers: raw disk I/O on Windows wants sector-aligned memory **/
static void *abuf_alloc(size_t size)
{
#ifdef _WIN32
    return _aligned_malloc(size, IO_ALIGN);
#else
    void *p = NULL;
    if(posix_memalign(&p, IO_ALIGN, size) != 0)
    {
        return NULL;
    }
    return p;
#endif
}

static void abuf_free(void *p)
{
    if(p == NULL)
    {
        return;
    }
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

/** Format a byte count into a human-readable string **/
static void format_size(_long bytes, char *out, int outsz)
{
    if(bytes >= 1099511627776LL)
    {
        snprintf(out, outsz, "%.1f TB", (double)bytes / 1099511627776.0);
    }
    else if(bytes >= 1073741824LL)
    {
        snprintf(out, outsz, "%.1f GB", (double)bytes / 1073741824.0);
    }
    else if(bytes >= 1048576LL)
    {
        snprintf(out, outsz, "%.1f MB", (double)bytes / 1048576.0);
    }
    else
    {
        snprintf(out, outsz, "%" PRId64 " B", bytes);
    }
}

/** Fatal error: nothing sensible left to do (the last saved map file stays usable) **/
static void die(const char *msg)
{
    fprintf(stderr, "\n\033[91m%s\033[m\n\n", msg);
    exit(EXIT_FAIL);
}

/* ------------------------------------------------------------------ */
/*  Windows drive / volume helpers                                     */
/* ------------------------------------------------------------------ */
#ifdef _WIN32

#define MAX_LOCKED 64
static HANDLE locked_vols[MAX_LOCKED];
static int    n_locked = 0;

/** Detect \\.\  or  //./ device paths **/
static int is_device_path(const char *path)
{
    if(path[0] == '\\' && path[1] == '\\' && path[2] == '.' && path[3] == '\\')
    {
        return 1;
    }
    if(path[0] == '/' && path[1] == '/' && path[2] == '.' && path[3] == '/')
    {
        return 1;
    }
    return 0;
}

/** \\.\X:  -> returns 'X', anything else -> 0 **/
static char volume_letter(const char *path)
{
    if(is_device_path(path) && isalpha((unsigned char)path[4]) && path[5] == ':' && path[6] == 0)
    {
        return (char)toupper((unsigned char)path[4]);
    }
    return 0;
}

/** Drive letter Windows is installed on (usually 'C') **/
static char system_drive_letter(void)
{
    char buf[MAX_PATH];
    UINT n = GetWindowsDirectoryA(buf, sizeof(buf));
    if(n >= 2 && buf[1] == ':')
    {
        return (char)toupper((unsigned char)buf[0]);
    }
    return 0;
}

/** Physical disk number behind a disk or volume handle, -1 if unknown **/
static int disk_number_of(HANDLE h)
{
    STORAGE_DEVICE_NUMBER sdn;
    DWORD unused;
    if(DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                       &sdn, sizeof(sdn), &unused, NULL)
       && sdn.DeviceType == FILE_DEVICE_DISK)
    {
        return (int)sdn.DeviceNumber;
    }
    return -1;
}

static void lock_and_dismount(HANDLE h, const char *name)
{
    DWORD unused;
    if(!DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &unused, NULL))
    {
        fprintf(stderr, "\033[93mWarning: unable to lock volume %s (error %lu), close programs and Explorer windows using it.\033[m\n",
                name, (unsigned long)GetLastError());
    }
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &unused, NULL);
}

/**
 *  Windows (Vista+) refuses writes to disk sectors that belong to a mounted
 *  volume.  Before writing to \\.\PhysicalDriveN, lock and dismount every
 *  volume that lives on it (including ones without a drive letter).
 *  The handles stay open (= locked) until release_devices() or process exit.
 */
static void lock_disk_volumes(int disk)
{
    char name[MAX_PATH];
    char sysvol[MAX_PATH];
    char windir[MAX_PATH];
    char sysroot[MAX_PATH];

    /* GUID name of the Windows system volume, e.g. \\?\Volume{...}\ */
    sysvol[0] = 0;
    if(GetWindowsDirectoryA(windir, sizeof(windir))
       && GetVolumePathNameA(windir, sysroot, sizeof(sysroot)))
    {
        GetVolumeNameForVolumeMountPointA(sysroot, sysvol, sizeof(sysvol));
    }

    HANDLE fv = FindFirstVolumeA(name, sizeof(name));
    if(fv == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        size_t len = strlen(name);
        int is_sys = (sysvol[0] && !strcmp(name, sysvol));

        if(len == 0 || name[len - 1] != '\\')
        {
            continue;
        }

        /* CreateFile needs the GUID path without the trailing backslash */
        name[len - 1] = 0;
        HANDLE hv = CreateFileA(name, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if(hv == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        if(disk_number_of(hv) != disk || n_locked >= MAX_LOCKED)
        {
            CloseHandle(hv);
            continue;
        }

        if(is_sys)
        {
            fprintf(stderr, "\n\033[91mRefusing to write: PhysicalDrive%d holds the Windows system volume\033[m\n\n", disk);
            exit(EXIT_FAIL);
        }

        fprintf(stdout, "Locking volume %s on PhysicalDrive%d\n", name, disk);
        lock_and_dismount(hv, name);
        locked_vols[n_locked] = hv;
        n_locked++;
    }
    while(FindNextVolumeA(fv, name, sizeof(name)));

    FindVolumeClose(fv);
}

/**
 *  Open a Windows device (\\.\PhysicalDriveN or \\.\X:) and return an
 *  unbuffered stdio FILE*, so every fread/fwrite goes straight to the disk
 *  with exactly the size and offset we ask for (raw disk I/O must be
 *  sector aligned).  Volumes are locked & dismounted when writing.
 *  Returns NULL on failure.
 */
static FILE *open_device(const char *path, int want_write)
{
    DWORD access = GENERIC_READ;
    DWORD share  = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD unused;
    char  letter = volume_letter(path);

    if(want_write)
    {
        access |= GENERIC_WRITE;
        if(letter && letter == system_drive_letter())
        {
            fprintf(stderr, "\n\033[91mRefusing to write to the Windows system volume %c:\033[m\n\n", letter);
            exit(EXIT_FAIL);
        }
    }

    HANDLE h = CreateFileA(path, access, share, NULL, OPEN_EXISTING,
                           want_write ? FILE_FLAG_WRITE_THROUGH : 0, NULL);
    if(h == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }

    /* Allow access to the last sectors of a volume (beyond the filesystem end) */
    DeviceIoControl(h, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &unused, NULL);

    if(want_write)
    {
        if(letter)
        {
            lock_and_dismount(h, path);
        }
        else
        {
            int disk = disk_number_of(h);
            if(disk >= 0)
            {
                lock_disk_volumes(disk);
            }
        }
    }

    int fd = _open_osfhandle((intptr_t)h, (want_write ? _O_RDWR : _O_RDONLY) | _O_BINARY);
    if(fd == -1)
    {
        CloseHandle(h);
        return NULL;
    }

    FILE *f = _fdopen(fd, want_write ? "r+b" : "rb");
    if(f == NULL)
    {
        _close(fd);
        return NULL;
    }

    setvbuf(f, NULL, _IONBF, 0);
    return f;
}

/** Get the byte length of a device / disk / volume **/
static _long device_size(FILE *f)
{
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    DWORD unused;
    GET_LENGTH_INFORMATION gli;
    DISK_GEOMETRY_EX gex;

    if(DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                       &gli, sizeof(gli), &unused, NULL))
    {
        return (_long)gli.Length.QuadPart;
    }

    if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                       &gex, sizeof(gex), &unused, NULL))
    {
        return (_long)gex.DiskSize.QuadPart;
    }

    return -1;
}

/** Logical sector size of a device, 512 if unknown **/
static _long device_sector_size(FILE *f)
{
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    DWORD unused;
    DISK_GEOMETRY g;

    if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                       &g, sizeof(g), &unused, NULL) && g.BytesPerSector >= 512)
    {
        return (_long)g.BytesPerSector;
    }
    return 512;
}

/** After writing a disk: let Windows re-read the partition table, unlock volumes **/
static void release_devices(FILE *f, int is_physical)
{
    int i;
    if(f != NULL && is_physical)
    {
        DWORD unused;
        HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
        FlushFileBuffers(h);
        DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &unused, NULL);
    }
    for(i = 0; i < n_locked; i++)
    {
        CloseHandle(locked_vols[i]);
    }
    n_locked = 0;
}

static void rtrim(char *s)
{
    int len = (int)strlen(s);
    while(len > 0 && isspace((unsigned char)s[len - 1]))
    {
        len--;
        s[len] = 0;
    }
}

/**
 *  List all physical drives and volumes visible on this machine.
 *  Shows: device path, size, model/product name, mapped drive letters.
 */
static void list_drives(void)
{
    int d;
    int i;
    DWORD unused;
    int letter_disk[26];          /* A=0..Z=25 -> PhysicalDrive# or -1 */

    /* No "There is no disk in the drive" popups for empty card readers */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    /* --- Map every drive letter to its physical disk number --- */
    for(d = 0; d < 26; d++)
    {
        char vol[8];
        letter_disk[d] = -1;
        sprintf(vol, "\\\\.\\%c:", 'A' + d);

        HANDLE hv = CreateFileA(vol, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if(hv == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        letter_disk[d] = disk_number_of(hv);
        CloseHandle(hv);
    }

    /* --- Enumerate PhysicalDrive0..31 --- */
    fprintf(stdout, "\n\033[1m  Physical Drives\033[m\n\n");
    fprintf(stdout, "  %-22s %-10s %-40s %s\n", "Path", "Size", "Model", "Volumes");
    fprintf(stdout, "  %-22s %-10s %-40s %s\n", "----", "----", "-----", "-------");

    for(i = 0; i < 32; i++)
    {
        char path[64];
        char size_str[32];
        char model[128];
        char letters[96];
        _long size = 0;
        DISK_GEOMETRY_EX gex;

        sprintf(path, "\\\\.\\PhysicalDrive%d", i);

        /* access 0 = query only, works without Administrator rights */
        HANDLE h = CreateFileA(path, 0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
        if(h == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        /* Size (GEOMETRY_EX needs no read access, GET_LENGTH_INFO does) */
        if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                           NULL, 0, &gex, sizeof(gex), &unused, NULL))
        {
            size = (_long)gex.DiskSize.QuadPart;
        }

        if(size > 0)
        {
            format_size(size, size_str, sizeof(size_str));
        }
        else
        {
            strcpy(size_str, "no media");
        }

        /* Model name via STORAGE_QUERY_PROPERTY */
        strcpy(model, "(unknown)");
        {
            BYTE buf[1024];
            STORAGE_PROPERTY_QUERY spq;
            memset(buf, 0, sizeof(buf));
            memset(&spq, 0, sizeof(spq));
            spq.PropertyId = StorageDeviceProperty;
            spq.QueryType  = PropertyStandardQuery;

            if(DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                               &spq, sizeof(spq), buf, sizeof(buf) - 1,
                               &unused, NULL))
            {
                STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)buf;
                char vendor[64];
                char product[64];
                vendor[0] = 0;
                product[0] = 0;

                if(desc->VendorIdOffset && desc->VendorIdOffset < sizeof(buf))
                {
                    snprintf(vendor, sizeof(vendor), "%s", (char *)buf + desc->VendorIdOffset);
                    rtrim(vendor);
                }
                if(desc->ProductIdOffset && desc->ProductIdOffset < sizeof(buf))
                {
                    snprintf(product, sizeof(product), "%s", (char *)buf + desc->ProductIdOffset);
                    rtrim(product);
                }

                if(vendor[0] && product[0])
                {
                    snprintf(model, sizeof(model), "%s %s", vendor, product);
                }
                else if(vendor[0] || product[0])
                {
                    snprintf(model, sizeof(model), "%s", vendor[0] ? vendor : product);
                }
            }
        }

        /* Collect drive letters that sit on this physical disk */
        letters[0] = 0;
        for(d = 0; d < 26; d++)
        {
            if(letter_disk[d] == i)
            {
                char lbl[8];
                if(letters[0])
                {
                    strcat(letters, ", ");
                }
                sprintf(lbl, "%c:", 'A' + d);
                strcat(letters, lbl);
            }
        }

        fprintf(stdout, "  %-22s %-10s %-40s %s\n",
                path, size_str, model, letters[0] ? letters : "-");

        CloseHandle(h);
    }

    /* --- List all drive-letter volumes --- */
    fprintf(stdout, "\n\033[1m  Volumes\033[m\n\n");
    fprintf(stdout, "  %-10s %-10s %-10s %-16s %s\n", "Path", "Size", "Type", "Disk", "Label");
    fprintf(stdout, "  %-10s %-10s %-10s %-16s %s\n", "----", "----", "----", "----", "-----");

    for(d = 0; d < 26; d++)
    {
        char root[4];
        char label[MAX_PATH + 1];
        char size_str[32];
        char disk_str[24];
        char vol_path[16];
        const char *type_str = "";
        ULARGE_INTEGER total;

        root[0] = (char)('A' + d);
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;

        UINT dtype = GetDriveTypeA(root);
        if(dtype == DRIVE_NO_ROOT_DIR || dtype == DRIVE_UNKNOWN)
        {
            continue;
        }

        switch(dtype)
        {
            case DRIVE_REMOVABLE:
            {
                type_str = "Removable";
                break;
            }
            case DRIVE_FIXED:
            {
                type_str = "Fixed";
                break;
            }
            case DRIVE_REMOTE:
            {
                type_str = "Network";
                break;
            }
            case DRIVE_CDROM:
            {
                type_str = "CD-ROM";
                break;
            }
            case DRIVE_RAMDISK:
            {
                type_str = "RAMDisk";
                break;
            }
            default:
            {
                break;
            }
        }

        /* Volume label */
        label[0] = 0;
        GetVolumeInformationA(root, label, sizeof(label), NULL, NULL, NULL, NULL, 0);

        /* Size via GetDiskFreeSpaceEx (total capacity) */
        if(GetDiskFreeSpaceExA(root, NULL, &total, NULL))
        {
            format_size((_long)total.QuadPart, size_str, sizeof(size_str));
        }
        else
        {
            strcpy(size_str, "-");
        }

        if(letter_disk[d] >= 0)
        {
            sprintf(disk_str, "PhysicalDrive%d", letter_disk[d]);
        }
        else
        {
            strcpy(disk_str, "-");
        }

        sprintf(vol_path, "\\\\.\\%c:", 'A' + d);

        fprintf(stdout, "  %-10s %-10s %-10s %-16s %s\n",
                vol_path, size_str, type_str, disk_str, label[0] ? label : "-");
    }

    fprintf(stdout, "\n\033[93m  Writing to a drive destroys its contents. Double-check the PhysicalDrive number!\033[m\n\n");
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/*  Source / destination I/O                                           */
/* ------------------------------------------------------------------ */

/** Some systems report EOF after an I/O error: reopen the source to clear it **/
static void reopen_src(void)
{
    fclose(src);
#ifdef _WIN32
    if(src_is_device)
    {
        src = open_device(src_path, 0);
    }
    else
    {
        src = fopen(src_path, "rb");
    }
#else
    src = fopen(src_path, "rb");
#endif
    if(src == NULL)
    {
        die("Unable to reopen source file for reading");
    }
}

/** Read len bytes at pos, returns the number of good bytes (len = success) **/
static _long read_at(_long pos, char *buf, _long len)
{
    size_t got = 0;

    if(FSEEK(src, pos, SEEK_SET) == 0)
    {
        got = fread(buf, 1, (size_t)len, src);
    }

    if((_long)got < len)
    {
        if(feof(src))
        {
            reopen_src();
        }
        else
        {
            clearerr(src);
        }
    }

#if PFR_EMULATE_BAD > 0
    {
        _long bad_start = PFR_EMULATE_BAD;
        _long bad_end = (_long)PFR_EMULATE_BAD + PFR_EMULATE_LEN;

        if(PFR_EMULATE_FLAKY && phase == PHASE_RETRY)
        {
            bad_start += PFR_EMULATE_LEN / 2;
        }

        if(pos < bad_end && pos + (_long)got > bad_start)
        {
            got = (bad_start > pos) ? (size_t)(bad_start - pos) : 0;
        }
    }
#endif

    if((_long)got < len)
    {
        st_read_errors++;
    }
    return (_long)got;
}

/** Write len bytes at pos; pads to a whole sector on raw drives (data needs room for it) **/
static void write_at(_long pos, char *data, _long len)
{
    _long wlen = len;

    if(dst_raw && (wlen % align) != 0)
    {
        _long pad = align - (wlen % align);
        memset(data + len, fill_byte, (size_t)pad);
        wlen += pad;
    }

    if(FSEEK(dst, pos, SEEK_SET) != 0)
    {
        die("Failed to seek Destination!");
    }

    if(fwrite(data, 1, (size_t)wlen, dst) != (size_t)wlen)
    {
        die("Failed to Write Destination!");
    }

    st_written += wlen;
}

/** Push everything written so far to the disk, so the map file never claims more than is stored **/
static void sync_dst(void)
{
    if(fflush(dst) != 0)
    {
        die("Failed to Write Destination!");
    }
#ifdef _WIN32
    _commit(_fileno(dst));
#else
    fsync(fileno(dst));
#endif
}

/** Differential mode: true when the destination already holds this data **/
static bool same_as_dst(_long pos, const char *data, _long len)
{
    bool same = false;

    if(FSEEK(dst, pos, SEEK_SET) == 0)
    {
        size_t got = fread(tmp2, 1, (size_t)len, dst);
        if((_long)got == len && memcmp(data, tmp2, (size_t)len) == 0)
        {
            same = true;
        }
    }
    clearerr(dst);
    return same;
}

/* Block Mining write-back buffer: collects contiguous sectors, written in big pieces */

static void buf_flush(void)
{
    if(buff_len > 0)
    {
        write_at(buff_pos, buff, buff_len);
        buff_pos += buff_len;
        buff_len = 0;
    }
}

static void buf_at(_long pos)
{
    if(buff_len > 0 && buff_pos + buff_len != pos)
    {
        buf_flush();
    }
    if(buff_len == 0)
    {
        buff_pos = pos;
    }
}

static void buf_put(_long pos, const char *data, _long len)
{
    buf_at(pos);
    memcpy(buff + buff_len, data, (size_t)len);
    buff_len += len;
    if(buff_len >= BUFFSZ)
    {
        buf_flush();
    }
}

static void buf_fill(_long pos, _long len)
{
    while(len > 0)
    {
        _long n;

        buf_at(pos);
        n = BUFFSZ - buff_len;
        if(n > len)
        {
            n = len;
        }

        memset(buff + buff_len, fill_byte, (size_t)n);
        buff_len += n;
        pos += n;
        len -= n;

        if(buff_len >= BUFFSZ)
        {
            buf_flush();
        }
    }
}

/**
 *  Source unreadable at [pos, pos+len): normally filled with the fill byte.
 *  In differential mode the destination is an older copy, so it keeps its
 *  previous content there; only the part beyond its old end gets filled
 *  (there is nothing to keep, and the output must stay the full size).
 */
static void mark_unreadable(_long pos, _long len)
{
    _long keep_end = pos;

    if(g_differential && g_dst_old_size > pos)
    {
        keep_end = pos + len;
        if(keep_end > g_dst_old_size)
        {
            keep_end = g_dst_old_size;
        }
    }

    if(pos + len > keep_end)
    {
        buf_fill(keep_end, pos + len - keep_end);
    }
}

/** How unreadable areas look in the destination, for the report and the map file **/
static void unreadable_note(char *out, size_t outsz)
{
    if(g_differential)
    {
        snprintf(out, outsz, "previous destination content kept, 0x%02X beyond its old end",
                 (unsigned int)(unsigned char)fill_byte);
    }
    else
    {
        snprintf(out, outsz, "filled with 0x%02X", (unsigned int)(unsigned char)fill_byte);
    }
}

/* ------------------------------------------------------------------ */
/*  State map                                                          */
/* ------------------------------------------------------------------ */

/** Append [start, end) (callers add ranges in ascending order), merge with the last one if possible **/
static void map_add(badmap_t *m, _long start, _long end, int state)
{
    if(end <= start)
    {
        return;
    }

    if(m->n > 0)
    {
        range_t *last = &m->r[m->n - 1];
        if(last->end == start && last->state == state)
        {
            last->end = end;
            return;
        }
    }

    if(m->n == m->cap)
    {
        size_t ncap = (m->cap > 0) ? m->cap * 2 : 64;
        range_t *nr = realloc(m->r, ncap * sizeof(range_t));
        if(nr == NULL)
        {
            die("Out of memory (map)");
        }
        m->r = nr;
        m->cap = ncap;
    }

    m->r[m->n].start = start;
    m->r[m->n].end = end;
    m->r[m->n].state = state;
    m->n++;
}

static void map_free(badmap_t *m)
{
    free(m->r);
    m->r = NULL;
    m->n = 0;
    m->cap = 0;
}

/** Bytes in ranges of this state (0 = any state) from position `from` on **/
static _long map_bytes(const badmap_t *m, int state, _long from)
{
    _long total = 0;
    size_t i;
    for(i = 0; i < m->n; i++)
    {
        if(state == 0 || m->r[i].state == state)
        {
            _long a = (m->r[i].start > from) ? m->r[i].start : from;
            if(m->r[i].end > a)
            {
                total += m->r[i].end - a;
            }
        }
    }
    return total;
}

/** Bytes that are filled instead of real data (bad + skipped) **/
static _long map_unreadable(const badmap_t *m)
{
    return map_bytes(m, RS_BAD, 0) + map_bytes(m, RS_SKIPPED, 0);
}

static char state_char(int state)
{
    if(state == RS_BAD)
    {
        return '-';
    }
    if(state == RS_SKIPPED)
    {
        return '*';
    }
    return '?';
}

/**
 *  Next unreadable area for the report: touching bad and skipped ranges are
 *  merged into one (the map file keeps them separate). Returns false at the end.
 */
static bool next_area(const badmap_t *m, size_t *i, _long *a, _long *b, _long *skipped)
{
    while(*i < m->n && m->r[*i].state == RS_NONTRIED)
    {
        (*i)++;
    }

    if(*i >= m->n)
    {
        return false;
    }

    *a = m->r[*i].start;
    *b = m->r[*i].end;
    *skipped = (m->r[*i].state == RS_SKIPPED) ? (*b - *a) : 0;
    (*i)++;

    while(*i < m->n && m->r[*i].state != RS_NONTRIED && m->r[*i].start == *b)
    {
        if(m->r[*i].state == RS_SKIPPED)
        {
            *skipped += m->r[*i].end - m->r[*i].start;
        }
        *b = m->r[*i].end;
        (*i)++;
    }
    return true;
}

static void print_map(const badmap_t *m)
{
    size_t i = 0;
    size_t shown = 0;
    size_t total = 0;
    _long a;
    _long b;
    _long skipped;
    _long nontried = map_bytes(m, RS_NONTRIED, 0);

    while(next_area(m, &i, &a, &b, &skipped))
    {
        total++;
    }

    if(total == 0)
    {
        fprintf(stdout, "\nNo unreadable sectors.\n");
    }
    else
    {
        char note[96];

        unreadable_note(note, sizeof(note));
        fprintf(stdout, "\n\033[1mUnreadable ranges (%s):\033[m\n", note);
        fprintf(stdout, "  %13s   %13s   %s\n", "From", "To", "Bytes");

        i = 0;
        while(shown < MAX_LIST_RANGES && next_area(m, &i, &a, &b, &skipped))
        {
            if(skipped > 0)
            {
                fprintf(stdout, "  %13" PRId64 " - %13" PRId64 "   %" PRId64 "  (%" PRId64 " bytes not tried yet)\n",
                        a, b, b - a, skipped);
            }
            else
            {
                fprintf(stdout, "  %13" PRId64 " - %13" PRId64 "   %" PRId64 "\n", a, b, b - a);
            }
            shown++;
        }

        if(total > shown)
        {
            fprintf(stdout, "  ... and %lu more ranges (see the map file)\n", (unsigned long)(total - shown));
        }
    }

    if(nontried > 0)
    {
        fprintf(stdout, "Not copied yet: %" PRId64 " bytes\n", nontried);
    }
}

/* ------------------------------------------------------------------ */
/*  Map file (GNU ddrescue mapfile layout, CRLF line endings)          */
/* ------------------------------------------------------------------ */

static void timestamp(char *out, size_t outsz, const char *fmt)
{
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);

    if(tmv == NULL || strftime(out, outsz, fmt, tmv) == 0)
    {
        snprintf(out, outsz, "unknown");
    }
}

/** pfr_<destination name>_<YYYYMMDD-HHMMSS>.map in the current directory **/
static void make_map_path(const char *dst_path)
{
    char stamp[32];
    char name[128];
    const char *base = dst_path;
    const char *p;
    size_t n = 0;

    for(p = dst_path; *p; p++)
    {
        if(*p == '/' || *p == '\\')
        {
            base = p + 1;
        }
    }

    /* keep only characters that are safe in a file name on every OS */
    for(p = base; *p && n < sizeof(name) - 1; p++)
    {
        if(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_')
        {
            name[n] = *p;
            n++;
        }
    }
    name[n] = 0;

    if(n == 0)
    {
        strcpy(name, "output");
    }

    timestamp(stamp, sizeof(stamp), "%Y%m%d-%H%M%S");
    snprintf(map_path, sizeof(map_path), "pfr_%s_%s.map", name, stamp);
}

/** Collects consecutive blocks with the same status into one line **/
typedef struct
{
    FILE *f;
    _long pos;
    _long size;
    char st;
} mapw_t;

static void mapw_flush(mapw_t *w)
{
    if(w->size > 0)
    {
        fprintf(w->f, "0x%08" PRIX64 "  0x%08" PRIX64 "  %c\r\n", w->pos, w->size, w->st);
        w->size = 0;
    }
}

static void mapw_emit(mapw_t *w, _long start, _long end, char st)
{
    if(end <= start)
    {
        return;
    }

    if(w->size > 0 && w->st == st && w->pos + w->size == start)
    {
        w->size += end - start;
        return;
    }

    mapw_flush(w);
    w->pos = start;
    w->size = end - start;
    w->st = st;
}

static bool replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

/**
 *  Header: start time, command line, current time, result (all "#" comments),
 *  then an empty line (CRLFCRLF), then the map covering 0..end position:
 *    +  copied   -  bad sector (filled)   *  skipped area (filled)   ?  not copied yet
 *  Written to a temporary file first and then renamed, so a crash never leaves a broken map.
 */
static bool write_mapfile(const badmap_t *map, _long cur_pos, char cur_status, const char *result)
{
    FILE *f;
    char now[32];
    char tmp_path[320];
    int i;
    size_t k;
    _long cur;
    bool ok;
    mapw_t w;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", map_path);
    f = fopen(tmp_path, "wb");
    if(f == NULL)
    {
        return false;
    }

    timestamp(now, sizeof(now), "%Y-%m-%d %H:%M:%S");

    fprintf(f, "# Start time:   %s\r\n", start_time);
    fprintf(f, "# Command line:");
    for(i = 0; i < g_argc; i++)
    {
        if(strchr(g_argv[i], ' ') != NULL)
        {
            fprintf(f, " \"%s\"", g_argv[i]);
        }
        else
        {
            fprintf(f, " %s", g_argv[i]);
        }
    }
    fprintf(f, "\r\n");
    fprintf(f, "# Current time: %s\r\n", now);
    fprintf(f, "# Mapfile. Created by [P]artial [F]ile [R]ecovery version %s\r\n", VERSION);
    fprintf(f, "# Result: %s\r\n", result);
    fprintf(f, "# Read OK: %" PRId64 " bytes, Unreadable: %" PRId64 " bytes, Not copied yet: %" PRId64 " bytes, Rescued by Pass 2/3: %" PRId64 " bytes, Failed reads: %" PRId64 "\r\n",
            st_acquired, map_unreadable(map), map_bytes(map, RS_NONTRIED, 0), st_recovered, st_read_errors);
    fprintf(f, "# Sector size: %" PRId64 ", fill: 0x%02X, status: + copied, - bad sector, * skipped around damage, ? not copied yet\r\n",
            unit, (unsigned int)(unsigned char)fill_byte);
    {
        char note[96];
        unreadable_note(note, sizeof(note));
        fprintf(f, "# Unreadable areas (- and *): %s\r\n", note);
    }
    fprintf(f, "\r\n");

    fprintf(f, "# current_pos  current_status  current_pass\r\n");
    fprintf(f, "0x%08" PRIX64 "     %c               %d\r\n", cur_pos, cur_status, phase);
    fprintf(f, "#      pos        size  status\r\n");

    w.f = f;
    w.pos = 0;
    w.size = 0;
    w.st = 0;
    cur = 0;

    for(k = 0; k < map->n; k++)
    {
        mapw_emit(&w, cur, map->r[k].start, '+');
        mapw_emit(&w, map->r[k].start, map->r[k].end, state_char(map->r[k].state));
        cur = map->r[k].end;
    }
    mapw_emit(&w, cur, src_size, '+');
    mapw_flush(&w);

    ok = (fflush(f) == 0);
    if(fclose(f) != 0)
    {
        ok = false;
    }

    if(!ok || !replace_file(tmp_path, map_path))
    {
        remove(tmp_path);
        return false;
    }
    return true;
}

/**
 *  Load a map file (written by pfr or by GNU ddrescue).
 *  '+' copied, '-' bad, '*' or '/' to be scraped, '?' not copied yet; gaps count as '?'.
 *  Returns false on error, *extent = end of the last block.
 */
static bool load_mapfile(const char *path, badmap_t *m, _long *extent)
{
    FILE *f;
    char line[512];
    _long last_end = 0;
    int lineno = 0;

    f = fopen(path, "rb");
    if(f == NULL)
    {
        fprintf(stderr, "Unable to open map file: %s\n\n", path);
        return false;
    }

    while(fgets(line, sizeof(line), f) != NULL)
    {
        char t1[64];
        char t2[64];
        char t3[64];
        char *s = line;
        char *e1;
        char *e2;
        int nt;
        int state;
        _long pos;
        _long size;

        lineno++;

        while(*s == ' ' || *s == '\t')
        {
            s++;
        }

        if(*s == '#' || *s == '\r' || *s == '\n' || *s == 0)
        {
            continue;
        }

        nt = sscanf(s, "%63s %63s %63s", t1, t2, t3);

        /* status line: current_pos  current_status  [current_pass] */
        if(nt >= 2 && strlen(t2) == 1 && !isdigit((unsigned char)t2[0]))
        {
            continue;
        }

        if(nt != 3 || strlen(t3) != 1)
        {
            fprintf(stderr, "Map file %s, line %d: expected \"pos size status\"\n\n", path, lineno);
            fclose(f);
            return false;
        }

        pos = (_long)strtoll(t1, &e1, 0);
        size = (_long)strtoll(t2, &e2, 0);
        if(*e1 != 0 || *e2 != 0 || pos < 0 || size < 0)
        {
            fprintf(stderr, "Map file %s, line %d: invalid number\n\n", path, lineno);
            fclose(f);
            return false;
        }

        if(pos < last_end)
        {
            fprintf(stderr, "Map file %s, line %d: blocks overlap or are not sorted\n\n", path, lineno);
            fclose(f);
            return false;
        }

        switch(t3[0])
        {
            case '+':
            {
                state = 0;
                break;
            }
            case '-':
            {
                state = RS_BAD;
                break;
            }
            case '*':
            case '/':
            {
                state = RS_SKIPPED;
                break;
            }
            case '?':
            {
                state = RS_NONTRIED;
                break;
            }
            default:
            {
                fprintf(stderr, "Map file %s, line %d: unknown status '%c'\n\n", path, lineno, t3[0]);
                fclose(f);
                return false;
            }
        }

        map_add(m, last_end, pos, RS_NONTRIED);    /* a gap is not known to be copied */
        if(state != 0)
        {
            map_add(m, pos, pos + size, state);
        }
        last_end = pos + size;
    }

    fclose(f);
    *extent = last_end;
    return true;
}

/** Fit a loaded map to the current source range [0, end) **/
static void map_fit(badmap_t *m, _long extent, _long end)
{
    badmap_t out = { NULL, 0, 0 };
    size_t i;

    for(i = 0; i < m->n; i++)
    {
        if(m->r[i].start >= end)
        {
            break;
        }
        map_add(&out, m->r[i].start, (m->r[i].end < end) ? m->r[i].end : end, m->r[i].state);
    }

    if(extent < end)
    {
        map_add(&out, extent, end, RS_NONTRIED);
    }

    map_free(m);
    *m = out;
}

/* ------------------------------------------------------------------ */
/*  Checkpoints                                                        */
/* ------------------------------------------------------------------ */

static char running_status(void)
{
    if(phase == PHASE_COPY)
    {
        return '?';
    }
    if(phase == PHASE_SCRAPE)
    {
        return '/';
    }
    return '-';
}

/**
 *  Called often from the passes with the first unfinished position of the
 *  current range. Every CKPT_MS it syncs the destination and saves the map:
 *  finished part of the pass + unfinished rest of the current range + ranges not reached yet.
 */
static void checkpoint_tick(_long pos)
{
    badmap_t snap = { NULL, 0, 0 };
    size_t j;
    char result[64];
    uint64_t t;

    g_ckpt_pos = pos;
    t = now_ms();

    if(g_pass_out == NULL || t - g_ckpt_last < CKPT_MS)
    {
        return;
    }
    g_ckpt_last = t;

    buf_flush();
    sync_dst();

    for(j = 0; j < g_pass_out->n; j++)
    {
        map_add(&snap, g_pass_out->r[j].start, g_pass_out->r[j].end, g_pass_out->r[j].state);
    }
    map_add(&snap, g_ckpt_pos, g_ckpt_end, g_ckpt_state);
    for(j = g_pass_idx + 1; j < g_pass_src->n; j++)
    {
        map_add(&snap, g_pass_src->r[j].start, g_pass_src->r[j].end, g_pass_src->r[j].state);
    }

    snprintf(result, sizeof(result), "Running Pass %d (checkpoint)", phase);
    if(!write_mapfile(&snap, g_ckpt_pos, running_status(), result))
    {
        fprintf(stderr, "\033[93mWarning: unable to save map file %s\033[m\n", map_path);
    }
    map_free(&snap);
}

/* ------------------------------------------------------------------ */
/*  Status line                                                        */
/* ------------------------------------------------------------------ */

static void show_status(const char *op, _long pos, bool show_speed, bool force)
{
    time_t now = time(NULL);
    float progress;

    if(!force && now == st_last)
    {
        return;
    }
    st_last = now;

    if(st_lines % 20 == 0)
    {
        fprintf(stderr, "\033[7mOperation    | Current Block | Round Left | Lost Blocks |  Recovered  | Written Bytes | Acquired Blocks | Progress, Speed\033[m\n");
    }
    st_lines++;

    if(sp_time == 0)
    {
        sp_time = now;
        sp_pos = pos;
        sp_mbps = 0;
    }
    else if(now > sp_time && pos > sp_pos)
    {
        sp_mbps = (int)(((pos - sp_pos) / (_long)(now - sp_time)) / 1048576);
        sp_time = now;
        sp_pos = pos;
    }

    progress = (float)(((double)pos * 100.0) / (double)src_size);

    if(show_speed)
    {
        fprintf(stdout, "%-12s | %13" PRId64 " | %10" PRId64 " | %11" PRId64 " | %11" PRId64 " | %13" PRId64 " |  %13" PRId64 "  |  %.2f, %d MB/s\n",
                op, pos, g_rounds_left, st_lost, st_recovered, st_written, st_acquired, progress, sp_mbps);
    }
    else
    {
        fprintf(stdout, "%-12s | %13" PRId64 " | %10" PRId64 " | %11" PRId64 " | %11" PRId64 " | %13" PRId64 " |  %13" PRId64 "  |  %.2f, -- MB/s\n",
                op, pos, g_rounds_left, st_lost, st_recovered, st_written, st_acquired, progress);
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/*  Recovery passes                                                    */
/* ------------------------------------------------------------------ */

/** Length of the sector at p, clipped to end (first one is shorter when p is not aligned) **/
static _long sector_len(_long p, _long end)
{
    _long n = unit - (p % unit);
    if(n > end - p)
    {
        n = end - p;
    }
    return n;
}

/** One read attempt of a filled sector (Pass 2 / Pass 3); on success it is written **/
static bool try_sector(_long p, _long len)
{
    if(read_at(p, tmp, len) == len)
    {
        write_at(p, tmp, len);
        st_acquired += len;
        st_lost -= len;
        st_recovered += len;
        return true;
    }
    return false;
}

/**
 *  Run one pass over every range of the given state at or after seek.
 *  fn() handles [a, b) and appends what is still missing to `out`;
 *  all other ranges are carried over unchanged.
 */
static void run_pass(badmap_t *map, int state, pass_fn fn)
{
    badmap_t out = { NULL, 0, 0 };
    size_t i;

    g_pass_out = &out;
    g_pass_src = map;

    for(i = 0; i < map->n; i++)
    {
        range_t r = map->r[i];

        if(r.state != state || stop_req || r.end <= g_seek)
        {
            map_add(&out, r.start, r.end, r.state);
            continue;
        }

        if(r.start < g_seek)
        {
            map_add(&out, r.start, g_seek, r.state);
            r.start = g_seek;
        }

        g_pass_idx = i;
        g_ckpt_pos = r.start;
        g_ckpt_end = r.end;
        g_ckpt_state = r.state;

        fn(r.start, r.end, &out);
    }

    g_pass_out = NULL;
    g_pass_src = NULL;
    map_free(map);
    *map = out;
}

/**
 *  Pass 1 for one "not copied yet" range: copy everything that reads
 *  without trouble, one attempt per sector, jump over damaged areas.
 */
static void copy_range(_long a, _long b, badmap_t *out)
{
    _long pos = a;
    bool fast = true;
    _long clean = 0;
    int consec = 0;

    while(pos < b)
    {
        if(stop_req)
        {
            break;
        }

        checkpoint_tick(pos);

        if(fast)
        {
            _long rlen = b - pos;
            _long got;
            uint64_t t0;

            if(rlen > BUFFSZ)
            {
                rlen = BUFFSZ;
            }

            buf_flush();
            show_status("Fast Forward", pos, true, false);

            t0 = now_ms();
            got = read_at(pos, tmp, rlen);

            if(got == rlen)
            {
                bool skip_write = false;

                if(g_differential)
                {
/* A 4yr old's contribution
0
 ,űŰ
-> mn;íł v5tcnchfcf vhb7z7hmi ,m,kl nl.é.
*/
                    skip_write = same_as_dst(pos, tmp, rlen);
                }

                if(!skip_write)
                {
                    write_at(pos, tmp, rlen);
                }

                st_acquired += rlen;
                pos += rlen;

                /* Throttle: hold each chunk to slimit MB/s, counting the time the read already took */
                if(g_slimit > 0)
                {
                    uint64_t expected_ms = ((uint64_t)rlen * 1000ULL) / ((uint64_t)g_slimit * 1048576ULL);
                    uint64_t spent_ms = now_ms() - t0;
                    if(expected_ms > spent_ms)
                    {
                        sleep_ms(expected_ms - spent_ms);
                    }
                }
                continue;
            }

            /* Partial read: keep the good part up to the last whole sector boundary */
            {
                _long endp = pos + got;
                endp -= endp % unit;
                if(endp > pos)
                {
                    write_at(pos, tmp, endp - pos);
                    st_acquired += endp - pos;
                    pos = endp;
                }
            }

            fast = false;
            clean = 0;
            consec = 0;
            continue;
        }

        /* Block Mining: one sector, one attempt */
        {
            _long want = sector_len(pos, b);

            show_status("Block Mining", pos, true, false);

            if(read_at(pos, tmp, want) == want)
            {
                buf_put(pos, tmp, want);
                st_acquired += want;
                pos += want;
                clean += want;
                consec = 0;

                if(clean >= CLEAN_BYTES)
                {
                    fast = true;
                }
            }
            else
            {
                mark_unreadable(pos, want);
                map_add(out, pos, pos + want, RS_BAD);
                st_lost += want;
                pos += want;
                clean = 0;
                consec++;

                /* Bad sectors in a row: probably a damaged area, jump over it and come back later */
                if(consec >= 2 && pos < b)
                {
                    int shift = consec - 1;
                    _long jump;

                    if(shift > MAX_JUMP_SHIFT)
                    {
                        shift = MAX_JUMP_SHIFT;
                    }

                    jump = unit << shift;
                    jump -= (pos + jump) % unit;     /* land on a sector boundary */
                    if(jump > b - pos)
                    {
                        jump = b - pos;
                    }

                    if(jump > 0)
                    {
                        mark_unreadable(pos, jump);
                        map_add(out, pos, pos + jump, RS_SKIPPED);
                        st_lost += jump;
                        pos += jump;
                    }
                }
            }
        }
    }

    buf_flush();
    map_add(out, pos, b, RS_NONTRIED);    /* interrupted: the rest stays not copied */
    g_ckpt_pos = pos;
}

/**
 *  Pass 2 for one skipped range: trim forward until the first error,
 *  trim backward until the first error, then read the middle once.
 */
static void scrape_range(_long s, _long e, badmap_t *out)
{
    _long p = s;
    _long q = e;
    _long tail_start = -1;
    _long tail_end = -1;

    /* Trim forward */
    while(p < q && !stop_req)
    {
        _long n = sector_len(p, q);

        checkpoint_tick(p);
        show_status("Trimming", p, false, false);

        if(try_sector(p, n))
        {
            p += n;
        }
        else
        {
            map_add(out, p, p + n, RS_BAD);
            p += n;
            break;
        }
    }

    /* Trim backward (the unfinished part still starts at p for the checkpoint) */
    while(q > p && !stop_req)
    {
        _long qs = (q - 1) - ((q - 1) % unit);

        if(qs < p)
        {
            qs = p;
        }

        checkpoint_tick(p);
        show_status("Trimming", qs, false, false);

        if(try_sector(qs, q - qs))
        {
            q = qs;
        }
        else
        {
            tail_start = qs;
            tail_end = q;
            q = qs;
            break;
        }
    }

    /* Scrape the middle, one attempt per sector */
    while(p < q)
    {
        _long n;

        if(stop_req)
        {
            map_add(out, p, q, RS_SKIPPED);
            break;
        }

        n = sector_len(p, q);
        checkpoint_tick(p);
        show_status("Scraping", p, false, false);

        if(!try_sector(p, n))
        {
            map_add(out, p, p + n, RS_BAD);
        }
        p += n;
    }

    if(tail_start >= 0)
    {
        map_add(out, tail_start, tail_end, RS_BAD);
    }
}

/** Pass 3: one more attempt on every sector of a bad range **/
static void retry_range(_long a, _long b, badmap_t *out)
{
    _long p = a;

    while(p < b)
    {
        _long n;

        if(stop_req)
        {
            map_add(out, p, b, RS_BAD);
            break;
        }

        n = sector_len(p, b);
        checkpoint_tick(p);
        show_status("Retrying", p, false, false);

        if(!try_sector(p, n))
        {
            map_add(out, p, p + n, RS_BAD);
        }
        p += n;
    }
}

/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "\033[96mThis software is intended to rescue data. It allows you to ignore and skip hard drive errors, and continue the operation with *possible* data loss.\n");
    fprintf(stderr, "Even the most secure filesystems like ZFS can suffer data corruption. This software retrieves the data regardless of data loss, with many additional options.\n");
    fprintf(stderr, "In our research, we concluded that restoring files or block devices with some data loss does not mean the data is corrupted or unusable.\n\n");
    fprintf(stderr, "To spare a failing drive, all readable data is copied first; bad areas are skipped, noted in a map and retried only at the end.\n");
    fprintf(stderr, "The map is saved to a file regularly, an interrupted or crashed run continues from it with --map.\n");
    fprintf(stderr, "Since version 2.0 there is a differential method to minimize writes. This is handy when writing is slower than reading twice.\n");
    fprintf(stderr, "It can also restore old snapshots or differential clones back onto themselves.\n");
    fprintf(stderr, "Author: Viktor Hlavaji aka DaVieS, davies@npulse.net | Licensed under: GNU/GPLv3\033[m\n\n");

    fprintf(stderr, "\033[93mPlease make a backup and test before you start any live operation. This software comes without any warranty.\033[m\n\n");

    fprintf(stderr, "\033[1mUsage: %s [--map mapfile] [source_file] [dest_file] [differential = 0] [throttle = 0] [try = 1] [fill = 00] [skip = 512] [seek = 0] [endpos = 0]\033[m\n", prog);
    fprintf(stderr, "\033[1m       %s --list              (Windows: list physical drives and volumes)\033[m\n\n", prog);
    fprintf(stderr, "\033[93m - --map: (Optional) continue from a map file of an earlier run (pfr or GNU ddrescue format)\n");
    fprintf(stderr, "   | - copied areas are skipped, the rest is copied, scraped and retried; the destination must be the same\n");
    fprintf(stderr, " - source_file: The file or device to recover\n");
    fprintf(stderr, " - dest_file: Output file or device written by this software\n");
    fprintf(stderr, " - differential: (Optional) 0 = no, 1 = yes\n");
    fprintf(stderr, "   | - scan both input & output and overwrite only the differences, handy when writes are expensive\n");
    fprintf(stderr, " - throttle: (Optional) 0 = no limit, otherwise limit read speed to [ x ] MB/s\n");
    fprintf(stderr, " - try: (Optional) read attempts per bad sector, the extra attempts happen at the end, after all readable data is copied\n");
    fprintf(stderr, " - fill: (Optional) unrecovered bytes are replaced by hex code 0x[XX]\n");
    fprintf(stderr, " - skip: (Optional) sector size: bad areas are read and skipped in sectors of (x) bytes, should match the sector or block size\n");
    fprintf(stderr, " - seek: (Optional) start reading from this byte position\n");
    fprintf(stderr, " - endpos: (Optional) stop reading at this byte position\033[m\n\n");

    fprintf(stderr, "\033[1m[Example Software Output]\033[m\n");
    fprintf(stderr, "Operation    | Current Block | Round Left | Lost Blocks |  Recovered  | Written Bytes | Acquired Blocks | Progress, Speed\n");
    fprintf(stderr, "Fast Forward |             0 |          0 |           0 |           0 |             0 |              0  |  0.00, 0 MB/s\n");
    fprintf(stderr, "Fast Forward |     230686720 |          0 |           0 |           0 |     230686720 |      230686720  |  11.59, 220 MB/s\n\n");

    fprintf(stderr, "\033[1m[Examples to acquire a damaged file]\033[m\n");
    fprintf(stderr, "To rescue a single file: %s ./broken_file.mp3 ./broken_file_fix.mp3\n", prog);
    fprintf(stderr, "To rescue a MySQL ibd: %s ./important_tbl.ibd ./important_tbl.ibd.bck\n", prog);
    fprintf(stderr, "To rescue a text file: %s ./mail.txt ./mail.txt.bck 0 0 1 20\n", prog);

    fprintf(stderr, "\n\033[1m[Examples to rescue a whole block device]\033[m\n");
    fprintf(stderr, "To rescue a 4K SATA bad drive: %s /dev/sda /dev/sdb 0 0 1 00 4096\n", prog);
    fprintf(stderr, "To rescue a 512B SATA partition, 3 tries per bad sector: %s /dev/sda1 /dev/sdb1 0 0 3 00 512\n", prog);
    fprintf(stderr, "To rescue a 512B SATA partition limited to 22MB/s: %s /dev/sda1 /dev/sdb1 0 22 1 00 512\n", prog);
    fprintf(stderr, "To continue an interrupted run and retry its bad sectors 3 times: %s --map pfr_sdb_20261110-194523.map /dev/sda /dev/sdb 0 0 3\n", prog);

#ifdef _WIN32
    fprintf(stderr, "\n\033[1m[Examples for Windows drives (run as Administrator, use --list to find the drive)]\033[m\n");
    fprintf(stderr, "Write ISO to USB pendrive: %s C:\\image.iso \\\\.\\PhysicalDrive1 0 0 1 00 512\n", prog);
    fprintf(stderr, "Clone partition E: to file: %s \\\\.\\E: D:\\backup.img 0 0 1 00 512\n", prog);
    fprintf(stderr, "Restore image to drive 2:  %s D:\\backup.img \\\\.\\PhysicalDrive2 0 0 1 00 4096\n", prog);
    fprintf(stderr, "Note: with drives, skip / seek / endpos must be multiples of the sector size.\n");
#endif

    fprintf(stderr, "\n\033[1m[Example of differential method]\033[m\n");
    fprintf(stderr, "To copy only the changes (differential copy) from file1 into file2: %s ./file1.img.old ./file2.img 1\n", prog);
}

int main(int argc, char *argv[])
{
    _long dst_size = -1;
    _long retries = 1;
    _long limit = 0;
    _long seek = 0;
    _long round;
    char fill[3];
    badmap_t map = { NULL, 0, 0 };
    const char *map_in = NULL;
    char **pargv;
    int pargc = 0;
    int i;
    int io_fail;

    g_argc = argc;
    g_argv = argv;
    strcpy(fill, "00");
    signal(SIGINT, signalHandler);
    timestamp(start_time, sizeof(start_time), "%Y-%m-%d %H:%M:%S");

    /** ENABLE COLORED OUTPUT ON WINDOWS **/
#ifdef _WIN32
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if(hOut != INVALID_HANDLE_VALUE)
        {
            DWORD dwMode = 0;
            if(GetConsoleMode(hOut, &dwMode))
            {
                dwMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(hOut, dwMode);
            }
        }
    }
#endif

    fprintf(stderr, "\n\n\033[7m[P]artial [F]ile [R]ecovery Version: %s\033[m\n", VERSION);

    /* Take out "--map file", the rest are positional arguments */
    pargv = malloc(sizeof(char *) * (size_t)(argc + 1));
    if(pargv == NULL)
    {
        die("Out of memory");
    }

    for(i = 0; i < argc; i++)
    {
        if(i > 0 && !strcmp(argv[i], "--map"))
        {
            if(i + 1 >= argc)
            {
                fprintf(stderr, "Invalid Argument: --map needs a file name\n");
                return EXIT_FAIL;
            }
            map_in = argv[i + 1];
            i++;
            continue;
        }
        pargv[pargc] = argv[i];
        pargc++;
    }
    pargv[pargc] = NULL;

    /* --list / -l : enumerate drives (Windows only) */
    if(pargc >= 2 && (!strcmp(pargv[1], "--list") || !strcmp(pargv[1], "-l")))
    {
#ifdef _WIN32
        list_drives();
        return EXIT_OK;
#else
        fprintf(stderr, "\nDrive listing is available on Windows only.  On Linux use: lsblk\n\n");
        return EXIT_FAIL;
#endif
    }

    if(pargc < 3)
    {
        usage(pargv[0]);
        return EXIT_FAIL;
    }

    /* -------------------------------------------------------------- */
    /*  Parse optional arguments                                       */
    /* -------------------------------------------------------------- */

    if(pargc >= 10)
    {
        limit = (_long)strtoull(pargv[9], NULL, 10);
    }

    if(pargc >= 9)
    {
        seek = (_long)strtoull(pargv[8], NULL, 10);
    }

    if(pargc >= 8)
    {
        unit = atoi(pargv[7]);
        if(unit <= 0 || unit > BUFFSZ)
        {
            fprintf(stderr, "Invalid Argument: skip must be between 1 and %d\n", BUFFSZ);
            return EXIT_FAIL;
        }
    }

    if(pargc >= 7)
    {
        int fv;

        if(strlen(pargv[6]) != 2 || !isxdigit((unsigned char)pargv[6][0]) || !isxdigit((unsigned char)pargv[6][1]))
        {
            fprintf(stderr, "Invalid Argument: fill must be two hex digits (00..FF)\n");
            return EXIT_FAIL;
        }

        fv = (int)strtol(pargv[6], NULL, 16);
        fill_byte = (char)fv;
        snprintf(fill, sizeof(fill), "%02X", fv);
    }

    if(pargc >= 6)
    {
        retries = atoi(pargv[5]);
        if(retries <= 0)
        {
            retries = 1;
        }
    }

    if(pargc >= 5)
    {
        g_slimit = atoi(pargv[4]);
        if(g_slimit <= 0)
        {
            g_slimit = 0;
        }
    }

    if(pargc >= 4)
    {
        if(atoi(pargv[3]) > 0)
        {
            g_differential = true;
            fprintf(stderr, "\033[96mContinuing in Differential Mode: source and destination are both scanned and only the differing blocks are overwritten\n");
            fprintf(stderr, "Where the source is unreadable, the destination keeps its previous content\033[m\n");
        }
    }

    if(!g_differential)
    {
        fprintf(stderr, "\033[1mContinuing in Normal Mode: source is scanned and written to the destination immediately\033[m\n");
    }

    /* -------------------------------------------------------------- */
    /*  Open source                                                    */
    /* -------------------------------------------------------------- */

    src_path = pargv[1];

#ifdef _WIN32
    src_is_device = is_device_path(pargv[1]);
    if(src_is_device)
    {
        src = open_device(pargv[1], 0);
    }
    else
    {
        src = fopen(pargv[1], "rb");
    }
#else
    src = fopen(pargv[1], "rb");
#endif

    if(src == NULL)
    {
        fprintf(stderr, "Unable to open source file for reading: %s\n\n", pargv[1]);
        if(src_is_device)
        {
            fprintf(stderr, "Hint: drive access requires Administrator privileges\n\n");
        }
        return EXIT_FAIL;
    }

#ifdef _WIN32
    if(src_is_device)
    {
        src_size = device_size(src);
        if(src_size <= 0)
        {
            fprintf(stderr, "Unable to determine device size: %s\n\n", pargv[1]);
            fclose(src);
            return EXIT_FAIL;
        }
        if(device_sector_size(src) > align)
        {
            align = device_sector_size(src);
        }
    }
#endif

    if(!src_is_device)
    {
        if(FSEEK(src, 0, SEEK_END) != 0)
        {
            fprintf(stderr, "File is unrecoverable, seek failed: %s\n\n", pargv[1]);
            fclose(src);
            return EXIT_FAIL;
        }
        src_size = FTELL(src);
        if(src_size <= 0)
        {
            fprintf(stderr, "File is unrecoverable, seek failed: %s\n\n", pargv[1]);
            fclose(src);
            return EXIT_FAIL;
        }
    }

    /* Validate seek / endpos now that src_size is known */
    if(seek >= src_size)
    {
        fprintf(stderr, "Invalid seek: %" PRId64 " (source size %" PRId64 ")\n", seek, src_size);
        fclose(src);
        return EXIT_FAIL;
    }

    if(limit > 0)
    {
        if(limit > src_size || limit <= seek)
        {
            fprintf(stderr, "Invalid endpos (source size %" PRId64 ")\n", src_size);
            fclose(src);
            return EXIT_FAIL;
        }
        src_size = limit;
    }

    /* -------------------------------------------------------------- */
    /*  State map: from --map, or everything "not copied yet"          */
    /* -------------------------------------------------------------- */

    if(map_in != NULL)
    {
        _long extent = 0;

        if(!load_mapfile(map_in, &map, &extent))
        {
            fclose(src);
            return EXIT_FAIL;
        }

        if(extent != src_size)
        {
            fprintf(stderr, "\033[93mWarning: the map covers %" PRId64 " bytes, the source range is %" PRId64 " bytes\033[m\n", extent, src_size);
        }

        map_fit(&map, extent, src_size);
        st_lost = map_unreadable(&map);
    }
    else
    {
        map_add(&map, 0, src_size, RS_NONTRIED);
    }

    /* -------------------------------------------------------------- */
    /*  Open destination                                               */
    /* -------------------------------------------------------------- */

#ifdef _WIN32
    dst_is_device = is_device_path(pargv[2]);
    if(dst_is_device)
    {
        dst = open_device(pargv[2], 1);
        if(dst == NULL)
        {
            fprintf(stderr, "Unable to open destination device for writing: %s\n", pargv[2]);
            fprintf(stderr, "Hint: drive access requires Administrator privileges\n\n");
            fclose(src);
            return EXIT_FAIL;
        }

        dst_raw = true;
        if(device_sector_size(dst) > align)
        {
            align = device_sector_size(dst);
        }
        dst_size = device_size(dst);
    }
#endif

    if(!dst_is_device)
    {
        dst = fopen(pargv[2], "r+b");
        if(dst == NULL)
        {
            if(g_differential)
            {
                fprintf(stderr, "Unable to open destination file for differential writing: %s\n\n", pargv[2]);
                fclose(src);
                return EXIT_FAIL;
            }

            if(map_in != NULL)
            {
                fprintf(stderr, "Unable to open destination file: %s\nWith --map the destination of the earlier run is needed, it holds the copied data.\n\n", pargv[2]);
                fclose(src);
                return EXIT_FAIL;
            }

            dst = fopen(pargv[2], "wb");
            if(dst == NULL)
            {
                fprintf(stderr, "Unable to open destination file for writing: %s\n\n", pargv[2]);
                fclose(src);
                return EXIT_FAIL;
            }
        }
    }

#if PFR_TEST_ALIGN > 0
    align = PFR_TEST_ALIGN;
    dst_raw = true;
#endif

    /* Raw devices only accept whole, sector aligned reads and writes */
    if(align > 1)
    {
        const char *bad = NULL;

        /* endpos may be odd only when the source is a file (the tail gets padded) */
        if(unit % align)
        {
            bad = "skip";
        }
        else if(seek % align)
        {
            bad = "seek";
        }
        else if(limit > 0 && src_is_device && (limit % align))
        {
            bad = "endpos";
        }

        if(bad != NULL)
        {
            fprintf(stderr, "Invalid %s: must be a multiple of the sector size (%" PRId64 ") when using a drive\n\n", bad, align);
            fclose(src);
            fclose(dst);
            return EXIT_FAIL;
        }

        if(dst_raw && dst_size > 0)
        {
            _long need = ((src_size + align - 1) / align) * align;
            if(need > dst_size)
            {
                fprintf(stderr, "Destination is too small: needs %" PRId64 " bytes, device has %" PRId64 "\n\n", need, dst_size);
                fclose(src);
                fclose(dst);
                return EXIT_FAIL;
            }
        }
    }

    /* -------------------------------------------------------------- */
    /*  Buffers                                                        */
    /* -------------------------------------------------------------- */

    tmp   = abuf_alloc(BUFFCAP);
    tmp2  = abuf_alloc(BUFFCAP);
    buff  = abuf_alloc(BUFFCAP);
    MBUFF = malloc(MBUFFSIZE);
    if(tmp == NULL || tmp2 == NULL || buff == NULL || MBUFF == NULL)
    {
        die("Out of memory");
    }

    /* Big stdio buffer for files only, devices stay unbuffered */
    if(!dst_is_device)
    {
        setvbuf(dst, MBUFF, _IOFBF, MBUFFSIZE);
    }

    if(seek > 0)
    {
        if(!dst_is_device)
        {
            dst_size = 0;
            if(FSEEK(dst, 0, SEEK_END) == 0)
            {
                dst_size = FTELL(dst);
            }
        }

        if(seek > dst_size)
        {
            fprintf(stderr, "Error: Destination is smaller than the starting position!\n\n");
            fclose(src);
            fclose(dst);
            return EXIT_FAIL;
        }
    }

    /* Differential: remember how far the old destination content goes, it is kept where the source is unreadable */
    if(g_differential)
    {
        if(dst_is_device)
        {
            g_dst_old_size = dst_size;
        }
        else if(FSEEK(dst, 0, SEEK_END) == 0)
        {
            g_dst_old_size = FTELL(dst);
        }

        if(g_dst_old_size < 0)
        {
            g_dst_old_size = 0;
        }
    }

    g_seek = seek;
    make_map_path(pargv[2]);

    fprintf(stdout, "\n");
    fprintf(stdout, "Source File Opened Successfully: %s\n", pargv[1]);
    fprintf(stdout, "Destination File Opened Successfully: %s\n", pargv[2]);
    fprintf(stdout, "Total Bytes to recover: %" PRId64 "\n", src_size - seek);
    fprintf(stdout, "badBlock Try Count: %" PRId64 "\n", retries);
    fprintf(stdout, "badBlock fill char: 0x%s\n", fill);
    fprintf(stdout, "skip count / sectorsize: %" PRId64 "\n", unit);
    fprintf(stdout, "Reading Start Position: %" PRId64 "\n", seek);

    if(align > 1)
    {
        fprintf(stdout, "Device sector size: %" PRId64 "\n", align);
    }

    if(g_slimit > 0)
    {
        fprintf(stdout, "Speed Throttle: %" PRId64 " MB/s\n", g_slimit);
    }

    if(limit > 0)
    {
        fprintf(stdout, "Reading End Position: %" PRId64 "\n", limit);
    }

    if(map_in != NULL)
    {
        fprintf(stdout, "Map loaded: %s\n", map_in);
        fprintf(stdout, "  copied: %" PRId64 "  not copied yet: %" PRId64 "  skipped: %" PRId64 "  bad: %" PRId64 " bytes\n",
                src_size - map_bytes(&map, 0, 0), map_bytes(&map, RS_NONTRIED, 0),
                map_bytes(&map, RS_SKIPPED, 0), map_bytes(&map, RS_BAD, 0));

        if(retries <= 1 && map_bytes(&map, RS_BAD, seek) > 0)
        {
            fprintf(stdout, "\033[93m  Note: bad sectors are only retried with try > 1\033[m\n");
        }
    }

    fprintf(stdout, "Map file: %s (saved every %d seconds)\n", map_path, CKPT_MS / 1000);
    g_ckpt_last = now_ms();

    /* -------------------------------------------------------------- */
    /*  Pass 1: copy everything readable                               */
    /* -------------------------------------------------------------- */

    phase = PHASE_COPY;
    if(map_bytes(&map, RS_NONTRIED, seek) > 0)
    {
        fprintf(stdout, "\n\033[1mPass 1: copying %" PRId64 " bytes of readable data\033[m\n", map_bytes(&map, RS_NONTRIED, seek));
        st_lines = 0;
        run_pass(&map, RS_NONTRIED, copy_range);

        if(!stop_req)
        {
            show_status("Copied", g_ckpt_pos, false, true);
        }
    }

    /* -------------------------------------------------------------- */
    /*  Pass 2: scrape the areas jumped over                           */
    /* -------------------------------------------------------------- */

    if(!stop_req && map_bytes(&map, RS_SKIPPED, seek) > 0)
    {
        fprintf(stdout, "\n\033[1mPass 2: scraping %" PRId64 " skipped bytes around the bad areas\033[m\n",
                map_bytes(&map, RS_SKIPPED, seek));
        st_lines = 0;
        phase = PHASE_SCRAPE;
        run_pass(&map, RS_SKIPPED, scrape_range);
    }

    /* -------------------------------------------------------------- */
    /*  Pass 3: retry the bad sectors                                  */
    /* -------------------------------------------------------------- */

    for(round = 1; round < retries; round++)
    {
        if(stop_req || map_bytes(&map, RS_BAD, seek) == 0)
        {
            break;
        }

        phase = PHASE_RETRY;
        g_rounds_left = retries - 1 - round;
        fprintf(stdout, "\n\033[1mPass 3: retry round %" PRId64 " of %" PRId64 ", %" PRId64 " bytes left\033[m\n",
                round, retries - 1, map_bytes(&map, RS_BAD, seek));
        st_lines = 0;
        run_pass(&map, RS_BAD, retry_range);
    }

    /* -------------------------------------------------------------- */
    /*  Finish                                                         */
    /* -------------------------------------------------------------- */

    buf_flush();
    fclose(src);

    /* Buffered data is written here: a failure now (disk full, device gone) must not look like success */
    io_fail = (fflush(dst) != 0);

#ifdef _WIN32
    if(dst_is_device)
    {
        release_devices(dst, !volume_letter(pargv[2]));
    }
#endif

    if(fclose(dst) != 0)
    {
        io_fail = 1;
    }

    if(io_fail)
    {
        die("Failed to Write Destination: flush/close failed, the output is incomplete!");
    }

    print_map(&map);

    {
        char s_read[32];
        char s_lost[32];
        char s_resc[32];

        format_size(st_acquired, s_read, sizeof(s_read));
        format_size(map_unreadable(&map), s_lost, sizeof(s_lost));
        format_size(st_recovered, s_resc, sizeof(s_resc));

        fprintf(stdout, "\nRead OK: %s   Unreadable: %s (%" PRId64 " bytes)   Rescued by Pass 2/3: %s   Failed reads: %" PRId64 "\n",
                s_read, s_lost, map_unreadable(&map), s_resc, st_read_errors);
    }

    /* Final map file */
    {
        char result[400];
        char cur_status = '+';

        if(!stop_req)
        {
            if(map.n == 0)
            {
                snprintf(result, sizeof(result), "Finished, all data copied");
            }
            else if(map_bytes(&map, RS_NONTRIED, 0) > 0)
            {
                snprintf(result, sizeof(result), "Finished from seek, %" PRId64 " bytes unreadable, %" PRId64 " bytes before seek not copied",
                         map_unreadable(&map), map_bytes(&map, RS_NONTRIED, 0));
            }
            else
            {
                snprintf(result, sizeof(result), "Finished, %" PRId64 " bytes unreadable", map_unreadable(&map));
            }
        }
        else
        {
            cur_status = running_status();
            snprintf(result, sizeof(result), "Interrupted in Pass %d, continue with --map %s", phase, map_path);
        }

        if(write_mapfile(&map, g_ckpt_pos, cur_status, result))
        {
            fprintf(stdout, "Map file: %s\n", map_path);
        }
        else
        {
            fprintf(stderr, "\033[93mWarning: unable to write map file %s\033[m\n", map_path);
        }
        fflush(stdout);
    }

    if(stop_req)
    {
        fprintf(stderr, "\nInterrupted. Continue with:\n  %s --map %s", pargv[0], map_path);
        for(i = 1; i < pargc; i++)
        {
            if(strchr(pargv[i], ' ') != NULL)
            {
                fprintf(stderr, " \"%s\"", pargv[i]);
            }
            else
            {
                fprintf(stderr, " %s", pargv[i]);
            }
        }
        fprintf(stderr, "\n");
    }
    else
    {
        fprintf(stderr, "\033[7mFinished\033[m\n");
    }

    map_free(&map);
    abuf_free(tmp);
    abuf_free(tmp2);
    abuf_free(buff);
    free(MBUFF);
    free(pargv);

    return stop_req ? EXIT_FAIL : EXIT_OK;
}
