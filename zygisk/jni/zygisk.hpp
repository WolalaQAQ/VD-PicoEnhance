/* Copyright 2022-2023 John "topjohnwu" Wu
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

// This is the public API for Zygisk modules.
// DO NOT MODIFY ANY CODE IN THIS HEADER.
//
// Vendored verbatim from:
//   https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp
// API v5, minimal Magisk 27000. See docs/HAND-VD-ZYGISK-2026-09-28.md for the
// downgrade procedure if the device Magisk is older than 27000.

#pragma once

#include <jni.h>

#define ZYGISK_API_VERSION 5

namespace zygisk {

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

class ModuleBase {
public:

    // This method is called as soon as the module is loaded into the target process.
    // A Zygisk API handle will be passed as an argument.
    virtual void onLoad([[maybe_unused]] Api *api, [[maybe_unused]] JNIEnv *env) {}

    // This method is called before the app process is specialized.
    // At this point, the process just got forked from zygote, but no app specific specialization
    // is applied. This means that the process does not have any sandbox restrictions and
    // still runs with the same privilege of zygote.
    //
    // All the arguments that will be sent and used for app specialization is passed as a single
    // AppSpecializeArgs object. You can read and overwrite these arguments to change how the app
    // process is being specialized.
    //
    // If you need to run some operations as superuser, you can call Api::connectCompanion() to
    // get a socket to do IPC calls with a root companion process.
    // See Api::connectCompanion() for more info.
    virtual void preAppSpecialize([[maybe_unused]] AppSpecializeArgs *args) {}

    // This method is called after the app process is specialized.
    // At this point, the process has all sandbox restrictions enabled for this application.
    // This means that this method runs with the same privilege of the app's own code.
    virtual void postAppSpecialize([[maybe_unused]] const AppSpecializeArgs *args) {}

    // This method is called before the system server process is specialized.
    // See preAppSpecialize(args) for more info.
    virtual void preServerSpecialize([[maybe_unused]] ServerSpecializeArgs *args) {}

    // This method is called after the system server process is specialized.
    // At this point, the process runs with the privilege of system_server.
    virtual void postServerSpecialize([[maybe_unused]] const ServerSpecializeArgs *args) {}
};

struct AppSpecializeArgs {
    // Required arguments. These arguments are guaranteed to exist on all Android versions.
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jobjectArray &rlimits;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;

    // Optional arguments. Please check whether the pointer is null before de-referencing
    jintArray *const fds_to_ignore;
    jboolean *const is_child_zygote;
    jboolean *const is_top_app;
    jobjectArray *const pkg_data_info_list;
    jobjectArray *const whitelisted_data_info_list;
    jboolean *const mount_data_dirs;
    jboolean *const mount_storage_dirs;
    jboolean *const mount_sysprop_overrides;

    AppSpecializeArgs() = delete;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;

    ServerSpecializeArgs() = delete;
};

namespace internal {
struct api_table;
template <class T> void entry_impl(api_table *, JNIEnv *);
}

// These values are used in Api::setOption(Option)
enum Option : int {
    // Force Magisk's denylist unmount routines to run on this process.
    // Setting this option only makes sense in preAppSpecialize.
    FORCE_DENYLIST_UNMOUNT = 0,

    // When this option is set, your module's library will be dlclose-ed after post[XXX]Specialize.
    // YOU MUST NOT ENABLE THIS OPTION AFTER HOOKING ANY FUNCTIONS IN THE PROCESS.
    DLCLOSE_MODULE_LIBRARY = 1,
};

// Bit masks of the return value of Api::getFlags()
enum StateFlag : uint32_t {
    // The user has granted root access to the current process
    PROCESS_GRANTED_ROOT = (1u << 0),

    // The current process was added on the denylist
    PROCESS_ON_DENYLIST = (1u << 1),
};

// All API methods will stop working after post[XXX]Specialize as Zygisk will be unloaded
// from the specialized process afterwards.
struct Api {

    // Connect to a root companion process and get a Unix domain socket for IPC.
    int connectCompanion();

    // Get the file descriptor of the root folder of the current module.
    int getModuleDir();

    // Set various options for your module.
    void setOption(Option opt);

    // Get information about the current process.
    uint32_t getFlags();

    // Exempt the provided file descriptor from being automatically closed.
    bool exemptFd(int fd);

    // Hook JNI native methods for a class
    void hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int numMethods);

    // Hook functions in the PLT (Procedure Linkage Table) of ELFs loaded in memory.
    void pltHookRegister(dev_t dev, ino_t inode, const char *symbol, void *newFunc, void **oldFunc);

    // Commit all the hooks that was previously registered.
    bool pltHookCommit();

private:
    internal::api_table *tbl;
    template <class T> friend void internal::entry_impl(internal::api_table *, JNIEnv *);
};

// Register a class as a Zygisk module

#define REGISTER_ZYGISK_MODULE(clazz) \
void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) { \
    zygisk::internal::entry_impl<clazz>(table, env);                        \
}

// Register a root companion request handler function for your module

#define REGISTER_ZYGISK_COMPANION(func) \
void zygisk_companion_entry(int client) { func(client); }

/*********************************************************
 * The following is internal ABI implementation detail.
 * You do not have to understand what it is doing.
 *********************************************************/

namespace internal {

struct module_abi {
    long api_version;
    ModuleBase *impl;

    void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
    void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
    void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
    void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);

    module_abi(ModuleBase *module) : api_version(ZYGISK_API_VERSION), impl(module) {
        preAppSpecialize = [](auto m, auto args) { m->preAppSpecialize(args); };
        postAppSpecialize = [](auto m, auto args) { m->postAppSpecialize(args); };
        preServerSpecialize = [](auto m, auto args) { m->preServerSpecialize(args); };
        postServerSpecialize = [](auto m, auto args) { m->postServerSpecialize(args); };
    }
};

struct api_table {
    // Base
    void *impl;
    bool (*registerModule)(api_table *, module_abi *);

    void (*hookJniNativeMethods)(JNIEnv *, const char *, JNINativeMethod *, int);
    void (*pltHookRegister)(dev_t, ino_t, const char *, void *, void **);
    bool (*exemptFd)(int);
    bool (*pltHookCommit)();
    int  (*connectCompanion)(void * /* impl */);
    void (*setOption)(void * /* impl */, Option);
    int  (*getModuleDir)(void * /* impl */);
    uint32_t (*getFlags)(void * /* impl */);
};

template <class T>
void entry_impl(api_table *table, JNIEnv *env) {
    static Api api;
    api.tbl = table;
    static T module;
    ModuleBase *m = &module;
    static module_abi abi(m);
    if (!table->registerModule(table, &abi)) return;
    m->onLoad(&api, env);
}

} // namespace internal

inline int Api::connectCompanion() {
    return tbl->connectCompanion ? tbl->connectCompanion(tbl->impl) : -1;
}
inline int Api::getModuleDir() {
    return tbl->getModuleDir ? tbl->getModuleDir(tbl->impl) : -1;
}
inline void Api::setOption(Option opt) {
    if (tbl->setOption) tbl->setOption(tbl->impl, opt);
}
inline uint32_t Api::getFlags() {
    return tbl->getFlags ? tbl->getFlags(tbl->impl) : 0;
}
inline bool Api::exemptFd(int fd) {
    return tbl->exemptFd != nullptr && tbl->exemptFd(fd);
}
inline void Api::hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int numMethods) {
    if (tbl->hookJniNativeMethods) tbl->hookJniNativeMethods(env, className, methods, numMethods);
}
inline void Api::pltHookRegister(dev_t dev, ino_t inode, const char *symbol, void *newFunc, void **oldFunc) {
    if (tbl->pltHookRegister) tbl->pltHookRegister(dev, inode, symbol, newFunc, oldFunc);
}
inline bool Api::pltHookCommit() {
    return tbl->pltHookCommit != nullptr && tbl->pltHookCommit();
}

} // namespace zygisk

extern "C" {

[[gnu::visibility("default"), maybe_unused]]
void zygisk_module_entry(zygisk::internal::api_table *, JNIEnv *);

[[gnu::visibility("default"), maybe_unused]]
void zygisk_companion_entry(int);

} // extern "C"
