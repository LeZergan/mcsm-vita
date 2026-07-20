/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/sys.h"

#include <sys/errno.h>
#include <sys/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <string.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <stdlib.h>
#include <setjmp.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdarg.h>
#include <math.h>

#include "utils/utils.h"
#include "utils/logger.h"
#include "reimpl/io.h"

#define BIONIC_CLOCK_REALTIME           0
#define BIONIC_CLOCK_MONOTONIC          1
#define BIONIC_CLOCK_PROCESS_CPUTIME_ID 2
#define BIONIC_CLOCK_THREAD_CPUTIME_ID  3
#define BIONIC_CLOCK_MONOTONIC_RAW      4
#define BIONIC_CLOCK_REALTIME_COARSE    5
#define BIONIC_CLOCK_MONOTONIC_COARSE   6
#define BIONIC_CLOCK_BOOTTIME           7
#define BIONIC_CLOCK_REALTIME_ALARM     8
#define BIONIC_CLOCK_BOOTTIME_ALARM     9
#define BIONIC_CLOCK_SGI_CYCLE         10
#define BIONIC_CLOCK_TAI               11

#define __epoch 62135587294000000

#define AUX_AT_PLATFORM       15
#define AUX_AT_HWCAP          16
#define AUX_AT_CLKTCK         17
#define AUX_AT_SECURE         23
#define AUX_AT_BASE_PLATFORM  24
#define AUX_AT_HWCAP2         26
#define AUX_AT_PAGESZ         6

#define AUX_HWCAP_VFP         (1u << 6)
#define AUX_HWCAP_NEON        (1u << 12)
#define AUX_HWCAP_VFPV3       (1u << 13)
#define AUX_HWCAP_TLS         (1u << 15)
#define AUX_HWCAP_VFPV4       (1u << 16)
#define AUX_HWCAP_IDIVA       (1u << 17)
#define AUX_HWCAP_IDIVT       (1u << 18)

#define NETWORK_FORCE_OFFLINE 1

unsigned long long SDL_GetPerformanceCounter_soloader(void) {
    return (unsigned long long)sceKernelGetSystemTimeWide();
}

unsigned long long SDL_GetPerformanceFrequency_soloader(void) {
    return 1000000ULL;
}

unsigned int SDL_GetTicks_soloader(void) {
    return (unsigned int)(sceKernelGetSystemTimeWide() / 1000ULL);
}

int clock_gettime_soloader(clockid_t clock_id, struct timespec * tp) {
    switch (clock_id) {
        case BIONIC_CLOCK_MONOTONIC:
        case BIONIC_CLOCK_MONOTONIC_RAW:
        case BIONIC_CLOCK_MONOTONIC_COARSE:
        case BIONIC_CLOCK_BOOTTIME:
        case BIONIC_CLOCK_BOOTTIME_ALARM:
        case BIONIC_CLOCK_SGI_CYCLE:
        case BIONIC_CLOCK_PROCESS_CPUTIME_ID:
        case BIONIC_CLOCK_THREAD_CPUTIME_ID: {
            uint64_t systime = sceKernelGetSystemTimeWide();
            tp->tv_sec = (systime / 1000000);
            tp->tv_nsec = ((systime - (tp->tv_sec * 1000000)) * 1000);
            break;
        }
        case BIONIC_CLOCK_REALTIME:
        case BIONIC_CLOCK_REALTIME_COARSE:
        case BIONIC_CLOCK_REALTIME_ALARM:
        case BIONIC_CLOCK_TAI: {
            SceRtcTick tick;
            sceRtcGetCurrentTick(&tick);
            tick.tick -= __epoch;
            tp->tv_sec = (tick.tick / 1000000);
            tp->tv_nsec = ((tick.tick - (tp->tv_sec * 1000000)) * 1000);
            break;
        }
        default:
            l_error("clock_gettime / unexpected clock id %i", clock_id);
    }
    return 0;
}

int clock_getres_soloader(clockid_t clock_id, struct timespec * res) {
    res->tv_sec = 0; res->tv_nsec = 1000;
    return 0;
}

clock_t clock_soloader(void) { return sceKernelGetProcessTimeLow(); }

int sigaction(int signum, const struct sigaction * act, struct sigaction * oldact) {
    l_warn("sigaction(%i, ...): not implemented", signum);
    return 0;
}

int __system_property_get_soloader(const char *name, char *value) {
    l_warn("__system_property_get(%s, %p): not implemented", name, value);
    strncpy(value, "psvita", 7);
    return 7;
}

long syscall_soloader(long c, ...) {
    va_list args; va_start(args, c);
    long ret = 0;
    switch (c) {
        case 240: { // futex
            int *uaddr = va_arg(args, int *);
            int op = va_arg(args, int);
            int val = va_arg(args, int);
            void *timeout = va_arg(args, void *);
            int *uaddr2 = va_arg(args, int *);
            int val3 = va_arg(args, int);
            (void)uaddr; (void)timeout; (void)uaddr2; (void)val3;
            if ((op & 0xF) == 1) ret = val; else ret = 0;
            break;
        }
        case 241: case 242: ret = 0; break;
        default: l_warn("syscall(%li): not implemented", c); ret = 0; break;
    }
    va_end(args);
    return ret;
}

void __stack_chk_fail_soloader() {
    l_fatal("Stack collapsed at address %p", __builtin_return_address(0));
}

void abort_soloader() {
    l_fatal("Abort called from address %p", __builtin_return_address(0));
    abort();
}

void exit_soloader(int status) {
    l_fatal("Exit(%i) called from %p", status, __builtin_return_address(0));
    exit(status);
}

int __atomic_dec(volatile int *ptr) { return __sync_fetch_and_sub(ptr, 1); }
int __atomic_inc(volatile int *ptr) { return __sync_fetch_and_add(ptr, 1); }

int __atomic_swap(int new_value, volatile int *ptr) {
    int old_value;
    do { old_value = *ptr; } while (__sync_val_compare_and_swap(ptr, old_value, new_value) != old_value);
    return old_value;
}

int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr) {
    return __sync_val_compare_and_swap(ptr, old_value, new_value) != old_value;
}

char * getenv_soloader(const char * var) {
    static char env_empty[] = "";
    static char env_android[] = "android";
    static char env_zero[] = "0";
    static char env_one[] = "1";
    static char env_openssl_conf[] = "app0:/openssl.cnf";
    static char env_lua_path[] =
            "ux0:data/mcsm/?.lua;"
            "ux0:data/mcsm/?/init.lua;"
            "ux0:data/mcsm/scripts/?.lua;"
            "ux0:data/mcsm/scripts/?/init.lua;"
            "./?.lua;./?/init.lua";
    static char env_lua_cpath[] =
            "ux0:data/mcsm/?.so;"
            "ux0:data/mcsm/clibs/?.so;"
            "./?.so;./loadall.so";
    if (!var) return NULL;
    if (strcmp(var, "SDL_VIDEODRIVER") == 0) return env_android;
    if (strcmp(var, "SDL_VIDEO_GL_DRIVER") == 0) return env_empty;
    if (strcmp(var, "SDL_ACCEL_AS_JOY") == 0) return env_zero;
    if (strcmp(var, "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS") == 0) return env_one;
    if (strcmp(var, "SDL_DYNAMIC_API") == 0) return env_empty;
    if (strcmp(var, "SDL_GAMECONTROLLERCONFIG") == 0) return env_empty;
    if (strcmp(var, "SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS") == 0) return env_zero;
    if (strcmp(var, "GL_NV_depth_nonlinear") == 0) return env_empty;
    if (strcmp(var, "OPENSSL_CONF") == 0) return env_openssl_conf;
    if (strcmp(var, "LUA_PATH") == 0 || strcmp(var, "LUA_PATH_5_2") == 0) return env_lua_path;
    if (strcmp(var, "LUA_CPATH") == 0 || strcmp(var, "LUA_CPATH_5_2") == 0) return env_lua_cpath;
    l_warn("getenv(\"%s\"): not implemented.", var);
    return NULL;
}

int setenv_soloader(const char * name, const char * value, int overwrite) {
    l_warn("setenv(\"%s\", \"%s\"): not implemented.", name, value);
    return 0;
}

int getpagesize(void) { return PAGE_SIZE; }

long sysconf_soloader(int name) {
    if (name == _SC_NPROCESSORS_CONF || name == _SC_NPROCESSORS_ONLN || name == 96 || name == 97) return 4;
    if (name == _SC_PAGESIZE || name == _SC_PAGE_SIZE || name == 39 || name == 40) return PAGE_SIZE;
    if (name == _SC_PHYS_PAGES || name == 98) return (192 * 1024 * 1024) / PAGE_SIZE;
    if (name == _SC_AVPHYS_PAGES || name == 99) return (96 * 1024 * 1024) / PAGE_SIZE;
    if (name == _SC_CLK_TCK || name == 2) return 100;
    errno = EINVAL;
    l_warn("sysconf(%d): unsupported", name);
    return -1;
}

int __isinf_soloader(double x) { return isinf(x) ? 1 : 0; }

int setpriority_soloader(int which, int who, int prio) {
    (void)which; (void)who; (void)prio;
    return 0;
}

unsigned long getauxval_soloader(unsigned long type) {
    static const char platform[] = "v7l";
    switch (type) {
        case AUX_AT_PAGESZ: return PAGE_SIZE;
        case AUX_AT_CLKTCK: return 100;
        case AUX_AT_HWCAP: return AUX_HWCAP_VFP | AUX_HWCAP_NEON | AUX_HWCAP_VFPV3 | AUX_HWCAP_TLS | AUX_HWCAP_VFPV4 | AUX_HWCAP_IDIVA | AUX_HWCAP_IDIVT;
        case AUX_AT_HWCAP2: return 0;
        case AUX_AT_SECURE: return 0;
        case AUX_AT_PLATFORM: case AUX_AT_BASE_PLATFORM: return (unsigned long)platform;
        default: return 0;
    }
}

uid_t getuid_soloader(void) { return 0; }
uid_t geteuid_soloader(void) { return 0; }
gid_t getgid_soloader(void) { return 0; }
gid_t getegid_soloader(void) { return 0; }
pid_t gettid_soloader(void) { return sceKernelGetThreadId(); }

struct passwd *getpwuid_soloader(uid_t uid) {
    static char name[] = "vita"; static char passwd[] = "x"; static char comment[] = "vita";
    static char gecos[] = "vita"; static char dir[] = "ux0:data/mcsm"; static char shell[] = "/";
    static struct passwd entry;
    entry.pw_name = name; entry.pw_passwd = passwd; entry.pw_uid = uid; entry.pw_gid = 0;
    entry.pw_comment = comment; entry.pw_gecos = gecos; entry.pw_dir = dir; entry.pw_shell = shell;
    return &entry;
}

ssize_t pread_soloader(int fd, void *buf, size_t count, off_t offset) {
    if (asset_vfd_is(fd)) { return asset_vfd_pread(fd, buf, count, offset); }
    off_t old_offset = lseek(fd, 0, SEEK_CUR);
    if (old_offset < 0 || lseek(fd, offset, SEEK_SET) < 0) return -1;
    ssize_t read_bytes = read(fd, buf, count);
    lseek(fd, old_offset, SEEK_SET);
    if (obb_is_fd(fd)) {
        static int n = 0;
        if (count > 64 || offset > 8192 || n++ < 100) {
            l_info("[OBBIO] pread(fd=%d, off=%lld, count=%u) -> %d",
                   fd, (long long)offset, (unsigned)count, (int)read_bytes);
        }
    }
    return read_bytes;
}

ssize_t pwrite_soloader(int fd, const void *buf, size_t count, off_t offset) {
    if (asset_vfd_is(fd)) { (void)buf; (void)count; (void)offset; errno = EBADF; return -1; }
    off_t old_offset = lseek(fd, 0, SEEK_CUR);
    if (old_offset < 0 || lseek(fd, offset, SEEK_SET) < 0) return -1;
    ssize_t written_bytes = write_soloader(fd, buf, count);
    lseek(fd, old_offset, SEEK_SET);
    return written_bytes;
}

int gethostbyname_r_soloader(const char *name, struct hostent *ret, char *buf,
                             size_t buflen, struct hostent **result, int *h_errnop) {
    (void)buf; (void)buflen;
    struct hostent *host = gethostbyname_soloader(name);
    if (!host) { if (result) *result = NULL; if (h_errnop) *h_errnop = 1; return -1; }
    if (ret) *ret = *host;
    if (result) *result = ret ? ret : host;
    if (h_errnop) *h_errnop = 0;
    return 0;
}

static const char *gai_strerror_map(int errcode) {
    switch (errcode) {
        case 0: return "Success";
#ifdef EAI_AGAIN
        case EAI_AGAIN: return "Temporary failure in name resolution";
#endif
#ifdef EAI_BADFLAGS
        case EAI_BADFLAGS: return "Bad flags";
#endif
#ifdef EAI_FAIL
        case EAI_FAIL: return "Non-recoverable failure in name resolution";
#endif
#ifdef EAI_FAMILY
        case EAI_FAMILY: return "Address family not supported";
#endif
#ifdef EAI_MEMORY
        case EAI_MEMORY: return "Out of memory";
#endif
#ifdef EAI_NONAME
        case EAI_NONAME: return "Name or service not known";
#endif
#ifdef EAI_SERVICE
        case EAI_SERVICE: return "Service not supported for socket type";
#endif
#ifdef EAI_SOCKTYPE
        case EAI_SOCKTYPE: return "Socket type not supported";
#endif
#ifdef EAI_SYSTEM
        case EAI_SYSTEM: return "System error";
#endif
        default: return "Unknown getaddrinfo error";
    }
}

const char *gai_strerror_soloader(int errcode) { return gai_strerror_map(errcode); }

void freeaddrinfo_soloader(struct addrinfo *res) {
#if NETWORK_FORCE_OFFLINE
    (void)res;
#else
    freeaddrinfo(res);
#endif
}

int getaddrinfo_soloader(const char *node, const char *service,
                         const struct addrinfo *hints, struct addrinfo **res) {
    (void)hints; if (res) *res = NULL;
#if NETWORK_FORCE_OFFLINE
    l_warn("getaddrinfo(node=%s, service=%s): forced offline", node ? node : "(null)", service ? service : "(null)");
#ifdef EAI_NONAME
    return EAI_NONAME;
#else
    return -2;
#endif
#else
    int ret = getaddrinfo(node, service, hints, res);
    if (ret != 0) l_warn("getaddrinfo(node=%s, service=%s) failed: %d (%s)", node ? node : "(null)", service ? service : "(null)", ret, gai_strerror_map(ret));
    return ret;
#endif
}

struct hostent *gethostbyname_soloader(const char *name) {
#if NETWORK_FORCE_OFFLINE
    l_warn("gethostbyname(%s): forced offline", name ? name : "(null)");
    errno = ENETUNREACH;
    return NULL;
#else
    struct hostent *ret = gethostbyname(name);
    if (!ret) l_warn("gethostbyname(%s) failed", name ? name : "(null)");
    return ret;
#endif
}

static int fd_is_socket_soloader(int fd) {
    if (fd < 0) return 0;
#ifdef S_ISSOCK
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    return S_ISSOCK(st.st_mode) ? 1 : 0;
#else
    (void)fd;
    return 0;
#endif
}

int socket_soloader(int domain, int type, int protocol) {
#if NETWORK_FORCE_OFFLINE
    static unsigned log_count = 0;
    errno = ENETUNREACH;
    if (log_count < 32U) {
        l_warn("socket(domain=%d, type=%d, protocol=%d): forced offline", domain, type, protocol);
        log_count++;
    }
    return -1;
#else
    int fd = socket(domain, type, protocol);
    if (fd < 0) l_warn("socket(domain=%d, type=%d, protocol=%d) failed errno=%d", domain, type, protocol, errno);
    return fd;
#endif
}

int connect_soloader(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
#if NETWORK_FORCE_OFFLINE
    (void)addr; (void)addrlen;
    l_warn("connect(fd=%d): forced offline", sockfd);
    errno = ENETUNREACH;
    return -1;
#else
    int ret = connect(sockfd, addr, addrlen);
    if (ret < 0) l_warn("connect(fd=%d, family=%d) failed errno=%d", sockfd, addr ? (int)addr->sa_family : -1, errno);
    return ret;
#endif
}

// DLC DownloadManager calls poll() on sockets with 30s timeouts. Network is
// forced offline. If a socket still reaches poll/select, fail it immediately
// so boot cannot block on each download/license check timeout.
int poll_soloader(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (!fds && nfds > 0) {
        errno = EINVAL;
        return -1;
    }
    int ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd < 0) {
            fds[i].revents = 0;
            continue;
        }
        if (fd_is_socket_soloader(fds[i].fd)) {
            fds[i].revents = POLLERR | POLLHUP;
            ready++;
        }
    }
    if (ready > 0) {
        l_info("poll(nfds=%u, timeout=%d): sockets forced POLLERR", (unsigned)nfds, timeout);
        return ready;
    }
    if (timeout < 0 || timeout > 30000) {
        l_info("poll(nfds=%u, timeout=%d)", (unsigned)nfds, timeout);
    }
    return poll(fds, nfds, timeout > 5000 ? 5000 : timeout);
}

int select_soloader(int nfds, fd_set *readfds, fd_set *writefds,
                    fd_set *exceptfds, struct timeval *timeout) {
    int ready = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int isset = (readfds && FD_ISSET(fd, readfds)) || (writefds && FD_ISSET(fd, writefds));
        if (!isset) continue;
        if (fd_is_socket_soloader(fd)) {
            ready++;
            if (readfds) FD_CLR(fd, readfds);
            if (writefds) FD_CLR(fd, writefds);
            if (exceptfds) FD_SET(fd, exceptfds);
        }
    }
    if (ready > 0) {
        if (timeout) l_info("select(nfds=%d): sockets forced offline", nfds);
        else l_info("select(nfds=%d, timeout=NULL): sockets forced offline", nfds);
        return ready;
    }
    if (!timeout) l_info("select(nfds=%d, timeout=NULL)", nfds);
    else if (timeout->tv_sec > 30) l_info("select(nfds=%d, timeout=%lds)", nfds, (long)timeout->tv_sec);
    return select(nfds, readfds, writefds, exceptfds, timeout);
}

int sem_close_soloader(sem_t *sem) { (void)sem; return 0; }

typedef struct bionic_statfs_compat {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
} bionic_statfs_compat;

int statfs_soloader(const char *path, void *buf) {
    if (!buf) return -1;
    bionic_statfs_compat st;
    memset(&st, 0, sizeof(st));
    st.f_type = 0x58465342u;
    st.f_bsize = 4096;
    st.f_blocks = 1048576ULL;
    st.f_bfree = 524288ULL;
    st.f_bavail = 524288ULL;
    st.f_files = 65536ULL;
    st.f_ffree = 65535ULL;
    st.f_namelen = 255;
    st.f_frsize = 4096;
    memset(buf, 0, 128);
    memcpy(buf, &st, sizeof(st));
    static unsigned s_log_count = 0;
    if (s_log_count++ < 12U) {
        l_info("statfs(%s) -> bsize=%u bavail=%llu free=%llu MB",
               path ? path : "(null)",
               (unsigned)st.f_bsize,
               (unsigned long long)st.f_bavail,
               (unsigned long long)((st.f_bavail * st.f_bsize) / (1024ULL * 1024ULL)));
    }
    return 0;
}

ssize_t writev_soloader(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        ssize_t written = write_soloader(fd, iov[i].iov_base, iov[i].iov_len);
        if (written < 0) return total > 0 ? total : -1;
        total += written;
        if ((size_t)written != iov[i].iov_len) break;
    }
    return total;
}

int sigsetjmp_soloader(void *env, int savemask) { (void)savemask; return setjmp(*(jmp_buf *)env); }
void siglongjmp_soloader(void *env, int val) { longjmp(*(jmp_buf *)env, val); }
