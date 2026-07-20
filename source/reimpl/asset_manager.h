#ifndef ANDROID_ASSET_MANAGER_H
#define ANDROID_ASSET_MANAGER_H

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AAssetManager;
typedef struct AAssetManager AAssetManager;

struct AAssetDir;
typedef struct AAssetDir AAssetDir;

struct AAsset;
typedef struct AAsset AAsset;

enum {
    AASSET_MODE_UNKNOWN      = 0,
    AASSET_MODE_RANDOM       = 1,
    AASSET_MODE_STREAMING    = 2,
    AASSET_MODE_BUFFER       = 3
};

AAssetManager * AAssetManager_create(void);
AAssetManager * AAssetManager_fromJava(void *env, void *assetManager);

AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName);
AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode);

const char* AAssetDir_getNextFileName(AAssetDir* assetDir);
void AAssetDir_close(AAssetDir* assetDir);
void AAsset_close(AAsset* asset);

int AAsset_read(AAsset* asset, void* buf, size_t count);
off_t AAsset_seek(AAsset* asset, off_t offset, int whence);
off_t AAsset_getRemainingLength(AAsset* asset);
off_t AAsset_getLength(AAsset* asset);

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength);

const void * AAsset_getBuffer(AAsset* asset);

#ifdef __cplusplus
};
#endif

#endif