// VD-PicoEnhance - Zygisk loader.
//
// Thin by design: match the target process, stage the payload
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
#include "vd_capability.h"
#include "vdhs_module_config.h"

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
static const char* kShellProcess = "com.picoxr.xrshell";

// Files staged from <module>/payload/<name> into <app_data_dir>/vdhs/<basename>.
// Absent optional entries are removed from the stage so backend/config defaults
// cannot be overridden by a previous installation. The install-if-missing mesh
// is the exception: a user's existing copy is preserved.
struct StagedFile {
    const char* name;
    bool required;
    int fd;
    bool install_if_missing;
};

static StagedFile g_files[] = {
    {"payload/libvdhs.so", true, -1, false},
    {"payload/VdHsMod.dll", true, -1, false},
    {"payload/ui_zh.txt", false, -1, false},
    {"payload/Xenko.OpenXR.patched.dll", false, -1, false},
    {"payload/0Harmony.dll", false, -1, false},
    {"payload/backend.txt", false, -1, false},
    {"payload/mode.txt", false, -1, false},
    {"payload/hand_mesh_fb.bin", false, -1, true},
};
static const int g_fileCount = (int)(sizeof(g_files) / sizeof(g_files[0]));
static StagedFile g_shell_files[] = {
    {"payload/libvdhs_shell.so", true, -1, false},
};
static StagedFile* g_active = g_files;
static int g_active_count = g_fileCount;

static bool g_target = false;
static bool g_sources_ok = false;
static bool g_shell = false;
static int g_mode = 2;

static int read_module_setting(int directory, const char* name, int fallback) {
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? fallback : 0;
    FILE* stream = fdopen(fd, "r");
    if (!stream) { close(fd); return 0; }
    int value = vdhs_config_read(stream, fallback);
    if (ferror(stream)) value = 0;
    fclose(stream);
    return value;
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool copy_fd_to_file(int fd, const char* dst) {
    if (lseek(fd, 0, SEEK_SET) < 0) return false;
    char tmp[1280];
    int len = snprintf(tmp, sizeof tmp, "%s.XXXXXX", dst);
    if (len < 0 || (size_t)len >= sizeof tmp) { errno = ENAMETOOLONG; return false; }
    int out = mkstemp(tmp);
    if (out < 0) return false;
    char buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { if (w == 0) errno = EIO; ok = false; break; }
            off += w;
        }
        if (!ok) break;
    }
    int error = ok ? 0 : errno;
    if (close(out) != 0 && ok) { error = errno; ok = false; }
    if (ok && rename(tmp, dst) != 0) { error = errno; ok = false; }
    if (!ok) {
        unlink(tmp);
        errno = error;
    }
    return ok;
}

static void close_staged_fds() {
    for (int i = 0; i < g_active_count; i++) {
        if (g_active[i].fd >= 0) close(g_active[i].fd);
        g_active[i].fd = -1;
    }
}

class VdHsModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs*) override {
        int directory = api_->getModuleDir();
        vd_capability_prepare(directory);
        if (directory >= 0) close(directory);
    }

    void postServerSpecialize(const zygisk::ServerSpecializeArgs*) override {
        vd_capability_start(env_);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        g_target = false;
        g_sources_ok = false;
        g_shell = false;
        if (!args || !args->nice_name) return;

        const char* name = env_->GetStringUTFChars(args->nice_name, nullptr);
        bool match = name && strcmp(name, kTargetProcess) == 0;
        bool shell = name && strcmp(name, kShellProcess) == 0;
        if (name) env_->ReleaseStringUTFChars(args->nice_name, name);
        if (!match && !shell) return;

        int dir = api_->getModuleDir();
        if (dir < 0) { LOGE("getModuleDir failed"); return; }

        const char* environment_mode = getenv("VDHS_MODE");
        int fallback_mode = environment_mode && *environment_mode ? atoi(environment_mode) : 2;
        g_mode = read_module_setting(dir, "payload/mode.txt", fallback_mode);
        if (shell && !vdhs_shell_requested(g_mode)) {
            close(dir);
            return;
        }
        g_target = true;
        g_shell = shell;
        g_active = shell ? g_shell_files : g_files;
        g_active_count = shell ? (int)(sizeof g_shell_files / sizeof g_shell_files[0]) : g_fileCount;
        LOGI("preAppSpecialize: target process %s", shell ? kShellProcess : kTargetProcess);

        g_sources_ok = true;
        for (int i = 0; i < g_active_count; i++) {
            int fd = openat(dir, g_active[i].name, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                if (errno == ENOENT && !g_active[i].required) {
                    LOGI("stage: optional %s not present", g_active[i].name);
                } else {
                    LOGE("stage open %s: %s", g_active[i].name, strerror(errno));
                    g_sources_ok = false;
                }
                continue;
            }
            struct stat st;
            if (fstat(fd, &st) != 0) {
                LOGE("stage stat %s: %s", g_active[i].name, strerror(errno));
                close(fd);
                g_sources_ok = false;
                continue;
            }
            if (!S_ISREG(st.st_mode) || (g_active[i].required && st.st_size == 0)) {
                LOGE("stage: invalid payload file %s", g_active[i].name);
                close(fd);
                g_sources_ok = false;
                continue;
            }
            if (!api_->exemptFd(fd)) {
                LOGE("stage: exemptFd failed for %s", g_active[i].name);
                close(fd);
                g_sources_ok = false;
                continue;
            }
            g_active[i].fd = fd;
            LOGI("stage: %s -> fd %d", g_active[i].name, fd);
        }
        close(dir);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!g_target) return;
        if (!g_sources_ok) {
            LOGE("payload unavailable; injection skipped");
            close_staged_fds();
            return;
        }
        if (!args || !args->app_data_dir) {
            LOGE("no app_data_dir");
            close_staged_fds();
            return;
        }

        const char* data = env_->GetStringUTFChars(args->app_data_dir, nullptr);
        if (!data) {
            LOGE("app_data_dir unavailable");
            close_staged_fds();
            return;
        }
        char dir[1024];
        int len = snprintf(dir, sizeof dir, "%s/vdhs", data);
        env_->ReleaseStringUTFChars(args->app_data_dir, data);
        if (len < 0 || (size_t)len >= sizeof dir) {
            LOGE("app_data_dir path too long");
            close_staged_fds();
            return;
        }

        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            LOGE("mkdir %s: %s", dir, strerror(errno));
            close_staged_fds();
            return;
        }

        int staged = 0;
        bool stage_ok = true;
        for (int i = 0; i < g_active_count; i++) {
            char dst[1200];
            snprintf(dst, sizeof dst, "%s/%s", dir, base_name(g_active[i].name));
            if (g_active[i].install_if_missing) {
                // Install the bundled mesh on first launch; keep a user's own mesh on upgrade.
                struct stat st;
                if (lstat(dst, &st) == 0) {
                    if (!S_ISREG(st.st_mode)) {
                        LOGE("stage: existing mesh is not a regular file: %s", dst);
                        stage_ok = false;
                        break;
                    }
                    LOGI("stage: preserving existing %s", dst);
                    continue;
                }
                if (errno != ENOENT) {
                    LOGE("stage stat %s: %s", dst, strerror(errno));
                    stage_ok = false;
                    break;
                }
            }
            if (g_active[i].fd < 0) {
                if (unlink(dst) != 0 && errno != ENOENT) {
                    LOGE("stage remove %s: %s", dst, strerror(errno));
                    stage_ok = false;
                    break;
                }
            } else if (copy_fd_to_file(g_active[i].fd, dst)) {
                staged++;
            } else {
                LOGE("stage write %s: %s", dst, strerror(errno));
                stage_ok = false;
                break;
            }
        }
        close_staged_fds();
        if (!stage_ok) { LOGE("payload staging failed; injection skipped"); return; }
        LOGI("postAppSpecialize: staged %d file(s) into %s", staged, dir);

        char payload[1200];
        snprintf(payload, sizeof payload, "%s/%s", dir, g_shell ? "libvdhs_shell.so" : "libvdhs.so");
        void* handle = dlopen(payload, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) { LOGE("dlopen %s failed: %s", payload, dlerror()); return; }

        typedef int (*PayloadMain)(void*, int);
        const char* entry_name = g_shell ? "vdhs_shell_main" : "vdhs_payload_main";
        PayloadMain entry = (PayloadMain)dlsym(handle, entry_name);
        if (!entry) { LOGE("%s not found in payload", entry_name); return; }

        JavaVM* vm = nullptr;
        env_->GetJavaVM(&vm);

        int rc = entry(vm, g_mode);
        LOGI("postAppSpecialize: payload started rc=%d mode=%d", rc, g_mode);
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(VdHsModule)
