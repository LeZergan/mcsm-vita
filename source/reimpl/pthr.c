/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define PTHR_MAX_OBJECTS 1024

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

void * initializedObjects[PTHR_MAX_OBJECTS] = {0};
static SceKernelLwMutexWork pthr_mutex;
static volatile short int pthr_mutex_inited = 0;

#define PTHR_LOCK \
    if (!pthr_mutex_inited) { \
        int ret = sceKernelCreateLwMutex(&pthr_mutex, "log_lock", 0, 0, NULL); \
        if (ret < 0) { \
            sceClibPrintf("Error: failed to create pthr mutex: 0x%x\n", ret); \
            return 0; \
        } \
        pthr_mutex_inited = 1; \
    } \
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);

#define PTHR_UNLOCK \
    if (pthr_mutex_inited) { \
        sceKernelUnlockLwMutex(&pthr_mutex, 1); \
    }

/* No-lock cores: caller MUST already hold pthr_mutex. Used so the whole
 * check-init-remember sequence in the static-init helpers can be done under a
 * single lock hold (the lock is non-recursive, so we can't call the locking
 * wrappers from inside it). */
static int isObjectInitialized_nl(const void * mut) {
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mut) return 1;
    }
    return 0;
}

static int rememberObject_nl(void * mut) {
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == 0) {
            initializedObjects[i] = mut;
            return 1;
        }
    }
    return 0;
}

int forgetObject(const void * mut) {
    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mut) {
            initializedObjects[i] = 0;
            PTHR_UNLOCK
            return 1;
        }
    }
    PTHR_UNLOCK
    return 0;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;

    /* Hold the lock across the ENTIRE check-init-remember so two threads can't
     * both initialize the same statically-initialized mutex (which produced two
     * real mutexes and a missed-wakeup race). */
    PTHR_LOCK
    if (isObjectInitialized_nl(mutex)) {
        PTHR_UNLOCK
        return ret;
    }

    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t));
    if (!real) {
        PTHR_UNLOCK
        return ENOMEM;
    }
    sceClibMemset(real, 0, sizeof(pthread_mutex_t));

    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    ret = pthread_mutex_init(real, &mutattr);
    pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        /* Publish real_ptr only AFTER the mutex is fully constructed, with a
         * release barrier, so the lock-free fast path (which reads real_ptr
         * without PTHR_LOCK) can never observe a non-null pointer to a
         * half-initialized mutex on a concurrent first-touch. */
        __atomic_store_n(&mutex->real_ptr, real, __ATOMIC_RELEASE);
        rememberObject_nl(mutex);
    } else {
        free(real);
        l_error("mutex initialization for %p has failed", mutex);
    }

    PTHR_UNLOCK
    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    /* Atomic check-init-remember (see _mutex_t_static_init) to avoid two
     * threads creating two real conds for the same statically-initialized cond,
     * which caused signals/waits to target different conds → missed wakeup. */
    PTHR_LOCK
    if (isObjectInitialized_nl(cond)) {
        PTHR_UNLOCK
        return ret;
    }

    pthread_cond_t *real = malloc(sizeof(pthread_cond_t));
    if (!real) {
        PTHR_UNLOCK
        return ENOMEM;
    }
    sceClibMemset(real, 0, sizeof(pthread_cond_t));

    ret = pthread_cond_init(real, attr);

    if (ret == 0) {
        /* Publish only after full init, release-ordered (see _mutex_t_static_init). */
        __atomic_store_n(&cond->real_ptr, real, __ATOMIC_RELEASE);
        rememberObject_nl(cond);
    } else {
        free(real);
        l_error("cond initialization for %p has failed", cond);
    }

    PTHR_UNLOCK
    return ret;
}

/* 4TH-CORE FIX (2026-07-03). The user runs GrapheneCt's capUnlocker, which
 * unlocks CPU core 3 (the normally system-reserved core) for the app. But
 * threads still default to cores 0-2 unless we explicitly widen their affinity.
 * Wrap every engine thread's entry so that, on its first instruction, it sets
 * its own CPU-affinity mask to include core 3, letting the Vita scheduler put
 * the engine's ~15 worker threads (audio, streaming, asset decode) on all 4
 * cores. Masks: USER_0=0x10000, USER_1=0x20000, USER_2=0x40000, and the
 * system core 3 = 0x80000. Try all-4 (0xF0000); if capUnlocker isn't active
 * the kernel rejects the system core and we fall back to 0-2 (0x70000) so we
 * never break thread startup. Read the toggle once: ux0:data/mcsm/no_core3.txt
 * forces the 3-core mask. */
static int g_core3_mask = -1; /* -1=unresolved, else the mask to apply */
static int mcsm_resolve_core_mask(void) {
    if (g_core3_mask >= 0) return g_core3_mask;
    int want4 = 1;
    FILE *f = fopen("ux0:data/mcsm/no_core3.txt", "r");
    if (f) { want4 = 0; fclose(f); }
    g_core3_mask = want4 ? 0x000F0000 : 0x00070000;
    return g_core3_mask;
}

typedef struct { void *(*start)(void *); void *param; } mcsm_thr_wrap;
static void *mcsm_thr_entry(void *arg) {
    mcsm_thr_wrap *w = (mcsm_thr_wrap *)arg;
    void *(*start)(void *) = w->start;
    void *param = w->param;
    free(w);
    SceUID self = sceKernelGetThreadId();
    int mask = mcsm_resolve_core_mask();
    int rc = sceKernelChangeThreadCpuAffinityMask(self, mask);
    if (rc < 0 && mask != 0x00070000) {
        /* capUnlocker not granting core 3 -> fall back to the 3 user cores. */
        g_core3_mask = 0x00070000;
        rc = sceKernelChangeThreadCpuAffinityMask(self, 0x00070000);
        mask = 0x00070000;
    }
    static int s_logged = 0;
    if (!s_logged) {
        s_logged = 1;
        l_info("CPU: thread affinity mask=0x%05X rc=0x%08X (%s)", mask, (unsigned)rc,
               (mask & 0x00080000) ? "4 cores incl core3 via capUnlocker" : "3 user cores");
    }
    return start(param);
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;
    const size_t default_stack = 128 * 1024;
    const size_t min_stack = 64 * 1024;
    size_t requested_stack = 0;
    size_t stack_to_use = default_stack;
    pthread_attr_t local_attr;
    pthread_attr_t *real_attr = NULL;

    if (!thread || !start) {
        return EINVAL;
    }

    if (!attr) {
        pthread_attr_init(&local_attr);
        real_attr = &local_attr;
    } else {
        _attr_t_static_init((pthread_attr_t_bionic *)attr);
        real_attr = attr->real_ptr;
    }

    if (real_attr && pthread_attr_getstacksize(real_attr, &requested_stack) != 0) {
        requested_stack = 0;
    }

    if (requested_stack >= min_stack && requested_stack <= default_stack) {
        stack_to_use = requested_stack;
    }

    if (real_attr) {
        pthread_attr_setstacksize(real_attr, stack_to_use);
    }

    /* Wrap the entry so the new thread widens its own CPU affinity to include
     * core 3 (capUnlocker). Falls back cleanly to plain start on alloc fail. */
    void *(*entry)(void *) = start;
    void *entry_arg = param;
    mcsm_thr_wrap *wrap = (mcsm_thr_wrap *)malloc(sizeof(mcsm_thr_wrap));
    if (wrap) {
        wrap->start = start;
        wrap->param = param;
        entry = mcsm_thr_entry;
        entry_arg = wrap;
    }

    ret = pthread_create(thread, real_attr, entry, entry_arg);
    if (ret != 0 && real_attr && stack_to_use > min_stack) {
        pthread_attr_setstacksize(real_attr, min_stack);
        stack_to_use = min_stack;
        ret = pthread_create(thread, real_attr, entry, entry_arg);
        l_warn("pthread_create retry with %u KB stack -> ret=%d", (unsigned)(min_stack / 1024), ret);
    }

    if (ret == EAGAIN && real_attr) {
        // Resource pressure can be transient while worker threads bootstrap.
        for (int i = 0; i < 8 && ret == EAGAIN; ++i) {
            sceKernelDelayThread(20000);
            ret = pthread_create(thread, real_attr, entry, entry_arg);
            if (ret == 0) {
                l_warn("pthread_create recovered after %d EAGAIN retries", i + 1);
                break;
            }
        }
    }

    /* Reclaim the wrapper ONLY if no attempt ever started the thread; on
     * success the new thread owns it and frees it in mcsm_thr_entry. */
    if (ret != 0 && wrap && entry == mcsm_thr_entry) {
        free(wrap);
    }

    if (ret == 0) {
        l_info("pthread_create(start=%p, stack=%u KB) ok thread=%p",
               start, (unsigned)(stack_to_use / 1024), (void *)*thread);
    } else {
        l_warn("pthread_create(start=%p, stack=%u KB) failed ret=%d",
               start, (unsigned)(stack_to_use / 1024), ret);
    }

    if (!attr) {
        pthread_attr_destroy(&local_attr);
    }

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    forgetObject(mutex);
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    if (mutex->real_ptr) free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    /* Fast path: an initialized mutex holds a real heap pointer in real_ptr,
     * published with a release barrier only AFTER pthread_mutex_init completes
     * (see _mutex_t_static_init); a still-static bionic mutex holds a small
     * sentinel (<=0x8000). The acquire load pairs with that release so we never
     * fast-path onto a half-initialized mutex, and skips the global init lock +
     * 1024-entry scan for the common inited case. */
    pthread_mutex_t *rp = __atomic_load_n(&mutex->real_ptr, __ATOMIC_ACQUIRE);
    if ((uintptr_t)rp > 0x10000u) return pthread_mutex_lock(rp);
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_lock(mutex->real_ptr);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *rp = __atomic_load_n(&mutex->real_ptr, __ATOMIC_ACQUIRE);
    if ((uintptr_t)rp > 0x10000u) return pthread_mutex_trylock(rp);
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!mutex->real_ptr) return EINVAL;
    return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    forgetObject(cond);
    int ret = pthread_cond_destroy(cond->real_ptr);
    if (cond->real_ptr) free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    pthread_cond_t *rc = __atomic_load_n(&cond->real_ptr, __ATOMIC_ACQUIRE);
    if ((uintptr_t)rc > 0x10000u) return pthread_cond_signal(rc);
    _cond_t_static_init(cond, NULL);
    return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;
    if (!((uintptr_t)__atomic_load_n(&cond->real_ptr, __ATOMIC_ACQUIRE) > 0x10000u &&
          (uintptr_t)__atomic_load_n(&mutex->real_ptr, __ATOMIC_ACQUIRE) > 0x10000u)) {
        _cond_t_static_init(cond, NULL);
        _mutex_t_static_init(mutex, NULL);
    }
    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;
    if (!((uintptr_t)__atomic_load_n(&cond->real_ptr, __ATOMIC_ACQUIRE) > 0x10000u &&
          (uintptr_t)__atomic_load_n(&mutex->real_ptr, __ATOMIC_ACQUIRE) > 0x10000u)) {
        _cond_t_static_init(cond, NULL);
        _mutex_t_static_init(mutex, NULL);
    }
    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    pthread_cond_t *rc = __atomic_load_n(&cond->real_ptr, __ATOMIC_ACQUIRE);
    if ((uintptr_t)rc > 0x10000u) return pthread_cond_broadcast(rc);
    _cond_t_static_init(cond, NULL);
    return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) {
        return EINVAL;
    }

    int state = __atomic_load_n(once_control, __ATOMIC_ACQUIRE);
    if (state == 2) {
        return 0;
    }

    if (state == 0 && __sync_bool_compare_and_swap(once_control, 0, 1)) {
        init_routine();
        __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
        return 0;
    }

    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 1) {
        sceKernelDelayThread(1000);
    }

    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    if (!uid || !sval) {
        errno = EINVAL;
        return -1;
    }

    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    if (!uid) {
        errno = EINVAL;
        return -1;
    }

    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) {
        errno = EINVAL;
        return -1;
    }

    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    long long remaining = _timeout - now;
    if (remaining <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    uint timeout_real = (remaining > 0xFFFFFFFFLL) ? 0xFFFFFFFFu : (uint) remaining;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    return 0;
}

int sem_trywait_soloader (int * uid) {
    if (!uid) {
        errno = EINVAL;
        return -1;
    }

    uint timeout = 0;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0) {
        errno = EAGAIN;
        return -1;
    }

    return 0;
}

int sem_wait_soloader (int * uid) {
    if (!uid) {
        errno = EINVAL;
        return -1;
    }

    if (sceKernelWaitSema(*uid, 1, NULL) < 0) {
        errno = EINTR;
        return -1;
    }

    return 0;
}
