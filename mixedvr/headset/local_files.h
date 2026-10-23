// App-private files only. Directory FDs pin every app-controlled path component.
#include <dirent.h>
#include <sys/xattr.h>

#define LOCAL_APP "VirtualDesktop.Android"
#define LOCAL_REQUEST "mixedvr_request.txt"
#define LOCAL_STATUS "mixedvr_status.txt"

struct local_source { int dir; uid_t uid; gid_t gid; };
static int local_process = -1;

static bool local_process_matches(int fd, uid_t uid) {
    struct stat st;
    if (fstat(fd, &st) || st.st_uid != uid) return false;
    int cmd = openat(fd, "cmdline", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (cmd < 0) return false;
    char name[sizeof LOCAL_APP + 1];
    ssize_t n = read(cmd, name, sizeof name);
    close(cmd);
    return n >= (ssize_t)sizeof LOCAL_APP && !memcmp(name, LOCAL_APP, sizeof LOCAL_APP);
}

static bool local_find_process(uid_t uid, bool *changed) {
    if (local_process >= 0 && local_process_matches(local_process, uid)) return true;
    if (local_process >= 0) close(local_process);
    local_process = -1;
    DIR *proc = opendir("/proc");
    if (!proc) return false;
    struct dirent *entry;
    while ((entry = readdir(proc))) {
        const char *p = entry->d_name;
        if (!*p) continue;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p) continue;
        int fd = openat(dirfd(proc), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        if (local_process_matches(fd, uid)) { local_process = fd; *changed = true; break; }
        close(fd);
    }
    closedir(proc);
    // A pinned /proc/PID FD refers to that task, not a later reused numeric PID.
    return local_process >= 0;
}

static bool local_read(struct local_source *source, char text[128], size_t *length, bool *changed) {
    source->dir = -1;
    *changed = false;
    int data = open("/data/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (data < 0) return false;
    int app = openat(data, LOCAL_APP, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(data);
    if (app < 0) return false;
    struct stat st;
    bool ok = fstat(app, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid >= 10000;
    if (ok) { source->uid = st.st_uid; source->gid = st.st_gid; }
    int dir = ok ? openat(app, "vdhs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
    close(app);
    if (dir < 0) return false;
    ok = fstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == source->uid &&
         local_find_process(source->uid, changed);
    int fd = ok ? openat(dir, LOCAL_REQUEST, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    if (fd < 0) { close(dir); return false; }
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == source->uid &&
         st.st_nlink == 1 && st.st_size > 0 && st.st_size < 128;
    ssize_t n = ok ? read(fd, text, 128) : -1;
    close(fd);
    if (n <= 0 || n != st.st_size || n >= 128) { close(dir); return false; }
    source->dir = dir;
    *length = (size_t)n;
    return true;
}

static bool local_write(const struct local_source *source, const char *text) {
    uint64_t random;
    if (getrandom(&random, sizeof random, 0) != sizeof random) return false;
    char tmp[64];
    snprintf(tmp, sizeof tmp, ".mixedvr_status.%016" PRIx64, random);
    int fd = openat(source->dir, tmp, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    // Use the app directory's exact SELinux type/MCS label, not a shell command.
    char label[256];
    ssize_t size = fgetxattr(source->dir, "security.selinux", label, sizeof label);
    bool ok = size > 0 && fsetxattr(fd, "security.selinux", label, (size_t)size, 0) == 0 &&
              write_all(fd, text, strlen(text)) && fchmod(fd, 0600) == 0 &&
              fchown(fd, source->uid, source->gid) == 0;
    if (close(fd)) ok = false;
    // rename replaces a target symlink/hardlink itself; it never opens its target.
    if (ok && renameat(source->dir, tmp, source->dir, LOCAL_STATUS)) ok = false;
    if (!ok) unlinkat(source->dir, tmp, 0);
    return ok;
}
