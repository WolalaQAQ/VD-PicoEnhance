// VD handswitch - Zygisk loader (Track Z).
//
// Thin by design (design §3.2): match the target process, stage the payload
// files out of the module dir into the app's private data dir, dlopen
// libvdhs.so and hand control to vdhs_payload_main(). All managed-mod logic
// lives in VdHsMod.dll; the payload only bridges into Mono.
//
// Process matching uses the real package name, confirmed from the official
// APK's AndroidManifest (observed): VirtualDesktop.Android.
//
// No C++ STL is used (the Zygisk sample requires APP_STL=none); only libc.
#include <sys/types.h>   // dev_t / ino_t used by zygisk.hpp before it includes them
#include "zygisk.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "vdhs-zygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Observed from the untouched official APK (Virtual Desktop_1.34.22.0.apk).
static const char* kTargetProcess = "VirtualDesktop.Android";

// Files staged from <module>/payload/<name> into <app_data_dir>/vdhs/<basename>.
// Missing entries are skipped (e.g. 0Harmony.dll / the redirect artifact are
// optional and depend on the chosen backend).
struct StagedFile {
    const char* name;
    int fd;
};

static StagedFile g_files[] = {
    {"payload/libvdhs.so", -1},
    {"payload/VdHsMod.dll", -1},
    {"payload/Xenko.OpenXR.patched.dll", -1},
    {"payload/0Harmony.dll", -1},
    {"payload/backend.txt", -1},
    {"payload/mode.txt", -1},
};
static const int g_fileCount = (int)(sizeof(g_files) / sizeof(g_files[0]));

static bool g_target = false;

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool copy_fd_to_file(int fd, const char* dst) {
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) return false;
    lseek(fd, 0, SEEK_SET);
    char buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) { ok = false; break; }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
    }
    close(out);
    return ok;
}

class VdHsModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        g_target = false;
        if (!args || !args->nice_name) return;

        const char* name = env_->GetStringUTFChars(args->nice_name, nullptr);
        bool match = name && strcmp(name, kTargetProcess) == 0;
        if (name) env_->ReleaseStringUTFChars(args->nice_name, name);
        if (!match) return;

        g_target = true;
        LOGI("preAppSpecialize: target process %s", kTargetProcess);

        int dir = api_->getModuleDir();
        if (dir < 0) { LOGE("getModuleDir failed"); return; }

        for (int i = 0; i < g_fileCount; i++) {
            int fd = openat(dir, g_files[i].name, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                LOGI("stage: %s not present (%s)", g_files[i].name, strerror(errno));
                continue;
            }
            api_->exemptFd(fd); // survive app specialization
            g_files[i].fd = fd;
            LOGI("stage: %s -> fd %d", g_files[i].name, fd);
        }
        close(dir);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!g_target) return;
        if (!args || !args->app_data_dir) { LOGE("no app_data_dir"); return; }

        const char* data = env_->GetStringUTFChars(args->app_data_dir, nullptr);
        if (!data) { LOGE("app_data_dir unavailable"); return; }
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/vdhs", data);
        env_->ReleaseStringUTFChars(args->app_data_dir, data);

        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            LOGE("mkdir %s: %s", dir, strerror(errno));
            return;
        }

        int staged = 0;
        for (int i = 0; i < g_fileCount; i++) {
            if (g_files[i].fd < 0) continue;
            char dst[1200];
            snprintf(dst, sizeof dst, "%s/%s", dir, base_name(g_files[i].name));
            if (copy_fd_to_file(g_files[i].fd, dst)) staged++;
            else LOGE("stage write %s: %s", dst, strerror(errno));
            close(g_files[i].fd);
            g_files[i].fd = -1;
        }
        LOGI("postAppSpecialize: staged %d file(s) into %s", staged, dir);

        char payload[1200];
        snprintf(payload, sizeof payload, "%s/libvdhs.so", dir);
        void* handle = dlopen(payload, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) { LOGE("dlopen %s failed: %s", payload, dlerror()); return; }

        typedef int (*PayloadMain)(void*, int);
        PayloadMain entry = (PayloadMain)dlsym(handle, "vdhs_payload_main");
        if (!entry) { LOGE("vdhs_payload_main not found in payload"); return; }

        JavaVM* vm = nullptr;
        env_->GetJavaVM(&vm);

        int mode = 2;
        const char* m = getenv("VDHS_MODE");
        if (m && *m) mode = atoi(m);

        int rc = entry(vm, mode);
        LOGI("postAppSpecialize: payload started rc=%d mode=%d", rc, mode);
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(VdHsModule)
