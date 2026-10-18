// Payload for PICO's out-of-process OpenXR runtime (com.pico.xr.openxr_runtime).
//
// The platform tracking service recognises the system hand gestures (recenter /
// open dock) and pushes "hand gesture state" into the runtime process, which then
// hands every session an XR_DEVICE_SYSTEMGESTURE_STATE event. The shell is one of
// those sessions and performs the action, so the gesture only happens if that
// event reaches it.
//
// Nothing here tries to change what the platform decides. Instead the runtime's
// own event emitter is hooked: while Virtual Desktop owns the MR service the
// event is dropped before it is delivered to any session, so the shell never
// learns about the gesture. See vd_active() for the scoping.
//
// Nothing here touches VD: this library only runs in the runtime process.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>
#include <elf.h>
#include <sys/system_properties.h>

#include "shadowhook.h"
#include "vdhs_sym.h"

#define TAG "vdhs-gesture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Runtime library that turns the platform's gesture state into the session
// event. It is not exported, so it is found through the log string it prints
// (offsets are for the runtime shipped in 5.13.7).
static const char* kRtEmitString = "handgesturestate runtime: gesturestate->handgestureenable = %d";
// Second string from the same table, used to confirm the library is the right
// one before anything is hooked.
static const char* kRtEmitString2 = "handgesturestate runtime: controllerevent.status  = %d";
#define kRtStringOff  0x13cdc2u   // file offset of kRtEmitString
#define kRtString2Off 0x13ce01u   // file offset of kRtEmitString2
#define kRtEmitOff    0x0a8c2cu   // file offset of the emitter function

// void emit(unsigned char* state) — state[0] is the gesture enable bit.
typedef void (*PFN_emit)(unsigned char*);
static PFN_emit g_orig_emit;

// Fallback interception point (see rt_thread): the platform's push into the
// input manager, InputDeviceManager::service_status_callback(self, key, value).
//   key 1: hand gesture state        <- *value = handgestureenable (observed id 154)
//   key 2: controller event status   <- same enable bit
//   key 0: active input device change
typedef void (*PFN_service_status)(void*, int, void*);
static PFN_service_status g_orig_status;

static const char* kInputMgr = "libpxrinputmanager.so";
static const char* kStatusSymMangled = "_Z23service_status_callbackPviS_";
static const char* kStatusSymPlain = "service_status_callback";
static const char* kVdProcess = "VirtualDesktop.Android";

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Virtual Desktop is the focused app: its process is alive and Android has not
// demoted it to a cached one (cached apps sit at oom_score_adj 700+, the focused
// app at 0). Checked because PICO's MR-service property below only flips a few
// seconds after the app starts.
static int vd_process_foreground(void) {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64];
        char buf[256];
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        if (!strstr(buf, kVdProcess)) continue;
        snprintf(path, sizeof path, "/proc/%s/oom_score_adj", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        if (atoi(buf) <= 100) found = 1;
    }
    closedir(d);
    return found;
}

static int vd_active(void) {
    char v[PROP_VALUE_MAX] = {0};
    // 'mrserivce' is PICO's spelling.
    if (__system_property_get("sys.pxr.mrserivce.user", v) > 0 && strcmp(v, kVdProcess) == 0)
        return 1;
    if (__system_property_get("sys.pxr.mrservice.user", v) > 0 && strcmp(v, kVdProcess) == 0)
        return 1;
    static int64_t checked;
    static int cached;
    int64_t now = now_ms();
    if (now - checked >= 1000) {
        checked = now;
        cached = vd_process_foreground();
    }
    return cached;
}

static void my_gesture_emit(unsigned char* state) {
    if (state && vd_active()) {
        LOGI("gesture: dropped system gesture event (state=%d) while VD runs", *state);
        return;
    }
    g_orig_emit(state);
}

static void my_service_status(void* self, int key, void* value) {
    if (value && (key == 1 || key == 2)) {
        int v = *(int*)value;
        if (v != 0 && vd_active()) {
            *(int*)value = 0;
            LOGI("gesture: key=%d handgestureenable %d -> 0 (VD active)", key, v);
        }
    }
    g_orig_status(self, key, value);
}

static int count_map_regions(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) n++;
    fclose(f);
    return n;
}

static int in_segment(const ElfW(Phdr)* ph, uint32_t off, size_t len) {
    return ph->p_vaddr <= off && (uint64_t)off + len <= ph->p_vaddr + ph->p_filesz;
}

// Locate the emitter. The runtime's libraries are mapped straight out of
// XRRuntime.apk, so no path names them; the candidate load bases come from the
// ELF headers those mappings start with, and the emitter's identity is confirmed
// by the two log strings at their known offsets. Only file-backed bytes inside
// the image's own segments are ever read, so a stale or truncated mapping cannot
// fault the runtime.
static void* find_gesture_emitter(void) {
    uintptr_t bases[64];
    int n = vdhs_collect_elf_bases(bases, 64);
    const size_t len1 = strlen(kRtEmitString) + 1;
    const size_t len2 = strlen(kRtEmitString2) + 1;
    for (int i = 0; i < n; i++) {
        const ElfW(Ehdr)* eh = (const ElfW(Ehdr)*)bases[i];
        if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_CLASS] != ELFCLASS64) continue;
        if (eh->e_phoff == 0 || eh->e_phoff > 0x10000) continue;
        if (eh->e_phnum == 0 || eh->e_phnum > 64 || eh->e_phentsize < sizeof(ElfW(Phdr))) continue;
        const ElfW(Phdr)* ph = (const ElfW(Phdr)*)(bases[i] + eh->e_phoff);
        int ok_str = 0, ok_emit = 0;
        for (int j = 0; j < eh->e_phnum; j++) {
            if (ph[j].p_type != PT_LOAD) continue;
            if (in_segment(&ph[j], kRtStringOff, len1) && in_segment(&ph[j], kRtString2Off, len2))
                ok_str = 1;
            if ((ph[j].p_flags & PF_X) && in_segment(&ph[j], kRtEmitOff, 4))
                ok_emit = 1;
        }
        if (!ok_str || !ok_emit) continue;
        if (strcmp((const char*)(bases[i] + kRtStringOff), kRtEmitString) != 0) continue;
        if (strcmp((const char*)(bases[i] + kRtString2Off), kRtEmitString2) != 0) continue;
        LOGI("runtime lib base=%p emitter=%p", (void*)bases[i], (void*)(bases[i] + kRtEmitOff));
        return (void*)(bases[i] + kRtEmitOff);
    }
    return NULL;
}

static void* rt_thread(void* arg) {
    (void)arg;
    int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    LOGI("shadowhook %s init=%d", shadowhook_get_version(), rc);
    if (rc != 0) { LOGE("shadowhook init failed: %s", shadowhook_to_errmsg(rc)); return NULL; }

    // The runtime libraries are mapped lazily, the first time an app asks the
    // runtime for anything, which can be long after boot.
    const char* names[2] = { kStatusSymMangled, kStatusSymPlain };
    void* found[2] = { NULL, NULL };
    // Install the first interception point that becomes available: the emitter
    // when its library is already mapped, otherwise the platform push. Nothing
    // is waited for, so no gesture can slip through while a hook is pending.
    int64_t last_resolve = 0, last_log = now_ms();
    int regions = -1;
    for (;;) {
        int64_t now = now_ms();
        // The string scan is expensive, so only repeat it when a library was
        // mapped in the meantime.
        int n = count_map_regions();
        if (n != regions) {
            regions = n;
            void* emit = find_gesture_emitter();
            if (emit) {
                void* stub = shadowhook_hook_func_addr(emit, (void*)my_gesture_emit,
                                                       (void**)&g_orig_emit);
                LOGI("hook gesture emitter %s (%s)", stub ? "installed" : "FAILED",
                     stub ? "ok" : shadowhook_to_errmsg(shadowhook_get_errno()));
                if (stub) return NULL;
            }
        }
        if (!found[0] && !found[1] && now - last_resolve >= 500) {
            last_resolve = now;
            vdhs_resolve_mapped_symbols(kInputMgr, names, found, 2);
        }
        if (found[0] || found[1]) {
            void* target = found[0] ? found[0] : found[1];
            void* stub = shadowhook_hook_func_addr(target, (void*)my_service_status,
                                                   (void**)&g_orig_status);
            LOGI("hook service_status_callback %s (%s)", stub ? "installed" : "FAILED",
                 stub ? "ok" : shadowhook_to_errmsg(shadowhook_get_errno()));
            if (stub) break;
        }
        if (now - last_log >= 30000) {
            last_log = now;
            LOGI("waiting for the runtime libraries");
        }
        usleep(200000);
    }

    // Upgrade to the emitter once its library shows up (it is only mapped when
    // an app actually drives the runtime's device layer).
    for (;;) {
        usleep(3000000);
        int n = count_map_regions();
        if (n == regions) continue;
        regions = n;
        void* e = find_gesture_emitter();
        if (!e) continue;
        void* s2 = shadowhook_hook_func_addr(e, (void*)my_gesture_emit, (void**)&g_orig_emit);
        LOGI("hook gesture emitter (upgrade) %s (%s)", s2 ? "installed" : "FAILED",
             s2 ? "ok" : shadowhook_to_errmsg(shadowhook_get_errno()));
        if (s2) return NULL;
    }
}

static void spawn(void* (*fn)(void*), const char* what) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, fn, NULL);
    pthread_attr_destroy(&a);
    if (rc != 0) LOGE("%s thread failed: %d", what, rc);
}

__attribute__((visibility("default")))
int vdhs_rt_main(void* javaVM, int mode) {
    (void)javaVM;
    LOGI("runtime payload start (mode=%d)", mode);
    spawn(rt_thread, "rt");
    return 0;
}
