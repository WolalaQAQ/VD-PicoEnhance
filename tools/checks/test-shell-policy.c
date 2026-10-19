#include "../../zygisk/jni/vdhs_module_config.h"
#include "../../mod/native/vdhs_shell_policy.h"
#include "../../mod/native/vdhs_shell_abi.h"

size_t strlen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
void* memcpy(void* dst, const void* src, size_t n) {
    for (size_t i = 0; i < n; ++i) ((char*)dst)[i] = ((const char*)src)[i];
    return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int d = ((const unsigned char*)a)[i] - ((const unsigned char*)b)[i];
        if (d) return d;
    }
    return 0;
}

int fgetc(FILE* stream) {
    unsigned char c = (unsigned char)*stream->next;
    if (!c) return EOF;
    stream->next++;
    return c;
}

static int parse(const char* text, int fallback) {
    FILE stream = { text };
    return vdhs_config_read(&stream, fallback);
}

// Freestanding Windows entry point: exercise the same small policy functions
// without an Android runtime, Shell process, or a C runtime dependency.
int entry(void) {
    if (parse("# default\n0\n", 1) != 0) return 1;
    if (parse("\r\n# mode\r\n 2 \r\n", 0) != 2) return 2;
    if (parse("1 # enabled\n", 0) != 1) return 3;
    if (parse("# missing\n", 0) != 0) return 4;
    if (parse("garbage", 0) != 0) return 5;
    if (parse("10", 0) != 0) return 6;
    if (vdhs_shell_requested(1)) return 7;
    if (vdhs_shell_requested(0)) return 8;
    if (!vdhs_shell_requested(2)) return 9;
    if (vdhs_shell_requested(3)) return 10;
    if (!vdhs_shell_disable_for_vd(6, 0)) return 11;
    if (vdhs_shell_disable_for_vd(6, 1)) return 12;
    if (vdhs_shell_disable_for_vd(5, 0)) return 13;
    if (vdhs_shell_disable_for_vd(0, 0)) return 14;
    static char long_comment[3008];
    long_comment[0] = '#';
    for (int i = 1; i < 3000; ++i) long_comment[i] = 'x';
    long_comment[3000] = '\n';
    long_comment[3001] = '1';
    long_comment[3002] = '\n';
    if (parse(long_comment, 2) != 1) return 15;
    const char* vd = "VirtualDesktop.Android";
    char short_string[24] = { 0 };
    short_string[0] = (char)(strlen(vd) << 1);
    memcpy(short_string + 1, vd, strlen(vd));
    if (!vdhs_shell_string_is(short_string, vd)) return 16;
    if (vdhs_shell_string_is(short_string, "other")) return 17;
    const char* known = "com.picovr.picostreamassistant";
    struct { size_t capacity, length; const char* data; } long_string = { 33, strlen(known), known };
    if (!vdhs_shell_string_is((const char*)&long_string, known)) return 18;
    uintptr_t bounds[] = { (uintptr_t)&long_string, (uintptr_t)&long_string + 24 };
    if (!vdhs_shell_known_list_layout(bounds, known)) return 19;
    if (vdhs_shell_known_list_layout(bounds, "wrong app")) return 20;
    bounds[1] = bounds[0];
    if (vdhs_shell_known_list_layout(bounds, known)) return 21;
    return 0;
}
