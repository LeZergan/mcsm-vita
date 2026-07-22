#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>
#include "reimpl/asset_manager.h"
#include "java_runtime.h"
#include "utils/glutil.h"
#include "utils/launch_state.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/config.h"
#include "reimpl/controls.h"

enum mcsm_method_ids {
    MID_GET_EXTERNAL_STORAGE_DIRECTORY = 1000,
    MID_GET_PACKAGE_NAME = 1001,
    MID_GET_OBB_FILENAME = 1002,
    MID_GET_EXTERNAL_STORAGE_PATH = 1003,
    MID_GET_INTERNAL_STORAGE_PATH = 1004,
    MID_GET_ASSETS = 1005,
    MID_GET_CONTEXT = 1006,
    MID_GET_APPLICATION_CONTEXT = 1007,
    MID_GET_NATIVE_SURFACE = 1008,
    MID_FLIP_BUFFERS = 1009,
    MID_AUDIO_INIT = 1010,
    MID_AUDIO_WRITE_SHORT_BUFFER = 1011,
    MID_AUDIO_WRITE_BYTE_BUFFER = 1012,
    MID_AUDIO_QUIT = 1013,
    MID_POLL_INPUT_DEVICES = 1014,
    MID_INPUT_GET_INPUT_DEVICE_IDS = 1015,
    MID_GET_HARDWARE_MODEL = 1016,
    MID_SET_ACTIVITY_TITLE = 1017,
    MID_HAS_FEATURE = 1018,
    MID_SET_FRAMEBUFFER_SIZE = 1019,
    MID_GET_SAMPLE_RATE = 1020,
    MID_GET_OUTPUT_FRAMES_PER_BUFFER = 1021,
    MID_IS_USING_BLUETOOTH = 1022,
    MID_GET_HARDWARE_DISPLAY = 1023,
    MID_GET_HARDWARE_MANUFACTURER = 1024,
    MID_GET_XDPI = 1025,
    MID_GET_YDPI = 1026,
    MID_GET_WIDTH = 1027,
    MID_GET_HEIGHT = 1028,
    MID_GET_SCREEN_WIDTH = 1029,
    MID_GET_SCREEN_HEIGHT = 1030,
    MID_GET_SURFACE_WIDTH = 1031,
    MID_GET_SURFACE_HEIGHT = 1032,
    MID_GET_EXTERNAL_STORAGE_STATE = 1033,
    MID_GET_EXTERNAL_STORAGE_DIRS = 1034,
    MID_GET_FILES_DIR = 1035,
    MID_GET_ABSOLUTE_PATH = 1036,
    MID_CHECK_INIT = 1037,
    MID_SUPPORTS_LOW_LATENCY = 1038,
    MID_GET_ASSET_MANAGER = 1039,
    MID_FMOD_AUDIODEVICE_INIT = 1040,
    MID_GET_LOCALE = 1041,
    MID_GET_HARDWARE_OS = 1042,
    MID_GET_HARDWARE_BOARD = 1043,
    MID_AUDIO_DEVICE_INIT = 1044,
    MID_AUDIO_DEVICE_CLOSE = 1045,
    MID_AUDIO_DEVICE_WRITE = 1046,
    MID_IS_DATA_AVAILABLE = 1047,
    MID_IS_TV = 1048,
    MID_UPDATE_PURCHASES = 1049,
    MID_GET_OUTPUT_SAMPLE_RATE = 1050,
    MID_GET_OUTPUT_BLOCK_SIZE = 1051,
    MID_IS_PURCHASED = 1052,
    MID_GET_PURCHASED_SKUS = 1053,
    MID_REQUEST_PERMISSION = 1054,
    MID_IS_DOWNLOADED = 1055,
    MID_IS_DOWNLOADING = 1056,
    MID_GET_DOWNLOAD_PROGRESS = 1057,
    MID_CHECK_LICENSE = 1058,
    MID_IS_NETWORK_AVAILABLE = 1059,
    MID_GET_PURCHASE_PROVIDER = 1060,
    MID_IS_PRODUCT_PURCHASED = 1061,
    MID_PURCHASE = 1062,
    MID_ON_PURCHASE = 1063,
    MID_ON_UNLOCK_ACHIEVEMENT = 1064,
    MID_IS_SIGNED_IN = 1065,
};

/* Battery/perf default (2026-07-20): 720x408 = ~3/4 of native 960x544 (aspect
 * ~16:9). Stepped DOWN one notch from the previous 800x452 readability default:
 * the engine is FRAGMENT-BOUND, so this ~19%-fewer-pixels drop cuts GPU fill and
 * power in every scene (better battery + more 30fps headroom so the ARM-clock
 * governor can downclock further) while staying WELL above the old unreadable
 * 480x272 potato — text/UI at 720 wide is only ~10% smaller than at 800 and
 * still comfortably legible. The engine sizes its intermediate render targets
 * from this at boot, so it drives text/UI sharpness. Tunable via fb_override.txt:
 * 960x544 = native (sharpest, heaviest), 800x452 = the old sharper default,
 * 640x363 = lighter still, 480x272 = max fps. Height kept EVEN (408) — GXM
 * render targets are happiest with even dims and the fb_override sanitizer forces
 * even too, so this compile-time default matches an fb_override.txt of "720x408". */
#define MCSM_DEFAULT_RENDER_W 720
#define MCSM_DEFAULT_RENDER_H 408

static int g_fb_width = MCSM_DEFAULT_RENDER_W;
static int g_fb_height = MCSM_DEFAULT_RENDER_H;
static jobject g_asset_manager_obj = NULL;
static jobject g_context_obj = NULL;
static jobject g_native_surface_obj = NULL;
static int g_display_metrics_logged = 0;
static int g_audio_sample_rate = 48000;
static int g_audio_channels = 2;
static int g_audio_bytes_per_sample = 2;
static int g_audio_port = -1;
static int g_audio_port_frames = 1024;
static int g_audio_port_rate = 48000;
static int g_audio_port_channels = 2;
static int16_t *g_audio_scratch = NULL;
static int g_audio_scratch_samples = 0;
static int g_fb_override_loaded = 0;
static int g_fb_override_enabled = 1;
static int g_fb_override_width = MCSM_DEFAULT_RENDER_W;
static int g_fb_override_height = MCSM_DEFAULT_RENDER_H;
static unsigned int g_flipbuffers_count = 0;
static int g_request_permission_logged = 0;

enum mcsm_field_ids {
    FID_WINDOW_SERVICE = 0,
    FID_SDK_INT = 1,
    FID_M_ASSET_MGR = 2,
};

static void sync_runtime_field_objects(void);

static void ensure_runtime_fields(void) {
    if (!g_asset_manager_obj) {
        g_asset_manager_obj = (jobject)AAssetManager_create();
    }
    if (!g_context_obj) {
        g_context_obj = jni->NewStringUTF(&jni, "context_main");
    }
    if (!g_native_surface_obj) {
        g_native_surface_obj = jni->NewStringUTF(&jni, "native_surface");
    }
    sync_runtime_field_objects();
}

static void sanitize_framebuffer_override(int *w, int *h, int *sanitized) {
    if (!w || !h || !sanitized || *w <= 0 || *h <= 0) return;

    if (*w <= 720) {
        int clean_h = ((*w * 9) + 8) / 16;
        if (clean_h & 1) {
            clean_h--;
        }
        int delta = *h - clean_h;
        if (delta < 0) {
            delta = -delta;
        }
        if (clean_h > 0 && delta > 0 && delta <= 4) {
            *h = clean_h;
            *sanitized = 1;
            return;
        }
    }

    if ((*h & 1) && *h > 180) {
        (*h)--;
        *sanitized = 1;
    }
}

static void ensure_framebuffer_override_loaded(void) {
    if (g_fb_override_loaded) return;
    g_fb_override_loaded = 1;
    int w = mcsm_cfg()->render_w, h = mcsm_cfg()->render_h;
    if (w > 0 && h > 0) {
        const int requested_w = w;
        const int requested_h = h;
        int sanitized = 0;
        sanitize_framebuffer_override(&w, &h, &sanitized);
        g_fb_override_enabled = 1;
        g_fb_override_width = w;
        g_fb_override_height = h;
        g_fb_width = w;
        g_fb_height = h;
        if (sanitized) {
            l_info("Render-scale: %dx%d (from graphics.txt, requested=%dx%d sanitized)", w, h, requested_w, requested_h);
        } else {
            l_info("Render-scale: %dx%d (from graphics.txt)", w, h);
        }
    } else {
        l_info("Render-scale: %dx%d (engine default)", g_fb_width, g_fb_height);
    }
}

static int clamp_audio_rate(int rate) {
    switch (rate) {
        case 8000: case 11025: case 12000: case 16000:
        case 22050: case 24000: case 32000: case 44100:
        case 48000: return rate;
        default: return 48000;
    }
}

static int clamp_audio_channels(int channels) { return (channels == 1) ? 1 : 2; }

static int clamp_audio_frames(int frames) {
    if (frames <= 0) frames = 1024;
    if (frames < SCE_AUDIO_MIN_LEN) frames = SCE_AUDIO_MIN_LEN;
    if (frames > 4096) frames = 4096;
    frames = (frames + 63) & ~63;
    if (frames > SCE_AUDIO_MAX_LEN) frames = SCE_AUDIO_MAX_LEN;
    return frames;
}

#define AUDIO_GAIN_Q8_DEFAULT 320 /* 1.25x. Vita hardware volume is already 0 dB. */

static int audio_gain_q8(void) {
    static int s_gain_q8 = -1;
    if (s_gain_q8 < 0) {
        int percent = 125;
        char path[256];
        snprintf(path, sizeof(path), DATA_PATH "audio_gain.txt");
        FILE *fp = fopen(path, "r");
        if (!fp) {
            fp = fopen("ux0:data/mcsm/audio_gain.txt", "r");
        }
        if (fp) {
            char buf[32];
            if (fgets(buf, sizeof(buf), fp)) {
                int requested = atoi(buf);
                if (requested >= 50 && requested <= 200) {
                    percent = requested;
                }
            }
            fclose(fp);
        }
        s_gain_q8 = (percent * 256 + 50) / 100;
        l_info("AUDIO gain=%d%%", percent);
    }
    return s_gain_q8;
}

static int16_t audio_apply_gain_i16(int16_t sample, int gain_q8) {
    int value = ((int)sample * gain_q8) / 256;
    if (value > 32767) {
        value = 32767;
    } else if (value < -32768) {
        value = -32768;
    }
    return (int16_t)value;
}

static void audio_write_sleep_us(int byte_count) {
    if (byte_count <= 0) return;
    int sample_rate = g_audio_sample_rate > 0 ? g_audio_sample_rate : 48000;
    int channels = g_audio_channels > 0 ? g_audio_channels : 2;
    int bytes_per_sample = g_audio_bytes_per_sample > 0 ? g_audio_bytes_per_sample : 2;
    int bytes_per_frame = channels * bytes_per_sample;
    if (bytes_per_frame <= 0) bytes_per_frame = 4;
    int64_t usec = ((int64_t)byte_count * 1000000LL) / ((int64_t)sample_rate * (int64_t)bytes_per_frame);
    if (usec < 1000) usec = 1000;
    else if (usec > 100000) usec = 100000;
    sceKernelDelayThread((unsigned int)usec);
}

void audio_close_port(void) {
    if (g_audio_port >= 0) {
        int rc = sceAudioOutReleasePort(g_audio_port);
        l_info("AUDIO close port=%d rc=0x%08X", g_audio_port, (unsigned)rc);
        g_audio_port = -1;
    }
}

static int audio_ensure_scratch(int samples) {
    if (samples <= g_audio_scratch_samples) return 1;
    int16_t *new_buf = realloc(g_audio_scratch, (size_t)samples * sizeof(int16_t));
    if (!new_buf) { l_error("AUDIO scratch alloc failed (%d samples).", samples); return 0; }
    g_audio_scratch = new_buf; g_audio_scratch_samples = samples;
    return 1;
}

int audio_open_port(int sample_rate, int channels, int desired_frames) {
    const int rate = clamp_audio_rate(sample_rate);
    const int out_channels = clamp_audio_channels(channels);
    const int frames = clamp_audio_frames(desired_frames);
    if (g_audio_port >= 0 && g_audio_port_rate == rate &&
        g_audio_port_channels == out_channels && g_audio_port_frames == frames)
        return g_audio_port_frames;
    audio_close_port();
    const SceAudioOutMode mode = (out_channels == 1) ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, frames, rate, mode);
    int actual_rate = rate;
    if (port < 0 && rate != 48000) {
        l_warn("AUDIO open failed rate=%d frames=%d ch=%d rc=0x%08X; retrying 48000.", rate, frames, out_channels, (unsigned)port);
        actual_rate = 48000;
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, frames, actual_rate, mode);
    }
    if (port < 0) {
        l_warn("AUDIO BGM port failed rate=%d frames=%d ch=%d rc=0x%08X; retrying MAIN.", actual_rate, frames, out_channels, (unsigned)port);
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, frames, actual_rate, mode);
    }
    if (port < 0) { l_error("AUDIO open failed rate=%d frames=%d ch=%d rc=0x%08X.", actual_rate, frames, out_channels, (unsigned)port); return frames; }
    g_audio_port = port; g_audio_port_frames = frames; g_audio_port_rate = actual_rate; g_audio_port_channels = out_channels;
    g_audio_sample_rate = actual_rate;
    g_audio_channels = out_channels;
    g_audio_bytes_per_sample = 2;
    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(g_audio_port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volume);
    l_info("AUDIO open port=%d rate=%d frames=%d ch=%d", g_audio_port, g_audio_port_rate, g_audio_port_frames, g_audio_port_channels);
    return g_audio_port_frames;
}

void audio_output_i16_frames(const int16_t *samples, int frames, int channels) {
    static unsigned log_count = 0;
    channels = clamp_audio_channels(channels);
    if (!samples || frames <= 0) {
        static unsigned s_null = 0;
        if (s_null++ < 4U) l_warn("AUDIO write_i16 null: samples=%p frames=%d", (void*)samples, frames);
        return;
    }
    audio_open_port(g_audio_sample_rate, channels, g_audio_port_frames);
    if (g_audio_port < 0) {
        static unsigned s_noport = 0;
        if (s_noport++ < 8U) l_error("AUDIO write_i16 no port! rate=%d ch=%d frames=%d", g_audio_sample_rate, channels, g_audio_port_frames);
        audio_write_sleep_us(frames * channels * 2);
        return;
    }
    if (log_count < 4U) { l_info("AUDIO write_i16 frames=%d ch=%d first=%d,%d port=%d", frames, channels, samples[0], (channels > 1 && frames > 0) ? samples[1] : samples[0], g_audio_port); log_count++; }
    if (log_count == 2U) {
        /* AUDIO DIAG: verify FMOD is producing real audio data on first buffer */
        int has_nonzero = 0;
        for (int i = 0; i < frames * channels && i < 128; i++) { if (samples[i] != 0) { has_nonzero = 1; break; } }
        l_info("AUDIO first buffer has_nonzero=%d (FMOD %s producing audio)", has_nonzero, has_nonzero ? "IS" : "NOT");
    }
    int offset_frames = 0;
    while (offset_frames < frames) {
        int chunk_frames = frames - offset_frames;
        if (chunk_frames > g_audio_port_frames) chunk_frames = g_audio_port_frames;
        const int samples_per_chunk = chunk_frames * channels;
        const int full_samples = g_audio_port_frames * channels;
        const int16_t *out = samples + (offset_frames * channels);
        const int gain_q8 = audio_gain_q8();
        if (chunk_frames != g_audio_port_frames || gain_q8 != 256) {
            if (!audio_ensure_scratch(full_samples)) { audio_write_sleep_us(chunk_frames * channels * 2); return; }
            if (gain_q8 == 256) {
                memcpy(g_audio_scratch, out, (size_t)samples_per_chunk * sizeof(int16_t));
            } else {
                for (int i = 0; i < samples_per_chunk; ++i) {
                    g_audio_scratch[i] = audio_apply_gain_i16(out[i], gain_q8);
                }
            }
            memset(g_audio_scratch + samples_per_chunk, 0, (size_t)(full_samples - samples_per_chunk) * sizeof(int16_t));
            out = g_audio_scratch;
        }
        int rc = sceAudioOutOutput(g_audio_port, out);
        if (rc < 0) { l_error("AUDIO output failed rc=0x%08X.", (unsigned)rc); audio_write_sleep_us(chunk_frames * channels * 2); return; }
        offset_frames += chunk_frames;
    }
}

static void audio_output_byte_buffer(const jbyte *bytes, int byte_count) {
    const int channels = clamp_audio_channels(g_audio_channels);
    const int bps = (g_audio_bytes_per_sample == 1 || g_audio_bytes_per_sample == 4) ? g_audio_bytes_per_sample : 2;
    const int bytes_per_frame = channels * bps;
    if (!bytes || byte_count < bytes_per_frame) return;
    audio_open_port(g_audio_sample_rate, channels, g_audio_port_frames);
    int frames = byte_count / bytes_per_frame;
    int offset_frames = 0;
    while (offset_frames < frames) {
        int chunk_frames = frames - offset_frames;
        if (chunk_frames > g_audio_port_frames) chunk_frames = g_audio_port_frames;
        const int chunk_samples = chunk_frames * channels;
        const int scratch_samples = g_audio_port_frames * channels;
        if (!audio_ensure_scratch(scratch_samples)) { audio_write_sleep_us(chunk_frames * bytes_per_frame); return; }
        const jbyte *src = bytes + (offset_frames * bytes_per_frame);
        for (int i = 0; i < chunk_samples; i++) {
            if (bps == 1) g_audio_scratch[i] = (int16_t)(((int)((uint8_t)src[i]) - 128) << 8);
            else if (bps == 4) { int32_t value = ((int32_t)(uint8_t)src[i*4]) | ((int32_t)(uint8_t)src[i*4+1]<<8) | ((int32_t)(uint8_t)src[i*4+2]<<16) | ((int32_t)(uint8_t)src[i*4+3]<<24); g_audio_scratch[i] = (int16_t)(value>>16); }
            else g_audio_scratch[i] = (int16_t)(((uint16_t)(uint8_t)src[i*2]) | ((uint16_t)(uint8_t)src[i*2+1]<<8));
        }
        audio_output_i16_frames(g_audio_scratch, chunk_frames, channels);
        offset_frames += chunk_frames;
    }
}

static jobject ret_string(const char *value) {
    ensure_runtime_fields();
    if (!value) return NULL;
    return jni->NewStringUTF(&jni, value);
}

static jobject GetExternalStorageDirectory(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("/sdcard"); }
static jobject GetPackageName(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("com.telltalegames.minecraft100"); }
static jobject GetObbFileName(jmethodID id, va_list args) { (void)id; const int is_main = va_arg(args, int); if (is_main) return ret_string(DATA_PATH "main.40129.com.telltalegames.minecraft100.obb"); return ret_string(DATA_PATH "patch.40135.com.telltalegames.minecraft100.obb"); }
static jobject GetExternalStoragePath(jmethodID id, va_list args) { (void)id; (void)args; return ret_string(DATA_PATH); }
static jobject GetInternalStoragePath(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("/data/data/com.telltalegames.minecraft100/files"); }
static jobject GetContext(jmethodID id, va_list args) { (void)id; (void)args; ensure_runtime_fields(); return g_context_obj; }
static jobject GetApplicationContext(jmethodID id, va_list args) { (void)id; (void)args; ensure_runtime_fields(); return g_context_obj; }
static jobject GetAssets(jmethodID id, va_list args) { (void)id; (void)args; ensure_runtime_fields(); return g_asset_manager_obj; }
static jobject GetNativeSurface(jmethodID id, va_list args) { (void)id; (void)args; ensure_runtime_fields(); return g_native_surface_obj; }
static jobject GetExternalStorageState(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("mounted"); }
static jobject GetExternalStorageDirs(jmethodID id, va_list args) { (void)id; (void)args; jobjectArray arr = jni->NewObjectArray(&jni, 1, NULL, NULL); jni->SetObjectArrayElement(&jni, arr, 0, ret_string(DATA_PATH)); return arr; }
static jobject GetFilesDir(jmethodID id, va_list args) { (void)id; (void)args; return ret_string(DATA_PATH); }
static jobject GetAbsolutePath(jmethodID id, va_list args) { (void)id; (void)args; return ret_string(DATA_PATH); }

static void FlipBuffers(jmethodID id, va_list args) {
    (void)id; (void)args; gl_swap(); g_flipbuffers_count++;
    if (g_flipbuffers_count <= 8 || (g_flipbuffers_count & 0x7fU) == 0U) l_info("FlipBuffers count=%u", g_flipbuffers_count);
}

static jint AudioInit(jmethodID id, va_list args) {
    (void)id;
    const int sample_rate = va_arg(args, int), is_16bit = va_arg(args, int), is_stereo = va_arg(args, int), desired_frames = va_arg(args, int);
    g_audio_sample_rate = sample_rate > 0 ? sample_rate : 48000;
    g_audio_channels = is_stereo ? 2 : 1; g_audio_bytes_per_sample = is_16bit ? 2 : 1;
    int frames = audio_open_port(g_audio_sample_rate, g_audio_channels, desired_frames);
    l_info("AUDIO AudioTrack.init rate=%d bits=%d stereo=%d desired=%d -> frames=%d", g_audio_sample_rate, is_16bit ? 16 : 8, is_stereo, desired_frames, frames);
    return frames;
}

static void AudioWriteShortBuffer(jmethodID id, va_list args) {
    (void)id; jobject buffer = va_arg(args, jobject);
    int sample_count = buffer ? jni->GetArrayLength(&jni, buffer) : 0;
    { static unsigned int s_aws = 0; if (s_aws++ < 4U) l_info("AUDIO writeShort #%u samples=%d (FMOD driving audio)", s_aws, sample_count); }
    if (sample_count <= 0) sample_count = 1024;
    jshort *samples = jni->GetShortArrayElements(&jni, (jshortArray)buffer, NULL);
    if (samples) { audio_output_i16_frames(samples, sample_count / clamp_audio_channels(g_audio_channels), clamp_audio_channels(g_audio_channels)); jni->ReleaseShortArrayElements(&jni, (jshortArray)buffer, samples, 0); }
    else audio_write_sleep_us(sample_count * 2);
}

static void AudioWriteByteBuffer(jmethodID id, va_list args) {
    (void)id; jobject buffer = va_arg(args, jobject);
    int byte_count = buffer ? jni->GetArrayLength(&jni, buffer) : 0;
    { static unsigned int s_awb = 0; if (s_awb++ < 4U) l_info("AUDIO writeByte #%u bytes=%d (FMOD driving audio)", s_awb, byte_count); }
    if (byte_count <= 0) byte_count = 4096;
    jbyte *bytes = jni->GetByteArrayElements(&jni, (jbyteArray)buffer, NULL);
    if (bytes) { audio_output_byte_buffer(bytes, byte_count); jni->ReleaseByteArrayElements(&jni, (jbyteArray)buffer, bytes, 0); }
    else audio_write_sleep_us(byte_count);
}

static void AudioQuit(jmethodID id, va_list args) { (void)id; (void)args; audio_close_port(); }
extern void mcsm_register_virtual_controller(void);
static void PollInputDevices(jmethodID id, va_list args) { (void)id; (void)args; launch_state_mark_poll(); mcsm_register_virtual_controller(); }

static void UpdatePurchases(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 16U) { l_info("DLC updatePurchases -> no-op"); log_count++; }
}

static jboolean IsNetworkAvailable(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 16U) { l_info("DLC isNetworkAvailable -> false (offline local-data mode)"); log_count++; }
    return JNI_FALSE;
}

static jobject GetPurchaseProvider(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 8U) { l_info("DLC getPurchaseProvider -> Amazon"); log_count++; }
    return ret_string("Amazon");
}

static void RequestPermission(jmethodID id, va_list args) {
    (void)id; int permission_code = va_arg(args, int);
    if (g_request_permission_logged < 16) { l_info("DLC requestPermission(%d) -> granted", permission_code); g_request_permission_logged++; }
}

static jboolean IsPurchased(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; jobject sku = va_arg(args, jobject);
    const char *sku_name = sku ? jni->GetStringUTFChars(&jni, (jstring)sku, NULL) : NULL;
    if (log_count < 16U) { l_info("DLC purchase check(\"%s\") -> true", sku_name ? sku_name : "(null)"); log_count++; }
    if (sku_name) jni->ReleaseStringUTFChars(&jni, (jstring)sku, (char *)sku_name);
    return JNI_TRUE;
}

static void Purchase(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 8U) { l_info("DLC purchase -> no-op, already owned locally"); log_count++; }
}

static void OnPurchase(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 8U) { l_info("DLC onPurchase -> no-op"); log_count++; }
}

static void OnUnlockAchievement(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

static jobject GetPurchasedSkus(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    const char *skus[] = { "MCSM_Episode_101","MCSM_Episode_102","MCSM_Episode_103","MCSM_Episode_104","MCSM_Episode_105","MCSM_Episode_106","MCSM_Episode_107","MCSM_Episode_108" };
    const int num_skus = sizeof(skus)/sizeof(skus[0]);
    jobjectArray arr = jni->NewObjectArray(&jni, num_skus, NULL, NULL);
    if (!arr) return NULL;
    for (int i = 0; i < num_skus; i++) { jobject str = jni->NewStringUTF(&jni, skus[i]); jni->SetObjectArrayElement(&jni, arr, i, str); jni->DeleteLocalRef(&jni, str); }
    if (log_count < 4U) { l_info("DLC getPurchasedSkus -> [%d skus]", num_skus); log_count++; }
    return arr;
}

// Download manager stubs: the game's DownloadManager probes Google Play
// service endpoints for OBB download progress and license verification.
// On Vita all data is pre-installed at ux0:data/mcsm/ — immediately report
// "downloaded, not downloading, 100% progress, license valid".
static jboolean IsDownloaded(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 8U) { l_info("DLC isDownloaded -> true"); log_count++; }
    return JNI_TRUE;
}

static jboolean IsDownloading(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 4U) { l_info("DLC isDownloading -> false"); log_count++; }
    return JNI_FALSE;
}

static jint GetDownloadProgress(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 4U) { l_info("DLC getDownloadProgress -> 100%%"); log_count++; }
    return 100;
}

static jboolean CheckLicense(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; jobject sku = va_arg(args, jobject);
    const char *sku_name = sku ? jni->GetStringUTFChars(&jni, (jstring)sku, NULL) : NULL;
    if (log_count < 16U) { l_info("DLC checkLicense(\"%s\") -> valid", sku_name ? sku_name : "(null)"); log_count++; }
    if (sku_name) jni->ReleaseStringUTFChars(&jni, (jstring)sku, (char *)sku_name);
    return JNI_TRUE;
}

static jobject InputGetInputDeviceIds(jmethodID id, va_list args) {
    (void)id; const int source_mask = va_arg(args, int);
    jintArray devices = jni->NewIntArray(&jni, 1); if (!devices) return NULL;
    const jint vita_device_id = 0;
    jni->SetIntArrayRegion(&jni, devices, 0, 1, &vita_device_id);
    l_info("inputGetInputDeviceIds(source=0x%08X) -> [0]", (unsigned)source_mask);
    return devices;
}

static jobject GetHardwareModel(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("PlayStation Vita"); }
static jboolean SetActivityTitle(jmethodID id, va_list args) { (void)id; (void)args; return JNI_TRUE; }

static jboolean HasFeature(jmethodID id, va_list args) {
    (void)id; jobject feature = va_arg(args, jobject);
    const char *feature_name = feature ? jni->GetStringUTFChars(&jni, (jstring)feature, NULL) : NULL;
    const int is_low_latency = feature_name && (strstr(feature_name, "audio.low_latency") || strstr(feature_name, "FEATURE_AUDIO_LOW_LATENCY"));
    static unsigned log_count = 0;
    if (log_count < 4U) { l_info("Feature query \"%s\" -> false%s", feature_name ? feature_name : "(null)", is_low_latency ? " (low-latency disabled for FMOD output)" : ""); log_count++; }
    if (feature_name) jni->ReleaseStringUTFChars(&jni, (jstring)feature, (char *)feature_name);
    return JNI_FALSE;
}

static void SetFramebufferSize(jmethodID id, va_list args) {
    (void)id; ensure_framebuffer_override_loaded(); int w = va_arg(args, int), h = va_arg(args, int);
    if (g_fb_override_enabled) { g_fb_width = g_fb_override_width; g_fb_height = g_fb_override_height; l_info("SetFramebufferSize(%d,%d) -> render-scale %dx%d", w, h, g_fb_width, g_fb_height); return; }
    g_fb_width = MCSM_DEFAULT_RENDER_W; g_fb_height = MCSM_DEFAULT_RENDER_H;
    l_info("SetFramebufferSize(%d,%d) -> native %dx%d", w, h, g_fb_width, g_fb_height);
}

static jint GetSampleRate(jmethodID id, va_list args) { (void)id; (void)args; return 48000; }
static jint GetOutputFramesPerBuffer(jmethodID id, va_list args) { (void)id; (void)args; return 1024; }
static jint GetOutputSampleRate(jmethodID id, va_list args) { (void)id; (void)args; return 48000; }
static jint GetOutputBlockSize(jmethodID id, va_list args) { (void)id; (void)args; return 1024; }
static jboolean IsUsingBluetooth(jmethodID id, va_list args) { (void)id; (void)args; return JNI_FALSE; }
static jboolean CheckInit(jmethodID id, va_list args) { (void)id; (void)args; return JNI_TRUE; }

static jboolean IsDataAvailable(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 16U) { l_info("DLC isDataAvailable -> true"); log_count++; }
    return JNI_TRUE;
}

static jboolean IsTV(jmethodID id, va_list args) { (void)id; (void)args; return JNI_FALSE; }
static jboolean SupportsLowLatency(jmethodID id, va_list args) { (void)id; (void)args; return JNI_FALSE; }
static jboolean IsSignedIn(jmethodID id, va_list args) {
    static unsigned log_count = 0; (void)id; (void)args;
    if (log_count < 8U) { l_info("JNI isSignedIn -> true (#%u)", log_count + 1U); log_count++; }
    return JNI_TRUE;
}

static jobject GetAssetManager(jmethodID id, va_list args) { (void)id; (void)args; ensure_runtime_fields(); return g_asset_manager_obj; }
static jobject FmodAudioDeviceInit(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("fmod_audio_device"); }
static jobject GetLocale(jmethodID id, va_list args) {
    (void)id; (void)args;
    /* Language toggle: settings/language.txt holds a locale the engine maps to one
     * of the shipped packs, e.g. en_US, fr_FR, de_DE, ru_RU, zh_CN, es_ES, pt_BR.
     * Absent/empty -> English. Read once, cached. */
    static char loc[16] = "";
    static int logged = 0;
    if (!loc[0]) {
        strncpy(loc, mcsm_game()->language, sizeof(loc) - 1);
        if (!loc[0]) { strcpy(loc, "en_US"); }
    }
    if (!logged) { logged = 1; l_info("LANG: getLocale -> \"%s\"", loc); }
    return ret_string(loc);
}
static jobject GetHardwareOS(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("Android 4.4.4"); }
static jobject GetHardwareBoard(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("vita"); }

static jboolean AudioDeviceInit(jmethodID id, va_list args) {
    (void)id; int sample_rate = va_arg(args, int), channels_or_config = va_arg(args, int), bits_or_encoding = va_arg(args, int), desired_frames = va_arg(args, int);
    if (sample_rate > 0) g_audio_sample_rate = sample_rate;
    if (channels_or_config == 1 || channels_or_config == 2) g_audio_channels = channels_or_config;
    else if (channels_or_config == 4) g_audio_channels = 1; else if (channels_or_config == 12) g_audio_channels = 2;
    if (bits_or_encoding == 8 || bits_or_encoding == 3) g_audio_bytes_per_sample = 1;
    else if (bits_or_encoding == 16 || bits_or_encoding == 2) g_audio_bytes_per_sample = 2;
    else if (bits_or_encoding == 32 || bits_or_encoding == 4) g_audio_bytes_per_sample = 4;
    audio_open_port(g_audio_sample_rate, g_audio_channels, desired_frames);
    l_info("AUDIO AudioDevice.init args=%d,%d,%d,%d -> rate=%d ch=%d bps=%d frames=%d", sample_rate, channels_or_config, bits_or_encoding, desired_frames, g_audio_sample_rate, g_audio_channels, g_audio_bytes_per_sample, g_audio_port_frames);
    return JNI_TRUE;
}

static void AudioDeviceClose(jmethodID id, va_list args) { (void)id; (void)args; audio_close_port(); }
static void AudioDeviceWrite(jmethodID id, va_list args) {
    (void)id; jobject buffer = va_arg(args, jobject); int size = va_arg(args, int);
    int byte_count = buffer ? jni->GetArrayLength(&jni, buffer) : 0;
    if (size > 0 && (byte_count == 0 || size < byte_count)) byte_count = size;
    if (size <= 0) size = 4096;
    jbyte *bytes = buffer ? jni->GetByteArrayElements(&jni, (jbyteArray)buffer, NULL) : NULL;
    if (bytes && byte_count > 0) { audio_output_byte_buffer(bytes, byte_count); jni->ReleaseByteArrayElements(&jni, (jbyteArray)buffer, bytes, 0); }
    else audio_write_sleep_us(size);
}

static jobject GetHardwareDisplay(jmethodID id, va_list args) { (void)id; (void)args; ensure_framebuffer_override_loaded(); char buf[32]; snprintf(buf, sizeof(buf), "%dx%d", g_fb_width, g_fb_height); return ret_string(buf); }
static jobject GetHardwareManufacturer(jmethodID id, va_list args) { (void)id; (void)args; return ret_string("Sony"); }
/* DPI scaled with the render resolution: the engine sizes UI in density-independent
 * pixels (dp = px*160/dpi), so at a lower render res we must report a proportionally
 * lower DPI, otherwise UI elements keep their native pixel size and overflow the
 * smaller screen (the "overscan"). At native (g_fb_width==960) this is exactly 220. */
static jfloat mcsm_scaled_dpi(void) { ensure_framebuffer_override_loaded(); return 220.0f * (float)g_fb_width / 960.0f; }
static jfloat GetXDPI(jmethodID id, va_list args) { (void)id; (void)args; float d = mcsm_scaled_dpi(); if (!g_display_metrics_logged) { l_info("Display metrics: fb=%dx%d dpi=%.1f (native 220 scaled by render res)", g_fb_width, g_fb_height, d); g_display_metrics_logged = 1; } return d; }
static jfloat GetYDPI(jmethodID id, va_list args) { (void)id; (void)args; return mcsm_scaled_dpi(); }
static jint GetWidth(jmethodID id, va_list args) { (void)id; (void)args; ensure_framebuffer_override_loaded(); return g_fb_width; }
static jint GetHeight(jmethodID id, va_list args) { (void)id; (void)args; ensure_framebuffer_override_loaded(); return g_fb_height; }

int mcsm_get_framebuffer_width(void) { ensure_framebuffer_override_loaded(); return (g_fb_width > 0) ? g_fb_width : MCSM_DEFAULT_RENDER_W; }
int mcsm_get_framebuffer_height(void) { ensure_framebuffer_override_loaded(); return (g_fb_height > 0) ? g_fb_height : MCSM_DEFAULT_RENDER_H; }
/* Render-scale = the low-res the GAME's frame is rendered at into the FBO before
 * upscaling to native. From fb_override.txt, independent of the game's logical res. */
int mcsm_get_render_scale_width(void) { ensure_framebuffer_override_loaded(); return g_fb_override_enabled ? g_fb_override_width : MCSM_DEFAULT_RENDER_W; }
int mcsm_get_render_scale_height(void) { ensure_framebuffer_override_loaded(); return g_fb_override_enabled ? g_fb_override_height : MCSM_DEFAULT_RENDER_H; }

/*
 * JNI Methods
 */
NameToMethodID nameToMethodId[] = {
    { MID_GET_EXTERNAL_STORAGE_DIRECTORY, "getExternalStorageDirectory", METHOD_TYPE_OBJECT },
    { MID_GET_PACKAGE_NAME, "getPackageName", METHOD_TYPE_OBJECT },
    { MID_GET_OBB_FILENAME, "getObbFileName", METHOD_TYPE_OBJECT },
    { MID_GET_EXTERNAL_STORAGE_PATH, "getExternalStoragePath", METHOD_TYPE_OBJECT },
    { MID_GET_INTERNAL_STORAGE_PATH, "getInternalStoragePath", METHOD_TYPE_OBJECT },
    { MID_GET_ASSETS, "getAssets", METHOD_TYPE_OBJECT },
    { MID_GET_CONTEXT, "getContext", METHOD_TYPE_OBJECT },
    { MID_GET_APPLICATION_CONTEXT, "getApplicationContext", METHOD_TYPE_OBJECT },
    { MID_GET_NATIVE_SURFACE, "getNativeSurface", METHOD_TYPE_OBJECT },
    { MID_FLIP_BUFFERS, "flipBuffers", METHOD_TYPE_VOID },
    { MID_AUDIO_INIT, "audioInit", METHOD_TYPE_INT },
    { MID_AUDIO_WRITE_SHORT_BUFFER, "audioWriteShortBuffer", METHOD_TYPE_VOID },
    { MID_AUDIO_WRITE_BYTE_BUFFER, "audioWriteByteBuffer", METHOD_TYPE_VOID },
    { MID_AUDIO_QUIT, "audioQuit", METHOD_TYPE_VOID },
    { MID_POLL_INPUT_DEVICES, "pollInputDevices", METHOD_TYPE_VOID },
    { MID_INPUT_GET_INPUT_DEVICE_IDS, "inputGetInputDeviceIds", METHOD_TYPE_OBJECT },
    { MID_GET_HARDWARE_MODEL, "getHardwareModel", METHOD_TYPE_OBJECT },
    { MID_SET_ACTIVITY_TITLE, "setActivityTitle", METHOD_TYPE_BOOLEAN },
    { MID_HAS_FEATURE, "hasFeature", METHOD_TYPE_BOOLEAN },
    { MID_SET_FRAMEBUFFER_SIZE, "setFramebufferSize", METHOD_TYPE_VOID },
    { MID_GET_SAMPLE_RATE, "getSampleRate", METHOD_TYPE_INT },
    { MID_GET_OUTPUT_FRAMES_PER_BUFFER, "getOutputFramesPerBuffer", METHOD_TYPE_INT },
    { MID_IS_USING_BLUETOOTH, "isUsingBluetooth", METHOD_TYPE_BOOLEAN },
    { MID_GET_HARDWARE_DISPLAY, "getHardwareDisplay", METHOD_TYPE_OBJECT },
    { MID_GET_HARDWARE_MANUFACTURER, "getHardwareManufacturer", METHOD_TYPE_OBJECT },
    { MID_GET_XDPI, "getXDPI", METHOD_TYPE_FLOAT },
    { MID_GET_YDPI, "getYDPI", METHOD_TYPE_FLOAT },
    { MID_GET_WIDTH, "getWidth", METHOD_TYPE_INT },
    { MID_GET_HEIGHT, "getHeight", METHOD_TYPE_INT },
    { MID_GET_SCREEN_WIDTH, "getScreenWidth", METHOD_TYPE_INT },
    { MID_GET_SCREEN_HEIGHT, "getScreenHeight", METHOD_TYPE_INT },
    { MID_GET_SURFACE_WIDTH, "getSurfaceWidth", METHOD_TYPE_INT },
    { MID_GET_SURFACE_HEIGHT, "getSurfaceHeight", METHOD_TYPE_INT },
    { MID_GET_EXTERNAL_STORAGE_STATE, "getExternalStorageState", METHOD_TYPE_OBJECT },
    { MID_GET_EXTERNAL_STORAGE_DIRS, "getExternalStorageDirs", METHOD_TYPE_OBJECT },
    { MID_GET_FILES_DIR, "getFilesDir", METHOD_TYPE_OBJECT },
    { MID_GET_ABSOLUTE_PATH, "getAbsolutePath", METHOD_TYPE_OBJECT },
    { MID_CHECK_INIT, "checkInit", METHOD_TYPE_BOOLEAN },
    { MID_SUPPORTS_LOW_LATENCY, "supportsLowLatency", METHOD_TYPE_BOOLEAN },
    { MID_GET_ASSET_MANAGER, "getAssetManager", METHOD_TYPE_OBJECT },
    { MID_FMOD_AUDIODEVICE_INIT, "org/fmod/AudioDevice/<init>", METHOD_TYPE_OBJECT },
    { MID_GET_LOCALE, "getLocale", METHOD_TYPE_OBJECT },
    { MID_GET_HARDWARE_OS, "getHardwareOS", METHOD_TYPE_OBJECT },
    { MID_GET_HARDWARE_BOARD, "getHardwareBoard", METHOD_TYPE_OBJECT },
    { MID_AUDIO_DEVICE_INIT, "init", METHOD_TYPE_BOOLEAN },
    { MID_AUDIO_DEVICE_CLOSE, "close", METHOD_TYPE_VOID },
    { MID_AUDIO_DEVICE_WRITE, "write", METHOD_TYPE_VOID },
    { MID_IS_DATA_AVAILABLE, "isDataAvailable", METHOD_TYPE_BOOLEAN },
    { MID_IS_TV, "isTV", METHOD_TYPE_BOOLEAN },
    { MID_UPDATE_PURCHASES, "updatePurchases", METHOD_TYPE_VOID },
    { MID_GET_OUTPUT_SAMPLE_RATE, "getOutputSampleRate", METHOD_TYPE_INT },
    { MID_GET_OUTPUT_BLOCK_SIZE, "getOutputBlockSize", METHOD_TYPE_INT },
    { MID_IS_PURCHASED, "isPurchased", METHOD_TYPE_BOOLEAN },
    { MID_IS_PRODUCT_PURCHASED, "isProductPurchased", METHOD_TYPE_BOOLEAN },
    { MID_GET_PURCHASED_SKUS, "getPurchasedSkus", METHOD_TYPE_OBJECT },
    { MID_GET_PURCHASE_PROVIDER, "getPurchaseProvider", METHOD_TYPE_OBJECT },
    { MID_REQUEST_PERMISSION, "requestPermission", METHOD_TYPE_VOID },
    { MID_PURCHASE, "purchase", METHOD_TYPE_VOID },
    { MID_ON_PURCHASE, "onPurchase", METHOD_TYPE_VOID },
    { MID_ON_UNLOCK_ACHIEVEMENT, "onUnlockAchievement", METHOD_TYPE_VOID },
    { MID_IS_SIGNED_IN, "isSignedIn", METHOD_TYPE_BOOLEAN },
    { MID_IS_DOWNLOADED, "isDownloaded", METHOD_TYPE_BOOLEAN },
    { MID_IS_DOWNLOADING, "isDownloading", METHOD_TYPE_BOOLEAN },
    { MID_GET_DOWNLOAD_PROGRESS, "getDownloadProgress", METHOD_TYPE_INT },
    { MID_CHECK_LICENSE, "checkLicense", METHOD_TYPE_BOOLEAN },
    { MID_IS_NETWORK_AVAILABLE, "isNetworkAvailable", METHOD_TYPE_BOOLEAN },
};

MethodsBoolean methodsBoolean[] = {
    { MID_SET_ACTIVITY_TITLE, SetActivityTitle },
    { MID_HAS_FEATURE, HasFeature },
    { MID_IS_USING_BLUETOOTH, IsUsingBluetooth },
    { MID_CHECK_INIT, CheckInit },
    { MID_SUPPORTS_LOW_LATENCY, SupportsLowLatency },
    { MID_AUDIO_DEVICE_INIT, AudioDeviceInit },
    { MID_IS_DATA_AVAILABLE, IsDataAvailable },
    { MID_IS_TV, IsTV },
    { MID_IS_PURCHASED, IsPurchased },
    { MID_IS_PRODUCT_PURCHASED, IsPurchased },
    { MID_IS_DOWNLOADED, IsDownloaded },
    { MID_IS_DOWNLOADING, IsDownloading },
    { MID_CHECK_LICENSE, CheckLicense },
    { MID_IS_NETWORK_AVAILABLE, IsNetworkAvailable },
    { MID_IS_SIGNED_IN, IsSignedIn },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = { { MID_GET_XDPI, GetXDPI }, { MID_GET_YDPI, GetYDPI }, };
MethodsInt methodsInt[] = {
    { MID_AUDIO_INIT, AudioInit },
    { MID_GET_SAMPLE_RATE, GetSampleRate },
    { MID_GET_OUTPUT_FRAMES_PER_BUFFER, GetOutputFramesPerBuffer },
    { MID_GET_WIDTH, GetWidth }, { MID_GET_HEIGHT, GetHeight },
    { MID_GET_SCREEN_WIDTH, GetWidth }, { MID_GET_SCREEN_HEIGHT, GetHeight },
    { MID_GET_SURFACE_WIDTH, GetWidth }, { MID_GET_SURFACE_HEIGHT, GetHeight },
    { MID_GET_OUTPUT_SAMPLE_RATE, GetOutputSampleRate },
    { MID_GET_OUTPUT_BLOCK_SIZE, GetOutputBlockSize },
    { MID_GET_DOWNLOAD_PROGRESS, GetDownloadProgress },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { MID_GET_EXTERNAL_STORAGE_DIRECTORY, GetExternalStorageDirectory },
    { MID_GET_PACKAGE_NAME, GetPackageName },
    { MID_GET_OBB_FILENAME, GetObbFileName },
    { MID_GET_EXTERNAL_STORAGE_PATH, GetExternalStoragePath },
    { MID_GET_INTERNAL_STORAGE_PATH, GetInternalStoragePath },
    { MID_GET_ASSETS, GetAssets },
    { MID_GET_CONTEXT, GetContext },
    { MID_GET_APPLICATION_CONTEXT, GetApplicationContext },
    { MID_GET_NATIVE_SURFACE, GetNativeSurface },
    { MID_INPUT_GET_INPUT_DEVICE_IDS, InputGetInputDeviceIds },
    { MID_GET_HARDWARE_MODEL, GetHardwareModel },
    { MID_GET_HARDWARE_DISPLAY, GetHardwareDisplay },
    { MID_GET_HARDWARE_MANUFACTURER, GetHardwareManufacturer },
    { MID_GET_EXTERNAL_STORAGE_STATE, GetExternalStorageState },
    { MID_GET_EXTERNAL_STORAGE_DIRS, GetExternalStorageDirs },
    { MID_GET_FILES_DIR, GetFilesDir },
    { MID_GET_ABSOLUTE_PATH, GetAbsolutePath },
    { MID_GET_ASSET_MANAGER, GetAssetManager },
    { MID_FMOD_AUDIODEVICE_INIT, FmodAudioDeviceInit },
    { MID_GET_LOCALE, GetLocale },
    { MID_GET_HARDWARE_OS, GetHardwareOS },
    { MID_GET_HARDWARE_BOARD, GetHardwareBoard },
    { MID_GET_PURCHASED_SKUS, GetPurchasedSkus },
    { MID_GET_PURCHASE_PROVIDER, GetPurchaseProvider },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { MID_FLIP_BUFFERS, FlipBuffers },
    { MID_AUDIO_WRITE_SHORT_BUFFER, AudioWriteShortBuffer },
    { MID_AUDIO_WRITE_BYTE_BUFFER, AudioWriteByteBuffer },
    { MID_AUDIO_QUIT, AudioQuit },
    { MID_POLL_INPUT_DEVICES, PollInputDevices },
    { MID_SET_FRAMEBUFFER_SIZE, SetFramebufferSize },
    { MID_AUDIO_DEVICE_CLOSE, AudioDeviceClose },
    { MID_AUDIO_DEVICE_WRITE, AudioDeviceWrite },
    { MID_UPDATE_PURCHASES, UpdatePurchases },
    { MID_REQUEST_PERMISSION, RequestPermission },
    { MID_PURCHASE, Purchase },
    { MID_ON_PURCHASE, OnPurchase },
    { MID_ON_UNLOCK_ACHIEVEMENT, OnUnlockAchievement },
};

/*
 * JNI Fields
 */
char WINDOW_SERVICE[] = "window";
const int SDK_INT = 19;

NameToFieldID nameToFieldId[] = {
    { FID_WINDOW_SERVICE, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
    { FID_SDK_INT, "SDK_INT", FIELD_TYPE_INT },
    { FID_M_ASSET_MGR, "mAssetMgr", FIELD_TYPE_OBJECT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = { { FID_SDK_INT, SDK_INT }, };
FieldsObject fieldsObject[] = { { FID_WINDOW_SERVICE, WINDOW_SERVICE }, { FID_M_ASSET_MGR, (jobject)0x1 }, };
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

static void sync_runtime_field_objects(void) {
    for (size_t i = 0; i < sizeof(fieldsObject) / sizeof(fieldsObject[0]); ++i) {
        if (fieldsObject[i].id == FID_M_ASSET_MGR) { fieldsObject[i].value = g_asset_manager_obj; break; }
    }
}

__FALSOJNI_IMPL_CONTAINER_SIZES
