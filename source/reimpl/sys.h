/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  sys.h
 * @brief Implementations and wrappers for misc. system functions.
 */

#ifndef SOLOADER_SYS_H
#define SOLOADER_SYS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PAGE_SIZE 4096

clock_t clock_soloader(void);

// SDL high-resolution timer overrides. The game derives its frame delta-time as
// (counter_delta / frequency); if SDL's frequency comes back 0 the engine computes
// 1.0/0 = +inf seconds-per-cycle and a 0 delta yields NaN, freezing the game clock
// (every time-gated boot step then never completes). Back these with the Vita
// monotonic microsecond timer so the clock is always valid and advancing.
unsigned long long SDL_GetPerformanceCounter_soloader(void);
unsigned long long SDL_GetPerformanceFrequency_soloader(void);
unsigned int SDL_GetTicks_soloader(void);

int clock_gettime_soloader(clockid_t clock_id, struct timespec * tp);

int clock_getres_soloader(clockid_t clock_id, struct timespec * res);

int __system_property_get_soloader(const char *name, char *value);

long syscall_soloader(long c, ...);

void __stack_chk_fail_soloader();

void abort_soloader();

void exit_soloader(int status);

int __atomic_dec(volatile int *ptr);

int __atomic_inc(volatile int *ptr);

int __atomic_swap(int new_value, volatile int *ptr);

int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr);

char * getenv_soloader(const char * name);

int setenv_soloader(const char * name, const char * value, int overwrite);

int getpagesize(void);
long sysconf_soloader(int name);
int __isinf_soloader(double x);

int setpriority_soloader(int which, int who, int prio);
unsigned long getauxval_soloader(unsigned long type);

uid_t getuid_soloader(void);
uid_t geteuid_soloader(void);
gid_t getgid_soloader(void);
gid_t getegid_soloader(void);
pid_t gettid_soloader(void);
struct passwd *getpwuid_soloader(uid_t uid);

ssize_t pread_soloader(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite_soloader(int fd, const void *buf, size_t count, off_t offset);

int gethostbyname_r_soloader(const char *name, struct hostent *ret, char *buf,
                             size_t buflen, struct hostent **result, int *h_errnop);
struct hostent *gethostbyname_soloader(const char *name);
int getaddrinfo_soloader(const char *node, const char *service,
                         const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo_soloader(struct addrinfo *res);
const char *gai_strerror_soloader(int errcode);
int socket_soloader(int domain, int type, int protocol);
int connect_soloader(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int poll_soloader(struct pollfd *fds, nfds_t nfds, int timeout);
int select_soloader(int nfds, fd_set *readfds, fd_set *writefds,
                    fd_set *exceptfds, struct timeval *timeout);

int sem_close_soloader(sem_t *sem);

int statfs_soloader(const char *path, void *buf);

ssize_t writev_soloader(int fd, const struct iovec *iov, int iovcnt);

int sigsetjmp_soloader(void *env, int savemask);
void siglongjmp_soloader(void *env, int val);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SYS_H
