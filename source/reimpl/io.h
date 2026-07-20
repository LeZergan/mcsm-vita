/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  io.h
 * @brief Wrappers and implementations for some of the IO functions.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>
#include <utime.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

// Must match the Android/bionic ARM32 `struct stat64` layout byte-for-byte:
// the game reads fields (notably st_size at offset 48) using bionic's layout.
// Do NOT use __packed__ or newlib's nlink_t/uid_t/gid_t (16-bit on vitasdk) -
// that put st_size at offset 38, so the engine read OBB sizes as garbage/0 and
// loaded no archive content. Fixed-width types + natural alignment reproduce the
// real layout (st_size@48, st_blksize@56, st_blocks@64, st_ino@96, sizeof=104).
typedef struct stat64_bionic {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned long st_uid;
    unsigned long st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long long st_ino;
} stat64_bionic;

typedef struct __attribute__((__packed__)) dirent64_bionic {
    int16_t d_ino; // 2 bytes // offset 0x0
    int64_t d_off; // 8 bytes // offset 0x2
    uint64_t d_reclen; // 8 bytes // 0xA
    unsigned char d_type; // 1 byte // offset 0x12
    char d_name[256]; // 256 bytes // offset 0x13
} dirent64_bionic;

int open_soloader(const char * path, int oflag, ...);

/* Android-path-remapping filesystem wrappers (saves write under remapped paths). */
int mkdir_soloader(const char *path, mode_t mode);
int rename_soloader(const char *oldp, const char *newp);
int unlink_soloader(const char *path);
int remove_soloader(const char *path);
int rmdir_soloader(const char *path);
int access_soloader(const char *path, int amode);
int chmod_soloader(const char *path, mode_t mode);
int truncate_soloader(const char *path, off_t length);
int ftruncate_soloader(int fd, off_t length);
int lstat_soloader(const char *path, stat64_bionic *buf);
char *realpath_soloader(const char *path, char *resolved_path);
int chdir_soloader(const char *path);
int utime_soloader(const char *path, const struct utimbuf *times);

ssize_t read_soloader(int fd, void *buf, size_t count);
ssize_t write_soloader(int fd, const void *buf, size_t count);

off_t lseek_soloader(int fd, off_t offset, int whence);

int obb_is_fd(int fd);

int asset_vfd_open(const char *path, off_t length);
int asset_vfd_is(int fd);
ssize_t asset_vfd_read(int fd, void *buf, size_t count);
ssize_t asset_vfd_pread(int fd, void *buf, size_t count, off_t offset);
off_t asset_vfd_lseek(int fd, off_t offset, int whence);
int asset_vfd_fstat(int fd, stat64_bionic *buf);
int asset_vfd_close(int fd);
void asset_vfd_trim_cached_fds(unsigned int target_open);

FILE * fopen_soloader(const char * filename, const char * mode);

DIR *opendir_soloader(char *name);

int stat_soloader(const char * path, stat64_bionic * buf);

int fstat_soloader(int fd, stat64_bionic * buf);

struct dirent64_bionic * readdir_soloader(DIR *dir);

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result);

int close_soloader(int fd);

int fclose_soloader(FILE *f);

int closedir_soloader(DIR *dir);

int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... /* arg */);

int fsync_soloader(int fd);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_IO_H
