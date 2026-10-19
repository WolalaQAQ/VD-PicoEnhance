#include "vd_capability.h"
#include "vdhs_module_config.h"
#include <android/log.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr const char* kTag = "vdhs-capability";
constexpr size_t kMaxDex = 512 * 1024;
unsigned char* dex_bytes;
size_t dex_size;
bool requested;
jobject retained_loader, retained_bytes;

bool read_exact(int fd, void* data, size_t length) {
    auto* output = static_cast<unsigned char*>(data);
    while (length) {
        ssize_t count = read(fd, output, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        output += count; length -= static_cast<size_t>(count);
    }
    return true;
}

bool small_file(int directory, const char* name, char* text, size_t capacity, bool* missing) {
    *missing = false;
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { *missing = errno == ENOENT; return false; }
    struct stat st {};
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
                 (st.st_mode & 0022) == 0 && st.st_size > 0 && static_cast<size_t>(st.st_size) < capacity;
    if (valid) {
        valid = read_exact(fd, text, static_cast<size_t>(st.st_size));
        if (valid) text[st.st_size] = 0;
    }
    close(fd);
    return valid;
}

bool full_mode(int directory) {
    int fd = openat(directory, "payload/mode.txt", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT;
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022)) {
        close(fd);
        return false;
    }
    FILE* stream = fdopen(fd, "r");
    if (!stream) { close(fd); return false; }
    int mode = vdhs_config_read(stream, 2);
    bool valid = !ferror(stream) && mode == 2;
    fclose(stream);
    return valid;
}

bool payload_ready(int directory, const char* name, bool native) {
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st {};
    unsigned char header[20] {};
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
                 (st.st_mode & 0022) == 0 && st.st_size > 512 && read_exact(fd, header, sizeof header);
    close(fd);
    if (!valid) return false;
    return native ? (!memcmp(header, "\177ELF", 4) && header[4] == 2 && header[5] == 1 && header[18] == 183 && header[19] == 0) :
                    (header[0] == 'M' && header[1] == 'Z');
}

bool checked(JNIEnv* env, const char* step) {
    if (!env->ExceptionCheck()) return true;
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, kTag, "DEX bootstrap failed at %s; capability not confirmed", step);
    return false;
}
} // namespace

void vd_capability_prepare(int directory) {
    if (dex_bytes) { free(dex_bytes); dex_bytes = nullptr; dex_size = 0; }
    requested = true;
    if (directory < 0 || !full_mode(directory) ||
        !payload_ready(directory, "payload/libvdhs.so", true) || !payload_ready(directory, "payload/VdHsMod.dll", false)) return;
    char setting[32]; bool missing;
    if (small_file(directory, "vd-capability.txt", setting, sizeof setting, &missing)) {
        if (!strcmp(setting, "VDC2 0\n")) requested = false;
        else if (strcmp(setting, "VDC2 1\n")) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Invalid VD-only configuration; server helper skipped");
            return;
        }
    } else if (!missing) return;
    int fd = openat(directory, "capability/classes.dex", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st {};
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && (st.st_mode & 0022) == 0 &&
        st.st_size >= 112 && static_cast<size_t>(st.st_size) <= kMaxDex) {
        auto* bytes = static_cast<unsigned char*>(malloc(static_cast<size_t>(st.st_size)));
        if (bytes && read_exact(fd, bytes, static_cast<size_t>(st.st_size)) && !memcmp(bytes, "dex\n", 4)) {
            dex_bytes = bytes; dex_size = static_cast<size_t>(st.st_size);
        } else free(bytes);
    }
    close(fd);
}

void vd_capability_start(JNIEnv* env) {
    if (!env || !dex_bytes || !dex_size) return;
    if (env->PushLocalFrame(24) != JNI_OK) { checked(env, "local frame"); free(dex_bytes); dex_bytes = nullptr; return; }
    // Heap ByteBuffer and ClassLoader are retained for the system_server lifetime.
    // Only boot-framework classes are resolved here; PMS arrives later on Java's
    // independent worker. This does not wait for services on the boot thread.
    auto boot = [&]() -> bool {
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(dex_size));
        if (!checked(env, "byte array") || !bytes) return false;
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(dex_size), reinterpret_cast<const jbyte*>(dex_bytes));
        if (!checked(env, "DEX bytes")) return false;
        jclass buffer_class = env->FindClass("java/nio/ByteBuffer");
        if (!checked(env, "ByteBuffer") || !buffer_class) return false;
        jmethodID wrap = env->GetStaticMethodID(buffer_class, "wrap", "([B)Ljava/nio/ByteBuffer;");
        if (!checked(env, "ByteBuffer.wrap") || !wrap) return false;
        jobject buffer = env->CallStaticObjectMethod(buffer_class, wrap, bytes);
        if (!checked(env, "ByteBuffer instance") || !buffer) return false;
        jclass parent_class = env->FindClass("java/lang/ClassLoader");
        if (!checked(env, "ClassLoader") || !parent_class) return false;
        jmethodID get_parent = env->GetStaticMethodID(parent_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
        if (!checked(env, "getSystemClassLoader") || !get_parent) return false;
        jobject parent = env->CallStaticObjectMethod(parent_class, get_parent);
        if (!checked(env, "parent ClassLoader") || !parent) return false;
        jclass memory_class = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (!checked(env, "InMemoryDexClassLoader") || !memory_class) return false;
        jmethodID constructor = env->GetMethodID(memory_class, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (!checked(env, "DEX loader constructor") || !constructor) return false;
        jobject loader = env->NewObject(memory_class, constructor, buffer, parent);
        if (!checked(env, "DEX loader") || !loader) return false;
        jmethodID load = env->GetMethodID(parent_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!checked(env, "loadClass method") || !load) return false;
        jstring name = env->NewStringUTF("org.vdpico.capability.CapabilityServer");
        if (!checked(env, "helper name") || !name) return false;
        auto server = static_cast<jclass>(env->CallObjectMethod(loader, load, name));
        if (!checked(env, "helper class") || !server) return false;
        jmethodID start = env->GetStaticMethodID(server, "start", "(Z)V");
        if (!checked(env, "helper start method") || !start) return false;
        jobject loader_ref = env->NewGlobalRef(loader);
        if (!checked(env, "retain loader") || !loader_ref) return false;
        jobject bytes_ref = env->NewGlobalRef(bytes);
        if (!checked(env, "retain DEX bytes") || !bytes_ref) { env->DeleteGlobalRef(loader_ref); return false; }
        env->CallStaticVoidMethod(server, start, requested ? JNI_TRUE : JNI_FALSE);
        if (!checked(env, "helper start")) { env->DeleteGlobalRef(loader_ref); env->DeleteGlobalRef(bytes_ref); return false; }
        retained_loader = loader_ref; retained_bytes = bytes_ref;
        return true;
    };
    bool started = boot();
    env->PopLocalFrame(nullptr);
    free(dex_bytes); dex_bytes = nullptr; dex_size = 0;
    if (started) __android_log_print(ANDROID_LOG_INFO, kTag, "VD-only declaration worker started; readback still required (requested=%d)", requested);
}
