#include "reimpl/asset_manager.h"
#include "reimpl/io.h"
#include "utils/logger.h"
#ifdef USE_PVR_PSP2
#include "utils/loading_screen.h"
#endif

#include <pthread.h>
#include <malloc.h>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <libc_bridge/libc_bridge.h>
#include <string>
#include <fcntl.h>
#include <cerrno>

typedef struct assetManager {
    int dummy = 0;
    pthread_mutex_t mLock;
} assetManager;

typedef struct aAsset {
    char * filename;
    FILE* f;
    size_t bytesRead;
    size_t fileSize;
    bool opened = false;
    void *buffer;
    size_t bufferSize;
} aAsset;

typedef struct aAssetDirState {
    char **entries;
    size_t count;
    size_t cursor;
} aAssetDirState;

static AAssetManager * g_AAssetManager = NULL;

static FILE *asset_fopen(const char *path) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fopen(path, "rb");
#else
    return fopen(path, "rb");
#endif
}

static FILE *asset_fopen_with_retry(const char *path) {
    FILE *f = asset_fopen(path);
    if (f) {
        return f;
    }

    int first_errno = errno;
    asset_vfd_trim_cached_fds(8u);
    f = asset_fopen(path);

    static unsigned int s_fail_log = 0;
    if (f) {
        if (s_fail_log++ < 24u) {
            l_info("AAssetManager_open retry succeeded after VFD trim: %s", path);
        }
    } else if (s_fail_log++ < 24u) {
        l_warn("AAssetManager_open failed: %s errno=%d", path, first_errno);
    }
    return f;
}

static std::string normalize_asset_dir_name(const char *dirName) {
    if (!dirName) return "";
    std::string normalized(dirName);
    while (!normalized.empty() && normalized.front() == '/') normalized.erase(normalized.begin());
    while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
    return normalized;
}

static int asset_dir_entry_cmp(const void *a, const void *b) {
    const char *lhs = *(const char * const *)a;
    const char *rhs = *(const char * const *)b;
    return strcmp(lhs, rhs);
}

static bool asset_dir_push_entry(aAssetDirState *dir, const char *name) {
    char **new_entries = (char **)realloc(dir->entries, (dir->count + 1) * sizeof(char *));
    if (!new_entries) return false;
    dir->entries = new_entries;
    dir->entries[dir->count] = strdup(name);
    if (!dir->entries[dir->count]) return false;
    dir->count += 1;
    return true;
}

AAssetManager * AAssetManager_create() {
    if (g_AAssetManager) return g_AAssetManager;
    assetManager am;
    pthread_mutex_init(&am.mLock, NULL);
    g_AAssetManager = (AAssetManager *) malloc(sizeof(assetManager));
    memcpy(g_AAssetManager, &am, sizeof(assetManager));
    return g_AAssetManager;
}

AAssetManager * AAssetManager_fromJava(void *env, void *assetManager) {
    (void)env; (void)assetManager;
    return AAssetManager_create();
}

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
#ifdef USE_PVR_PSP2
    if (filename && *filename) { loading_screen_set_asset(filename); loading_screen_tick(); }
#endif
    std::string realp = std::string(DATA_PATH) + std::string("assets/") + std::string(filename);

    auto * a = new aAsset;
    a->filename = (char *) malloc(realp.length() + 1);
    strcpy(a->filename, realp.c_str());
    a->bytesRead = 0;
    a->buffer = NULL;
    a->bufferSize = 0;

    a->f = asset_fopen_with_retry((const char *)a->filename);

    if (!a->f) {
        /* PERF (2026-07-17): build the fallback path only when the primary open
         * fails (the common case succeeds) — saves a std::string alloc/free per open. */
        std::string fallbackp = std::string(DATA_PATH) + std::string(filename);
        free(a->filename);
        a->filename = (char *) malloc(fallbackp.length() + 1);
        strcpy(a->filename, fallbackp.c_str());
        a->f = asset_fopen_with_retry((const char *)a->filename);
    }

    if (!a->f) {
        free(a->filename);
        delete a;
        a = NULL;
    } else {
#ifdef USE_SCELIBC_IO
        sceLibcBridge_fseek(a->f, 0, SEEK_END);
        a->fileSize = sceLibcBridge_ftell(a->f);
        sceLibcBridge_fseek(a->f, 0, SEEK_SET);
#else
        /* PERF: 32 KB fully-buffered stream (newlib default BUFSIZ is 1 KB) so
         * AAsset_read/fread refills in big chunks instead of 1 KB sceIo reads.
         * Freed on fclose. (The hot .ttarch2 reads use the vfd/pread path, not this.) */
        setvbuf(a->f, NULL, _IOFBF, 32 * 1024);
        fseek(a->f, 0, SEEK_END);
        a->fileSize = ftell(a->f);
        fseek(a->f, 0, SEEK_SET);
#endif
        a->opened = true;
    }

    l_debug("AAssetManager_open<%p>(%p, %s, %i): %p", __builtin_return_address(0), mgr, realp.c_str(), mode, a);
    return (AAsset *) a;
}

void AAsset_close(AAsset* asset) {
    l_debug("AAsset_close<%p>(%p)", __builtin_return_address(0), asset);
    if (asset) {
        aAsset * a = (aAsset *) asset;
        free(a->filename);
        if (a->opened) {
#ifdef USE_SCELIBC_IO
            sceLibcBridge_fclose(a->f);
#else
            fclose(a->f);
#endif
        }
        if (a->buffer) { free(a->buffer); a->buffer = NULL; a->bufferSize = 0; }
        delete a;
    }
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
    l_debug("AAsset_read<%p>(%p, %p, %i)", __builtin_return_address(0), asset, buf, count);
    if (!asset) return -1;
    aAsset * a = (aAsset *) asset;
    if (!a->opened) return -1;
#ifdef USE_SCELIBC_IO
    size_t ret = sceLibcBridge_fread(buf, 1, count, a->f);
#else
    size_t ret = fread(buf, 1, count, a->f);
#endif
    if (ret > 0) { a->bytesRead += ret; return (int) ret; }
    else {
#ifdef USE_SCELIBC_IO
        if (sceLibcBridge_feof(a->f)) return 0; else return -1;
#else
        if (feof(a->f)) return 0; else return -1;
#endif
    }
}

off_t AAsset_seek(AAsset* asset, off_t offset, int whence) {
    l_debug("AAsset_seek(%p, %d, %i)", asset, offset, whence);
    if (!asset) return (off_t)-1;
    aAsset * a = (aAsset *) asset;
    if (!a->opened) return -1;
#ifdef USE_SCELIBC_IO
    if (sceLibcBridge_fseek(a->f, offset, whence) != 0) return -1;
    long pos = sceLibcBridge_ftell(a->f);
#else
    if (fseek(a->f, offset, whence) != 0) return -1;
    long pos = ftell(a->f);
#endif
    if (pos < 0) return -1;
    a->bytesRead = (size_t) pos;
    return (off_t) pos;
}

off_t AAsset_getRemainingLength(AAsset* asset) {
    if (!asset) return (off_t)-1;
    aAsset * a = (aAsset *) asset;
    if (!a->opened) return -1;
    return (off_t)(a->fileSize - a->bytesRead);
}

off_t AAsset_getLength(AAsset* asset) {
    if (!asset) return (off_t)-1;
    aAsset * a = (aAsset *) asset;
    return (off_t)a->fileSize;
}

AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName) {
    std::string normalizedDirName = normalize_asset_dir_name(dirName);
    std::string realDirPath = std::string(DATA_PATH) + "assets/";
    std::string fallbackDirPath = std::string(DATA_PATH);
    if (!normalizedDirName.empty()) { realDirPath += normalizedDirName + "/"; fallbackDirPath += normalizedDirName + "/"; }

    auto *dir = new aAssetDirState;
    dir->entries = NULL; dir->count = 0; dir->cursor = 0;

    DIR *osDir = opendir(realDirPath.c_str());
    bool usingFallbackDir = false;
    if (!osDir) { osDir = opendir(fallbackDirPath.c_str()); usingFallbackDir = osDir != NULL; }
    if (!osDir) {
        l_warn("AAssetManager_openDir<%p>(%p, %s): opendir failed for %s",
               __builtin_return_address(0), mgr, dirName ? dirName : "(null)", realDirPath.c_str());
        return (AAssetDir *)dir;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(osDir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        std::string assetName;
        if (!normalizedDirName.empty()) assetName = normalizedDirName + "/";
        assetName += entry->d_name;
        if (!asset_dir_push_entry(dir, assetName.c_str())) { l_error("AAssetManager_openDir: failed to append asset entry: %s", assetName.c_str()); break; }
    }
    closedir(osDir);
    if (dir->count > 1) qsort(dir->entries, dir->count, sizeof(char *), asset_dir_entry_cmp);
    l_debug("AAssetManager_openDir<%p>(%p, %s): %zu entries (%s)",
            __builtin_return_address(0), mgr, dirName ? dirName : "(null)", dir->count, usingFallbackDir ? "fallback" : "assets");
    return (AAssetDir *)dir;
}

const char* AAssetDir_getNextFileName(AAssetDir* assetDir) {
    if (!assetDir) return NULL;
    aAssetDirState *dir = (aAssetDirState *)assetDir;
    if (dir->cursor >= dir->count) return NULL;
    const char *name = dir->entries[dir->cursor];
    dir->cursor += 1;
    return name;
}

void AAssetDir_close(AAssetDir* assetDir) {
    if (assetDir) {
        aAssetDirState *dir = (aAssetDirState *)assetDir;
        for (size_t i = 0; i < dir->count; ++i) free(dir->entries[i]);
        free(dir->entries);
        delete dir;
    }
}

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength) {
    if (!asset) { l_warn("AAsset_openFileDescriptor(%p, %p, %p): asset is null", asset, outStart, outLength); return -1; }
    aAsset * a = (aAsset *) asset;
    if (outStart) *outStart = 0;
    if (outLength) *outLength = a->fileSize;
    if (a->opened) {
#ifdef USE_SCELIBC_IO
        sceLibcBridge_fclose(a->f);
#else
        fclose(a->f);
#endif
        a->opened = false;
        a->f = NULL;
    }
    int ret = asset_vfd_open(a->filename, (off_t)a->fileSize);
    l_debug("AAsset_openFileDescriptor(%p/\"%s\", %p, %p): ret %i", asset, a->filename, outStart, outLength, ret);
    return ret;
}

const void * AAsset_getBuffer(AAsset* asset) {
    static unsigned log_count = 0;
    if (!asset) { l_warn("AAsset_getBuffer: asset is null"); return NULL; }

    aAsset * a = (aAsset *) asset;

    if (a->buffer) return a->buffer;

    if (!a->opened && a->filename) {
        a->f = asset_fopen_with_retry(a->filename);
        if (!a->f) { l_error("AAsset_getBuffer: failed to re-open %s", a->filename); return NULL; }
        a->opened = true;
        a->bytesRead = 0;
    }

    if (!a->opened || !a->f) { l_error("AAsset_getBuffer: asset not open and no filename to re-open"); return NULL; }

    if (a->fileSize == 0) {
        if (log_count < 4U) { l_warn("AAsset_getBuffer: asset %s has 0 size", a->filename ? a->filename : "(null)"); log_count++; }
        return NULL;
    }

    void *buf = malloc(a->fileSize);
    if (!buf) { l_error("AAsset_getBuffer: malloc(%zu) failed for %s", a->fileSize, a->filename ? a->filename : "(null)"); return NULL; }

#ifdef USE_SCELIBC_IO
    sceLibcBridge_fseek(a->f, 0, SEEK_SET);
    size_t nread = sceLibcBridge_fread(buf, 1, a->fileSize, a->f);
#else
    fseek(a->f, 0, SEEK_SET);
    size_t nread = fread(buf, 1, a->fileSize, a->f);
#endif

    if (nread != a->fileSize) { l_error("AAsset_getBuffer: read %zu/%zu bytes for %s", nread, a->fileSize, a->filename ? a->filename : "(null)"); free(buf); return NULL; }

    a->buffer = buf;
    a->bufferSize = a->fileSize;
    a->bytesRead = a->fileSize;

    if (log_count < 8U) { l_info("AAsset_getBuffer: %s -> %p (%zu bytes)", a->filename ? a->filename : "(null)", buf, a->fileSize); log_count++; }
    return buf;
}
