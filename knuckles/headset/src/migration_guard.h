// Migration reads only the old root backend state, never VD app data/processes.
#ifndef PICO_MIXEDVR_MIGRATION_GUARD_H
#define PICO_MIXEDVR_MIGRATION_GUARD_H
#define LEGACY_ROOT "/data/adb/vdhs_mixedvr"

static bool migration_guard(int *held_lock) {
    struct stat st;
    *held_lock = -1;
    if (lstat(LEGACY_ROOT, &st)) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077)) return false;
    char requested[8];
    if (!read_small(LEGACY_ROOT "/requested", requested, sizeof requested, true) || strcmp(requested, "0")) return false;
    const char *pending[] = {LEGACY_ROOT "/enabled", LEGACY_ROOT "/baseline", LEGACY_ROOT "/inflight"};
    for (size_t i = 0; i < sizeof pending / sizeof pending[0]; ++i) {
        if (!lstat(pending[i], &st) || errno != ENOENT) return false;
    }
    // This file exists after the old supervisor has ever run. Opening read-only
    // does not change old journals; holding flock blocks its normal restart.
    int fd = open(LEGACY_ROOT "/supervisor.lock", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0077) || flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd); return false;
    }
    *held_lock = fd;
    return true;
}
#endif
