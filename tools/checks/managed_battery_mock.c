#include <stddef.h>
#include <stdint.h>

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
void* memset(void* dst, int c, size_t n) {
    for (size_t i = 0; i < n; ++i) ((unsigned char*)dst)[i] = (unsigned char)c;
    return dst;
}
void* memcpy(void* dst, const void* src, size_t n) {
    for (size_t i = 0; i < n; ++i) ((unsigned char*)dst)[i] = ((const unsigned char*)src)[i];
    return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int d = ((const unsigned char*)a)[i] - ((const unsigned char*)b)[i];
        if (d) return d;
    }
    return 0;
}

void* vdhs_resolve_mapped_symbol(const char*, const char*);
#define LOGE(...) ((void)0)
#include "managed_battery_source.h"

typedef struct {
    int image;
    const char* ns;
    const char* name;
    int is_value, type_code;
    int32_t size;
} MockClass;
typedef struct {
    MockClass* owner;
    const char* name;
    MockClass* type;
    uint32_t flags, offset;
} MockField;

static const char* images[] = {
    "VirtualDesktop.Mobile", "VirtualDesktop.Mobile.Shared", "VirtualDesktop.Interfaces"
};
static MockClass classes[] = {
    { 0, "VirtualDesktop.Mobile", "InputManager", 0, 0x12, 32 },
    { 0, "VirtualDesktop.Mobile", "Game", 0, 0x12, 0 },
    { 1, "VirtualDesktop.Mobile", "FrameData", 1, 0x11, 0 },
    { 2, "VirtualDesktop.Interfaces", "DeviceState", 1, 0x11, 0 },
    { 2, "VirtualDesktop.Interfaces", "ControllerPose", 1, 0x11, 0 },
    { 3, "System", "SByte", 1, 0x04, 1 },
    { 3, "System", "Byte", 1, 0x05, 1 },
};
static MockField fields[] = {
    { &classes[0], "_game", &classes[1], 0x10, 0 },
    { &classes[1], "_frameData", &classes[2], 0, 0 },
    { &classes[2], "DeviceState", &classes[3], 0, 0 },
    { &classes[3], "LeftControllerPose", &classes[4], 0, 0 },
    { &classes[3], "RightControllerPose", &classes[4], 0, 0 },
    { &classes[4], "BatteryPercentage", &classes[5], 0, 0 },
};
static unsigned char objects[2][2048], expected[2][2048], at_exit[2][2048];
static uint32_t expected_left, expected_right;
static MP current_game;
static int missing_image, missing_class, missing_field;
static int no_domain, no_vtable, wrong_object, force_image_fallback, null_cookie;
static int depth, enters, exits, errors, static_reads, field_queries, fallbacks;
static void** entered_marker;
static int cookie_token, domain_token, vtable_token;

static void guarded(void) { if (depth != 1) ++errors; }

static void* mock_enter(void** marker) {
    if (depth || !marker || *marker) ++errors;
    depth = 1;
    ++enters;
    entered_marker = marker;
    *marker = &cookie_token;
    return null_cookie ? NULL : &cookie_token;
}
static void mock_exit(void* cookie, void** marker) {
    guarded();
    if (cookie != (null_cookie ? NULL : &cookie_token) || marker != entered_marker ||
        *marker != &cookie_token) ++errors;
    memcpy(at_exit, objects, sizeof objects);
    depth = 0;
    ++exits;
}
static MP mock_image_loaded(const char* name) {
    guarded();
    if (force_image_fallback) return NULL;
    for (int i = 0; i < 3; ++i)
        if (i != missing_image && !strcmp(name, images[i])) return (MP)images[i];
    return NULL;
}
static void mock_assembly_foreach(void (*cb)(MP, MP), MP user) {
    guarded();
    ++fallbacks;
    for (int i = 0; i < 3; ++i) if (i != missing_image) cb((MP)images[i], user);
}
static MP mock_assembly_get_image(MP assembly) { guarded(); return assembly; }
static const char* mock_image_get_name(MP image) { guarded(); return image; }
static MP mock_class_from_name(MP image, const char* ns, const char* name) {
    guarded();
    for (int i = 0; i < 5; ++i)
        if (i != missing_class && image == images[classes[i].image] &&
            !strcmp(ns, classes[i].ns) && !strcmp(name, classes[i].name)) return &classes[i];
    return NULL;
}
static MP mock_class_get_field_from_name(MP klass, const char* name) {
    guarded();
    ++field_queries;
    for (int i = 0; i < 6; ++i)
        if (i != missing_field && klass == fields[i].owner && !strcmp(name, fields[i].name)) return &fields[i];
    return NULL;
}
static MP mock_domain_get(void) { guarded(); return no_domain ? NULL : &domain_token; }
static MP mock_class_vtable(MP domain, MP klass) {
    guarded();
    if (domain != &domain_token || klass != &classes[0]) ++errors;
    return no_vtable ? NULL : &vtable_token;
}
static void mock_field_static_get_value(MP vtable, MP field, void* out) {
    guarded();
    ++static_reads;
    if (vtable != &vtable_token || field != &fields[0]) ++errors;
    *(MP*)out = current_game;
}
static MP mock_object_get_class(MP object) {
    guarded();
    if (object != objects[0] && object != objects[1]) ++errors;
    return &classes[wrong_object ? 0 : 1];
}
static uint32_t mock_field_get_offset(MP field) { guarded(); return ((MockField*)field)->offset; }
static MP mock_field_get_type(MP field) { guarded(); return ((MockField*)field)->type; }
static uint32_t mock_field_get_flags(MP field) { guarded(); return ((MockField*)field)->flags; }
static MP mock_class_from_mono_type(MP type) { guarded(); return type; }
static int mock_class_is_valuetype(MP klass) { guarded(); return ((MockClass*)klass)->is_value; }
static int32_t mock_class_value_size(MP klass, uint32_t* align) {
    guarded();
    if (!((MockClass*)klass)->is_value) ++errors;
    *align = 8;
    return ((MockClass*)klass)->size;
}
static int32_t mock_class_instance_size(MP klass) { guarded(); return ((MockClass*)klass)->size; }
static int mock_type_get_type(MP type) { guarded(); return ((MockClass*)type)->type_code; }
static MP forbidden_api(void) { ++errors; return NULL; }

void* vdhs_resolve_mapped_symbol(const char* lib, const char* name) {
    if (strcmp(lib, "libmonosgen-2.0.so") || depth) ++errors;
#ifdef MISSING_API
    if (!strcmp(name, MISSING_API)) return NULL;
#endif
#define API(n) if (!strcmp(name, "mono_" #n)) return (void*)mock_##n
    API(image_loaded);
    API(assembly_foreach);
    API(assembly_get_image);
    API(image_get_name);
    API(class_from_name);
    API(class_get_field_from_name);
    API(class_vtable);
    API(domain_get);
    API(field_static_get_value);
    API(object_get_class);
    API(field_get_offset);
    API(field_get_type);
    API(field_get_flags);
    API(class_from_mono_type);
    API(class_is_valuetype);
    API(class_value_size);
    API(class_instance_size);
    API(type_get_type);
#undef API
    if (!strcmp(name, "mono_threads_enter_gc_unsafe_region")) return (void*)mock_enter;
    if (!strcmp(name, "mono_threads_exit_gc_unsafe_region")) return (void*)mock_exit;
    static const char* unused_core[] = {
        "mono_class_get_method_from_name", "mono_runtime_invoke", "mono_object_unbox",
        "mono_object_to_string", "mono_string_to_utf8", "mono_get_root_domain", "mono_thread_attach",
        "mono_string_new", "mono_field_static_set_value", "mono_free", "mono_class_get_parent",
        "mono_class_get_property_from_name", "mono_property_get_get_method", "mono_property_get_set_method"
    };
    for (size_t i = 0; i < sizeof unused_core / sizeof unused_core[0]; ++i)
        if (!strcmp(name, unused_core[i])) return (void*)forbidden_api;
    ++errors;
    return NULL;
}

static void setup(int variant) {
    memset(&g_battery_layout, 0, sizeof g_battery_layout);
    memset(objects[0], 0xA7, sizeof objects[0]);
    memset(objects[1], 0xD3, sizeof objects[1]);
    memcpy(expected, objects, sizeof objects);
    memcpy(at_exit, objects, sizeof objects);
    missing_image = missing_class = missing_field = -1;
    no_domain = no_vtable = wrong_object = force_image_fallback = null_cookie = 0;
    errors = depth = enters = exits = static_reads = field_queries = fallbacks = 0;
    current_game = objects[0];
    for (int i = 0; i < 5; ++i) classes[i].is_value = i >= 2;
    int32_t psize = 20 + variant * 3;
    uint32_t b = (uint32_t)(variant * 7 % psize);
    uint32_t l = 11 + (uint32_t)variant;
    uint32_t r = l + (uint32_t)psize + 5 + (uint32_t)variant;
    uint32_t d = 13 + (uint32_t)variant * 2;
    uint32_t f = 2 * sizeof(void*) + 19 + (uint32_t)variant * 7;
    classes[4].size = psize;
    classes[3].size = (int32_t)r + psize + 9;
    classes[2].size = (int32_t)d + classes[3].size + 25;
    classes[1].size = (int32_t)f + classes[2].size + 33;
    fields[0].type = &classes[1];
    fields[1].type = &classes[2];
    fields[2].type = &classes[3];
    fields[3].type = fields[4].type = &classes[4];
    fields[5].type = &classes[5];
    for (int i = 0; i < 6; ++i) fields[i].flags = i == 0 ? 0x10 : 0;
    fields[1].offset = f;
    fields[2].offset = 2 * sizeof(void*) + d;
    fields[3].offset = 2 * sizeof(void*) + l;
    fields[4].offset = 2 * sizeof(void*) + r;
    fields[5].offset = 2 * sizeof(void*) + b;
    expected_left = f + d + l + b;
    expected_right = f + d + r + b;
}

static int check_call(int rc, int left, int right, int object) {
    if (vdhs_managed_set_controller_battery(left, right) != rc) return 1;
    if (rc == 1) {
        expected[object][expected_left] = (unsigned char)left;
        expected[object][expected_right] = (unsigned char)right;
    }
    if (memcmp(expected, objects, sizeof objects)) return 2;
    if (memcmp(at_exit, objects, sizeof objects)) return 3;
    if (errors || depth || enters != exits) return 4;
    return 0;
}

#define CHECK(rc, left, right, obj) do { int error = check_call(rc, left, right, obj); if (error) return 10 + error; } while (0)

int entry(void) {
    setup(0);
    switch (BATTERY_CASE) {
        case 0:
            for (int i = 0; i < 12; ++i) {
                setup(i);
                CHECK(1, 17 + i, 98 - i, 0);
                if (enters != 1 || static_reads != 1) return 20;
            }
            break;
        case 1:
            CHECK(1, 25, 51, 0);
            current_game = objects[1];
            CHECK(1, 73, 99, 1);
            if (static_reads != 2 || enters != 2) return 21;
            break;
        case 2:
            for (int i = 0; i < 3; ++i) {
                setup(i);
                missing_image = i;
                CHECK(0, 25, 51, 0);
                missing_image = -1;
                CHECK(1, 25, 51, 0);
                if (enters != 2) return 22;
            }
            break;
        case 3:
        case 4:
            for (int i = 0; i < (BATTERY_CASE == 3 ? 5 : 6); ++i) {
                setup(i);
                if (BATTERY_CASE == 3) missing_class = i;
                else missing_field = i;
                CHECK(-1, 25, 51, 0);
                CHECK(-1, 25, 51, 0);
                if (enters != 2 || static_reads) return 23;
            }
            break;
        case 5:
        case 6:
            for (int i = 0; i < 6; ++i) {
                for (int mode = 0; mode < 2; ++mode) {
                    setup(i);
                    if (BATTERY_CASE == 5) fields[i].type = mode ? NULL : &classes[6];
                    else fields[i].flags = mode ? 0x40 : (i ? 0x10 : 0);
                    CHECK(-1, 25, 51, 0);
                }
            }
            break;
        case 7:
            for (int i = 1; i < 6; ++i) {
                for (int mode = 0; mode < 3; ++mode) {
                    setup(i);
                    fields[i].offset = mode == 0 ? 2 * sizeof(void*) - 1 :
                                       mode == 1 ? 0xFFFFFFFFu : 2000;
                    CHECK(-1, 25, 51, 0);
                }
            }
            break;
        case 8:
            for (int i = 1; i < 5; ++i) {
                for (int mode = 0; mode < 3; ++mode) {
                    setup(i);
                    classes[i].size = mode == 0 ? 0 : mode == 1 ? -1 : 1;
                    CHECK(-1, 25, 51, 0);
                }
            }
            break;
        case 9:
            for (int i = 0; i < 2; ++i) {
                setup(i);
                fields[4].offset = fields[3].offset + (uint32_t)i;
                CHECK(-1, 25, 51, 0);
            }
            break;
        case 10:
            wrong_object = 1;
            CHECK(-1, 25, 51, 0);
            break;
        case 11:
        case 12:
        case 13:
            no_domain = BATTERY_CASE == 11;
            no_vtable = BATTERY_CASE == 12;
            if (BATTERY_CASE == 13) current_game = NULL;
            CHECK(0, 25, 51, 0);
            no_domain = no_vtable = 0;
            current_game = objects[0];
            CHECK(1, 25, 51, 0);
            if (enters != 2) return 24;
            break;
        case 14:
            for (int i = 0; i < 5; ++i) {
                setup(i);
                classes[i].is_value = !classes[i].is_value;
                CHECK(-1, 25, 51, 0);
            }
            break;
        case 15:
            CHECK(1, -1, 100, 0);
            CHECK(1, 0, -1, 0);
            break;
        case 16: {
            CHECK(1, 25, 51, 0);
            int queries = field_queries;
            current_game = objects[1];
            CHECK(1, 73, 99, 1);
            if (queries != field_queries || !queries || static_reads != 2) return 25;
            break;
        }
        case 17:
            CHECK(-1, 25, 51, 0);
            if (enters || exits || static_reads) return 26;
            break;
        case 18:
            force_image_fallback = 1;
            CHECK(1, 25, 51, 0);
            CHECK(1, 73, 99, 0);
            if (fallbacks != 3) return 27;
            break;
        case 19:
            null_cookie = 1;
            CHECK(1, 25, 51, 0);
            if (enters != 1 || exits != 1) return 28;
            break;
        default: return 99;
    }
    return 0;
}
