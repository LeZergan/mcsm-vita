/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  mem.h
 * @brief Implementations and wrappers for memory-related functions.
 */

#ifndef SOLOADER_MEM_H
#define SOLOADER_MEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <stddef.h>

#define MAP_FAILED (void*)-1

void *calloc_soloader(size_t nmemb, size_t size);
void free_soloader(void *ptr);
void *malloc_soloader(size_t size);
void *memalign_soloader(size_t alignment, size_t size);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs);
int munmap(void *addr, size_t length);
void *realloc_soloader(void *ptr, size_t size);
void *valloc_soloader(size_t size);

void mem_stats_snapshot(char *out, size_t out_size);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_MEM_H
