// Payload entry, exported from the shared libvdhs.so.
//
// The Zygisk module (zygisk/jni/module.cpp) stages this library plus
// VdHsMod.dll into the target app's data dir, dlopens it, and calls
// vdhs_payload_main(javaVm, mode).
//
// Responsibilities:
//   1. wait until the app's Mono runtime is up (libmonosgen-2.0.so mapped and
//      a root domain exists);
//   2. attach this thread;
//   3. map the managed DllImport name "libvdhs" to the staged copy;
//   4. load VdHsMod.dll through the Mono embedding API and invoke
//      VdHsMod.Loader.Start();
//   5. fall back to mono_image_open_from_data_with_name if
//      mono_domain_assembly_open refuses a path.
//
// Nothing here blocks the app's main thread: all work happens on a detached
// pthread started by vdhs_payload_main.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <android/log.h>
#include <elf.h>
#include <link.h>   // ElfW

#include "vdhs_layer.h"

#define TAG "vdhs-zygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---------------------------------------------------------------- Mono types
typedef void MonoDomain;
typedef void MonoThread;
typedef void MonoAssembly;
typedef void MonoImage;
typedef void MonoClass;
typedef void MonoMethod;
typedef void MonoObject;
typedef void MonoString;

struct mono_api {
    int ready;
    MonoDomain* (*get_root_domain)(void);
    MonoThread* (*thread_attach)(MonoDomain*);
    void        (*thread_detach)(MonoThread*);
    MonoAssembly* (*domain_assembly_open)(MonoDomain*, const char*);
    MonoImage*  (*image_open_from_data_with_name)(char*, uint32_t, int, int*, const char*);
    MonoAssembly* (*assembly_load_from_full)(MonoImage*, const char*, int*, int);
    MonoImage*  (*assembly_get_image)(MonoAssembly*);
    MonoClass*  (*class_from_name)(MonoImage*, const char*, const char*);
    MonoMethod* (*class_get_method_from_name)(MonoClass*, const char*, int);
    MonoObject* (*runtime_invoke)(MonoMethod*, void*, void**, MonoObject**);
    MonoString* (*object_to_string)(MonoObject*, MonoObject**);
    char*       (*string_to_utf8)(MonoString*);
    void        (*dllmap_insert)(MonoImage*, const char*, const char*, const char*, const char*);
    MonoImage*  (*get_corlib)(void);
    MonoClass*  (*get_object_class)(void);
};

static const char* kMonoLib = "libmonosgen-2.0.so";
static const char* kModAssembly = "VdHsMod.dll";
static const char* kModClassNamespace = "VdHsMod";
static const char* kModClassName = "Loader";
static const char* kModMethod = "Start";

// ---------------------------------------------------------------- helpers
static char* read_file(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

// Find an already-mapped shared library by substring in /proc/self/maps and
// return its load base (first file-offset-0 readable mapping) plus its path.
// /proc/self/maps lists every mapping in the process regardless of linker
// namespace, which dlopen()/dl_iterate_phdr (caller namespace only) do not.
static int find_mapped_base(const char* needle, uintptr_t* out_base, char* path, size_t cap) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, needle)) continue;
        unsigned long start = 0, end = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &off) != 4) continue;
        if (off != 0 || perms[0] != 'r') continue;
        *out_base = (uintptr_t)start;
        char* slash = strrchr(line, '/');
        if (slash) {
            size_t n = strlen(slash);
            while (n > 0 && (slash[n - 1] == '\n' || slash[n - 1] == '\r')) n--;
            if (n > 0 && n < cap) { memcpy(path, slash, n); path[n] = '\0'; }
        }
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

// Symbol count from a SysV or GNU hash table (both give a tight upper bound on
// .dynsym entries). Used because .dynsym has no explicit length in ELF.
static size_t gnu_symcount(const uint32_t* gh) {
    const uint32_t nbuckets = gh[0];
    const uint32_t symoffset = gh[1];
    const uint32_t bloom_size = gh[2];
    const uint32_t* buckets = gh + 4 + bloom_size * (uint32_t)(sizeof(uintptr_t) / 4);
    if (nbuckets == 0) return symoffset;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
    if (last < symoffset) return symoffset;
    const uint32_t* chain = buckets + nbuckets;   // chain[k] belongs to symbol symoffset+k
    uint32_t idx = last;
    while (!(chain[idx - symoffset] & 1u)) idx++;
    return (size_t)idx + 1;
}

// Resolve dynamic symbols of an already-mapped library by walking its in-memory
// ELF. This sidesteps the linker namespace entirely: the app's Mono lives in
// the classloader namespace, but its mapped pages are ordinary readable memory
// for our (default-namespace) payload.
static int resolve_mapped_symbols(const char* needle, const char* const* names,
                                  void** out, int count) {
    uintptr_t base = 0;
    char path[512] = {0};
    if (!find_mapped_base(needle, &base, path, sizeof path)) return 0;

    const ElfW(Ehdr)* eh = (const ElfW(Ehdr)*)base;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1) return 0;
    if (eh->e_phoff == 0 || eh->e_phnum == 0) return 0;

    const ElfW(Phdr)* ph = (const ElfW(Phdr)*)(base + eh->e_phoff);
    const ElfW(Dyn)* dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dyn = (const ElfW(Dyn)*)(base + ph[i].p_vaddr); break; }
    }
    if (!dyn) return 0;

    const ElfW(Sym)* symtab = NULL;
    const char* strtab = NULL;
    const uint32_t* gnu = NULL;
    const uint32_t* hash = NULL;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = (const ElfW(Sym)*)(base + d->d_un.d_ptr); break;
            case DT_STRTAB:   strtab = (const char*)(base + d->d_un.d_ptr); break;
            case DT_GNU_HASH: gnu = (const uint32_t*)(base + d->d_un.d_ptr); break;
            case DT_HASH:     hash = (const uint32_t*)(base + d->d_un.d_ptr); break;
            default: break;
        }
    }
    if (!symtab || !strtab) return 0;

    size_t nsyms = 0;
    if (gnu) nsyms = gnu_symcount(gnu);
    else if (hash) nsyms = (size_t)hash[1];
    if (nsyms == 0) return 0;

    int got = 0;
    for (int j = 0; j < count; j++) {
        if (out[j]) { got++; continue; }
        for (size_t i = 0; i < nsyms; i++) {
            const char* nm = strtab + symtab[i].st_name;
            if (strcmp(nm, names[j]) == 0) {
                out[j] = (void*)(base + symtab[i].st_value);
                got++;
                break;
            }
        }
    }
    return got;
}

// Single-symbol variant, exported for vdhs_mark.c (weak reference there, so
// a build without this file still links).
__attribute__((visibility("default")))
void* vdhs_resolve_mapped_symbol(const char* lib, const char* name) {
    const char* names[1] = { name };
    void* out[1] = { NULL };
    if (resolve_mapped_symbols(lib, names, out, 1) <= 0) return NULL;
    return out[0];
}

static void dirname_of(const char* path, char* out, size_t cap) {
    size_t n = strlen(path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
    char* slash = strrchr(out, '/');
    if (slash) *slash = '\0';
}

static void log_managed_exception(struct mono_api* api, MonoObject* exc) {
    if (!exc) return;
    char* msg = NULL;
    if (api->object_to_string) {
        MonoObject* inner = NULL;
        MonoString* s = api->object_to_string(exc, &inner);
        if (s && api->string_to_utf8) msg = api->string_to_utf8(s);
    }
    LOGE("Loader.Start raised: %s", msg ? msg : "(unprintable)");
    if (msg) free(msg);
}

// mode.txt next to the payload overrides the env/argument. 1 = Layer-1 smoke
// test (stop before the managed load), 2 = full.
static int read_mode_file(const char* dir, int fallback) {
    char p[1200];
    snprintf(p, sizeof p, "%s/mode.txt", dir);
    FILE* f = fopen(p, "r");
    if (!f) return fallback;
    // The shipped mode.txt starts with '#' comment lines (some longer than any
    // fixed buffer), so scan character by character: skip whole comment lines,
    // then parse the first run of digits on the first data line.
    int v = -1, in_comment = 0, seen = 0, c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { in_comment = 0; seen = 0; continue; }
        if (in_comment) continue;
        if (!seen) {
            if (c == ' ' || c == '\t' || c == '\r') continue;
            if (c == '#') { in_comment = 1; continue; }
            seen = 1;
            v = 0;
        }
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
        else break;
    }
    fclose(f);
    return v > 0 ? v : fallback;
}

// Read the first non-comment, non-blank token line of a small text file.
// Used for backend.txt: the managed side must not touch System.IO because the
// app's trimmed BCL is missing methods such as File.ReadAllLines (observed).
static int read_token_file(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int in_comment = 0, seen = 0, c;
    size_t n = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { if (seen) break; in_comment = 0; seen = 0; continue; }
        if (in_comment) continue;
        if (!seen) {
            if (c == ' ' || c == '\t' || c == '\r') continue;
            if (c == '#') { in_comment = 1; continue; }
            seen = 1;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    fclose(f);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r')) n--;
    out[n] = '\0';
    return n > 0;
}

// ---------------------------------------------------------------- payload
static void* mono_waiter(void* arg) {
    void* java_vm = arg; (void)java_vm;
    int mode = 2;
    const char* m = getenv("VDHS_MODE");
    if (m && *m) mode = atoi(m);
    LOGI("payload: waiter start (mode=%d)", mode);

    char mod_path[1024];
    Dl_info info;
    if (dladdr((void*)mono_waiter, &info) && info.dli_fname) {
        dirname_of(info.dli_fname, mod_path, sizeof mod_path);
    } else {
        strncpy(mod_path, "/data/local/tmp", sizeof mod_path - 1);
        mod_path[sizeof mod_path - 1] = '\0';
    }
    mode = read_mode_file(mod_path, mode);
    LOGI("payload: dir=%s mode=%d", mod_path, mode);

    // Hand the managed side its config through the environment: the app's
    // trimmed netcore BCL lacks System.IO methods the mod would otherwise use.
    setenv("VDHS_PAYLOAD_DIR", mod_path, 1);
    if (!getenv("VDHS_BACKEND")) {
        char backend[64];
        char bp[1200];
        snprintf(bp, sizeof bp, "%s/backend.txt", mod_path);
        if (read_token_file(bp, backend, sizeof backend)) {
            setenv("VDHS_BACKEND", backend, 1);
            LOGI("payload: backend=%s", backend);
        }
    }

    // 1) Wait for the Mono runtime and resolve its embedding symbols (up to
    //    ~60 s). dlopen() cannot be used here: the app's libraries live in the
    //    classloader linker namespace, and both bare-name and absolute-path
    //    dlopen fail from our default-namespace payload. We instead read the
    //    already-mapped libmonosgen-2.0.so's .dynsym directly.
    static const char* kSyms[] = {
        "mono_get_root_domain",
        "mono_thread_attach",
        "mono_thread_detach",
        "mono_domain_assembly_open",
        "mono_image_open_from_data_with_name",
        "mono_assembly_load_from_full",
        "mono_assembly_get_image",
        "mono_class_from_name",
        "mono_class_get_method_from_name",
        "mono_runtime_invoke",
        "mono_object_to_string",
        "mono_string_to_utf8",
        "mono_dllmap_insert",
        "mono_get_corlib",
        "mono_get_object_class",
    };
    enum { kSymCount = (int)(sizeof(kSyms) / sizeof(kSyms[0])) };
    void* syms[kSymCount];
    int resolved = 0;
    for (int i = 0; i < 600 && !resolved; i++) {
        memset(syms, 0, sizeof syms);
        if (resolve_mapped_symbols(kMonoLib, kSyms, syms, kSymCount) > 0 && syms[0]) resolved = 1;
        else usleep(100 * 1000);
    }
    if (!resolved) { LOGE("payload: %s not mapped after 60s", kMonoLib); return NULL; }
    LOGI("payload: %s symbols resolved (root_domain=%p)", kMonoLib, syms[0]);

    struct mono_api api;
    memset(&api, 0, sizeof api);
    api.get_root_domain      = (MonoDomain* (*)(void))syms[0];
    api.thread_attach        = (MonoThread* (*)(MonoDomain*))syms[1];
    api.thread_detach        = (void (*)(MonoThread*))syms[2];
    api.domain_assembly_open = (MonoAssembly* (*)(MonoDomain*, const char*))syms[3];
    api.image_open_from_data_with_name = (MonoImage* (*)(char*, uint32_t, int, int*, const char*))syms[4];
    api.assembly_load_from_full = (MonoAssembly* (*)(MonoImage*, const char*, int*, int))syms[5];
    api.assembly_get_image   = (MonoImage* (*)(MonoAssembly*))syms[6];
    api.class_from_name      = (MonoClass* (*)(MonoImage*, const char*, const char*))syms[7];
    api.class_get_method_from_name = (MonoMethod* (*)(MonoClass*, const char*, int))syms[8];
    api.runtime_invoke       = (MonoObject* (*)(MonoMethod*, void*, void**, MonoObject**))syms[9];
    api.object_to_string     = (MonoString* (*)(MonoObject*, MonoObject**))syms[10];
    api.string_to_utf8       = (char* (*)(MonoString*))syms[11];
    api.dllmap_insert        = (void (*)(MonoImage*, const char*, const char*, const char*, const char*))syms[12];
    api.get_corlib           = (MonoImage* (*)(void))syms[13];
    api.get_object_class     = (MonoClass* (*)(void))syms[14];

    if (!api.get_root_domain || !api.thread_attach || !api.domain_assembly_open ||
        !api.assembly_get_image || !api.class_from_name || !api.class_get_method_from_name ||
        !api.runtime_invoke) {
        LOGE("payload: required Mono embedding symbols missing");
        return NULL;
    }

    LOGI("payload: resolved symbols root_domain=%p thread_attach=%p invoke=%p",
         (void*)api.get_root_domain, (void*)api.thread_attach, (void*)api.runtime_invoke);

    // 2) Wait until Mono is fully initialized, not merely until a root domain
    //    exists: mono_get_root_domain() can return non-NULL while the runtime's
    //    default classes are still being set up, and mono_thread_attach() then
    //    aborts with "Assertion ... object.c: condition `klass' not met"
    //    (observed on device). Corlib + System.Object resolving is a safe
    //    readiness gate.
    //    mono_get_object_class() only reads mono_defaults (no lookup, no
    //    assert), so it is a safe "defaults are populated" signal; the extra
    //    settle time lets the remaining defaults/thread classes finish.
    MonoDomain* domain = NULL;
    for (int i = 0; i < 600 && !domain; i++) {
        MonoClass* object_class = api.get_object_class ? api.get_object_class() : NULL;
        MonoDomain* d = api.get_root_domain();
        if (object_class && d) domain = d;
        else usleep(100 * 1000);
    }
    if (!domain) { LOGE("payload: runtime not ready after 60s"); return NULL; }
    usleep(1500 * 1000);
    MonoThread* thread = api.thread_attach(domain);
    LOGI("payload: attached to domain %p", (void*)domain);

    if (mode == 1) {
        LOGI("payload: LAYER-1 OK (runtime reachable); skipping managed load");
        if (api.thread_detach && thread) api.thread_detach(thread);
        return NULL;
    }

    // NOTE: no mono_dllmap_insert() here. In the official APK's netcore Mono
    // (dotnet/runtime) that export is a stub whose whole body is
    // g_assert_not_reached(), so calling it aborts the app (verified on
    // device). Pinvoke resolution is replaced by passing raw function
    // pointers to VdHsMod.NativeBridge below.

    // 4) load the managed mod and invoke Loader.Start() (retry while the app
    //    is still bringing assemblies up)
    char dll_path[1200];
    snprintf(dll_path, sizeof dll_path, "%s/%s", mod_path, kModAssembly);

    MonoAssembly* assembly = NULL;
    for (int i = 0; i < 300 && !assembly; i++) {
        assembly = api.domain_assembly_open(domain, dll_path);
        if (!assembly) usleep(100 * 1000);
    }

    if (!assembly) {
        LOGI("payload: mono_domain_assembly_open failed for path; trying raw image fallback");
        long len = 0;
        char* bytes = read_file(dll_path, &len);
        if (bytes) {
            int status = 0;
            MonoImage* image = api.image_open_from_data_with_name
                ? api.image_open_from_data_with_name(bytes, (uint32_t)len, 1, &status, dll_path) : NULL;
            free(bytes);
            if (image && api.assembly_load_from_full) {
                int st2 = 0;
                assembly = api.assembly_load_from_full(image, dll_path, &st2, 0);
                LOGI("payload: raw image load image=%p assembly=%p status=%d/%d", (void*)image, (void*)assembly, status, st2);
            }
        }
    }

    if (!assembly) { LOGE("payload: could not load %s", dll_path); if (api.thread_detach && thread) api.thread_detach(thread); return NULL; }

    MonoImage* image = api.assembly_get_image(assembly);
    MonoClass* klass = api.class_from_name(image, kModClassNamespace, kModClassName);
    if (!klass) { LOGE("payload: class %s.%s not found", kModClassNamespace, kModClassName); if (api.thread_detach && thread) api.thread_detach(thread); return NULL; }
    MonoMethod* method = api.class_get_method_from_name(klass, kModMethod, 0);
    if (!method) { LOGE("payload: %s.%s.%s() not found", kModClassNamespace, kModClassName, kModMethod); if (api.thread_detach && thread) api.thread_detach(thread); return NULL; }

    // 4b) Hand the two native entry points to VdHsMod.NativeBridge. They live in
    //     this very library, so we take their addresses directly; the managed
    //     side wraps them with Marshal.GetDelegateForFunctionPointer. This is the
    //     replacement for DllImport("libvdhs") (see the note above).
    {
        extern int vdhs_active_input(uint64_t instance);
        extern int vdhs_mark_loaded(void);
        MonoClass* bridge = api.class_from_name(image, kModClassNamespace, "NativeBridge");
        MonoMethod* setp = bridge ? api.class_get_method_from_name(bridge, "SetPointers", 2) : NULL;
        if (setp) {
            void* p0 = (void*)(uintptr_t)&vdhs_active_input;
            void* p1 = (void*)(uintptr_t)&vdhs_mark_loaded;
            void* argv[2] = { &p0, &p1 };
            MonoObject* e2 = NULL;
            api.runtime_invoke(setp, NULL, argv, &e2);
            if (e2) log_managed_exception(&api, e2);
            else LOGI("payload: NativeBridge.SetPointers delivered");
        } else {
            LOGE("payload: %s.NativeBridge.SetPointers(intptr,intptr) not found", kModClassNamespace);
        }
    }

    MonoObject* exc = NULL;
    LOGI("payload: invoking %s.%s.%s()", kModClassNamespace, kModClassName, kModMethod);
    api.runtime_invoke(method, NULL, NULL, &exc);
    if (exc) log_managed_exception(&api, exc);
    else LOGI("payload: Loader.Start() returned");

    if (api.thread_detach && thread) api.thread_detach(thread);
    return NULL;
}

// Called by the Zygisk module after dlopen. Non-blocking: spawns the waiter.
__attribute__((visibility("default")))
int vdhs_payload_main(void* java_vm, int mode) {
    if (mode <= 0) mode = 2;
    char m[8];
    snprintf(m, sizeof m, "%d", mode);
    setenv("VDHS_MODE", m, 1);

    // The compat layer must hook VD's loader before VD's first enumerate,
    // which can precede the Mono gate below by ~1 s, so start it here.
    // mode.txt=1 (smoke test / rollback) leaves VD's code untouched.
    {
        char dir[1024] = "/data/local/tmp";
        Dl_info info;
        if (dladdr((void*)vdhs_payload_main, &info) && info.dli_fname) dirname_of(info.dli_fname, dir, sizeof dir);
        int eff = read_mode_file(dir, mode);
        if (eff >= 2) vdhs_layer_start();
        else LOGI("payload_main: mode=%d, compat layer not started", eff);
    }

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, mono_waiter, java_vm);
    pthread_attr_destroy(&attr);
    LOGI("payload_main: mode=%d spawn=%d", mode, rc);
    return rc;
}
