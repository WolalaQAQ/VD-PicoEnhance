#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <openvr_driver.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "input_probe.h"

unsigned checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (0)

using Create = vr::EVRInputError (*)(void *, uint64_t, const char *, uint64_t *);
using Update = vr::EVRInputError (*)(void *, uint64_t, bool, double);
using Start = bool (*)(vr::IVRDriverContext *);
using Stop = void (*)();
using Probe = MvProbe *(*)();

struct BoolInput {
    virtual vr::EVRInputError make(uint64_t, const char *, uint64_t *) = 0;
    virtual vr::EVRInputError set(uint64_t, bool, double) = 0;
    virtual unsigned untouched() = 0;
};

template<unsigned Tag> struct Input final : BoolInput {
    std::atomic<unsigned> creates{0}, updates{0};
    vr::EVRInputError create_result = vr::VRInputError_None;
    vr::EVRInputError update_result = vr::VRInputError_None;
    uint64_t next_handle = 100 + Tag;
    uint64_t last_container = 0, last_component = 0, last_offset_bits = 0;
    const char *last_path = nullptr;
    uint64_t *last_out = nullptr;
    bool last_value = false;
    HANDLE entered = nullptr, release = nullptr;
    std::atomic<bool> threaded{false};

    vr::EVRInputError make(uint64_t container, const char *path, uint64_t *out) override {
        creates.fetch_add(1);
        last_container = container;
        last_path = path;
        last_out = out;
        if (out) *out = next_handle;
        SetLastError(0x1234 + Tag);
        return create_result;
    }
    vr::EVRInputError set(uint64_t component, bool value, double offset) override {
        updates.fetch_add(1);
        if (!threaded.load()) {
            last_component = component;
            last_value = value;
            std::memcpy(&last_offset_bits, &offset, sizeof(offset));
        }
        if (entered) {
            SetEvent(entered);
            WaitForSingleObject(release, INFINITE);
        }
        SetLastError(0x5678 + Tag);
        return update_result;
    }
    unsigned untouched() override { return 0xabc0 + Tag; }
};

void **slots(void *object) { return *static_cast<void ***>(object); }
void *slot(void *object, unsigned index) {
    return *reinterpret_cast<void *volatile *>(&slots(object)[index]);
}
vr::EVRInputError make(void *object, uint64_t container, const char *path, uint64_t *out) {
    return reinterpret_cast<Create>(slot(object, 0))(object, container, path, out);
}
vr::EVRInputError set(void *object, uint64_t component, bool value, double offset) {
    return reinterpret_cast<Update>(slot(object, 1))(object, component, value, offset);
}
void replace(void *object, unsigned index, void *value) {
    DWORD old = 0, ignored = 0;
    CHECK(VirtualProtect(&slots(object)[index], sizeof(void *), PAGE_READWRITE, &old));
    InterlockedExchangePointer(&slots(object)[index], value);
    CHECK(VirtualProtect(&slots(object)[index], sizeof(void *), old, &ignored));
}
DWORD protection(void *address) {
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(address, &info, sizeof(info)) == sizeof(info));
    return info.Protect;
}

struct Context final : vr::IVRDriverContext {
    void *input005 = nullptr, *input004 = nullptr, *input003 = nullptr;
    unsigned queries = 0, queries005 = 0, queries004 = 0, queries003 = 0;
    void *GetGenericInterface(const char *version, vr::EVRInitError *error) override {
        ++queries;
        void *result = nullptr;
        if (!std::strcmp(version, "IVRDriverInput_005")) { ++queries005; result = input005; }
        if (!std::strcmp(version, "IVRDriverInput_004")) { ++queries004; result = input004; }
        if (!std::strcmp(version, "IVRDriverInput_003")) { ++queries003; result = input003; }
        if (error) *error = result ? vr::VRInitError_None : vr::VRInitError_Init_InterfaceNotFound;
        return result;
    }
    vr::DriverHandle_t GetDriverHandle() override { return 1; }
};

void check_queries(const Context &context, bool require005 = false) {
    CHECK(context.queries004 == 1 && context.queries003 == 1);
    CHECK(context.queries005 <= 1);
    CHECK(context.queries == 2 + context.queries005);
    if (require005) CHECK(context.queries005 == 1);
}

MvComponent component(MvProbe *probe, uint64_t container) {
    for (const auto &entry : probe->components) if (entry.container == container) return entry;
    return {};
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    HMODULE library = LoadLibraryA(argv[1]);
    CHECK(library != nullptr);
    const auto start = reinterpret_cast<Start>(GetProcAddress(library, "test_start"));
    const auto stop = reinterpret_cast<Stop>(GetProcAddress(library, "test_stop"));
    const auto probe = reinterpret_cast<Probe>(GetProcAddress(library, "test_probe"));
    void *external = reinterpret_cast<void *>(GetProcAddress(library, "test_external_update"));
    CHECK(start && stop && probe && external);
    Input<0> a, shared;
    Input<1> b;
    Input<2> c;
    Context context;
    context.input004 = &a;
    context.input003 = &shared;
    const auto old_create = slot(&a, 0), old_update = slot(&a, 1), old_tail = slot(&a, 2);
    const DWORD old_protection = protection(slots(&a));
    uint64_t handle = 0;
    const char *scenario = argv[2];

    const bool only005 = !std::strcmp(scenario, "only005");
    const bool three_tables = !std::strcmp(scenario, "three-tables");
    const bool shared005004 = !std::strcmp(scenario, "shared005004");
    const bool shared005003 = !std::strcmp(scenario, "shared005003");
    const bool shared005004_then003 = !std::strcmp(scenario, "shared005004-then003");
    const bool shared005003_with004 = !std::strcmp(scenario, "shared005003-with004");
    const bool shared004003_with005 = !std::strcmp(scenario, "shared004003-with005");
    if (only005 || three_tables || shared005004 || shared005003 ||
        shared005004_then003 || shared005003_with004 || shared004003_with005) {
        // Model only the common Boolean prefix, not an invented full 005 SDK.
        context.input005 = &c;
        context.input004 = &a;
        context.input003 = &b;
        shared.next_handle = 200;
        if (only005) context.input004 = context.input003 = nullptr;
        if (shared005004 || shared005004_then003) context.input005 = &shared;
        if (shared005004) context.input003 = nullptr;
        if (shared005003 || shared005003_with004) {
            context.input005 = &shared;
            context.input003 = &a;
            context.input004 = shared005003 ? nullptr : &b;
        }
        if (shared004003_with005) context.input003 = &shared;
        void *objects[] = {context.input005, context.input004, context.input003};
        const unsigned tags[] = {context.input005 == &c ? 2u : 0u,
                                 context.input004 == &b ? 1u : 0u,
                                 context.input003 == &b ? 1u : 0u};
        void *originals[3][3]{};
        DWORD protections[3]{};
        uint64_t handles[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            if (!objects[i]) continue;
            for (unsigned j = 0; j < 3; ++j) originals[i][j] = slot(objects[i], j);
            protections[i] = protection(slots(objects[i]));
        }
        if (three_tables) {
            CHECK(slots(&a) != slots(&b) && slots(&a) != slots(&c) && slots(&b) != slots(&c));
        } else if (!only005) {
            CHECK(slots(&a) == slots(&shared));
            if (shared005004_then003 || shared005003_with004) CHECK(slots(&a) != slots(&b));
            if (shared004003_with005) CHECK(slots(&a) != slots(&c));
        }
        CHECK(start(&context));
        CHECK(!start(&context));
        unsigned available = 0;
        for (unsigned i = 0; i < 3; ++i) {
            if (!objects[i]) continue;
            ++available;
            CHECK(slot(objects[i], 0) != originals[i][0]);
            CHECK(slot(objects[i], 1) != originals[i][1]);
            CHECK(slot(objects[i], 2) == originals[i][2]);
            CHECK(protection(slots(objects[i])) == protections[i]);
            const uint64_t container = 70 + i;
            CHECK(make(objects[i], container, MV_TRACKPAD_TOUCH, &handles[i]) == vr::VRInputError_None);
            CHECK(GetLastError() == 0x1234 + tags[i]);
            CHECK(handles[i] && component(probe(), container).component == handles[i]);
            CHECK(!component(probe(), container).known);
            CHECK(set(objects[i], handles[i], true, -0.25) == vr::VRInputError_None);
            CHECK(GetLastError() == 0x5678 + tags[i]);
            auto item = component(probe(), container);
            CHECK(item.known && item.touched && item.updates == 1);
            CHECK(set(objects[i], handles[i], false, 0.125) == vr::VRInputError_None);
            CHECK(GetLastError() == 0x5678 + tags[i]);
            item = component(probe(), container);
            CHECK(item.known && !item.touched && item.updates == 2);
        }
        unsigned mapped = 0;
        for (const auto &entry : probe()->components) if (entry.component) ++mapped;
        CHECK(mapped == available);
        const auto check_calls = [&](const auto &input) {
            const unsigned expected = static_cast<unsigned>(context.input005 == &input) +
                static_cast<unsigned>(context.input004 == &input) +
                static_cast<unsigned>(context.input003 == &input);
            CHECK(input.creates.load() == expected);
            CHECK(input.updates.load() == 2 * expected);
        };
        check_calls(a); check_calls(shared); check_calls(b); check_calls(c);
        stop();
        for (unsigned i = 0; i < 3; ++i) {
            if (!objects[i]) continue;
            CHECK(set(objects[i], handles[i], true, 0.0) == vr::VRInputError_None);
            CHECK(component(probe(), 70 + i).updates == 2);
            uint64_t ignored = 0;
            CHECK(make(objects[i], 80 + i, MV_TRACKPAD_TOUCH, &ignored) == vr::VRInputError_None);
            CHECK(component(probe(), 80 + i).component == 0);
            CHECK(protection(slots(objects[i])) == protections[i]);
        }
        CHECK(!start(&context));
        check_queries(context, true);
    } else if (!std::strcmp(scenario, "missing")) {
        context.input004 = context.input003 = nullptr;
        CHECK(!start(&context));
        check_queries(context);
        CHECK(slot(&a, 0) == old_create && slot(&a, 1) == old_update);
    } else if (!std::strcmp(scenario, "external-before")) {
        replace(&b, 1, external);
        context.input003 = &b;
        CHECK(!start(&context));
        CHECK(slot(&a, 0) == old_create && slot(&a, 1) == old_update);
        CHECK(slot(&b, 1) == external);
    } else if (!std::strcmp(scenario, "clone-table")) {
        void *clone[3] = {old_create, old_update, old_tail};
        struct Object { void **vtable; } object{clone};
        context.input004 = context.input003 = &object;
        CHECK(!start(&context));
        CHECK(clone[0] == old_create && clone[1] == old_update);
    } else if (!std::strcmp(scenario, "stop-before-start")) {
        stop();
        CHECK(!start(&context));
        CHECK(slot(&a, 0) == old_create && slot(&a, 1) == old_update);
    } else {
        if (!std::strcmp(scenario, "only003")) context.input004 = nullptr;
        if (!std::strcmp(scenario, "only004")) context.input003 = nullptr;
        if (!std::strcmp(scenario, "two-tables")) context.input003 = &b;
        if (!std::strcmp(scenario, "historical")) {
            CHECK(make(&a, 7, MV_TRACKPAD_TOUCH, &handle) == vr::VRInputError_None);
        }
        CHECK(start(&context));
        check_queries(context);
        CHECK(slot(&a, 0) != old_create && slot(&a, 1) != old_update);
        CHECK(slot(&a, 2) == old_tail);
        CHECK(protection(slots(&a)) == old_protection);
        CHECK(!start(&context));
        CHECK(component(probe(), 7).component == 0);

        if (!std::strcmp(scenario, "historical")) {
            CHECK(set(&a, handle, true, 0.0) == vr::VRInputError_None);
            CHECK(component(probe(), 7).component == 0);
        } else if (!std::strcmp(scenario, "failed-and-path")) {
            a.create_result = vr::VRInputError_InvalidParam;
            CHECK(make(&a, 7, MV_TRACKPAD_TOUCH, &handle) == vr::VRInputError_InvalidParam);
            CHECK(GetLastError() == 0x1234);
            CHECK(component(probe(), 7).component == 0);
            a.create_result = vr::VRInputError_None;
            for (const char *path : {"/input/trackpad/touch_extra", "/input/Trackpad/touch",
                                     "/input/grip/touch", static_cast<const char *>(nullptr)}) {
                CHECK(make(&a, 7, path, &handle) == vr::VRInputError_None);
                CHECK(component(probe(), 7).component == 0);
            }
            CHECK(make(&a, 7, MV_TRACKPAD_TOUCH, nullptr) == vr::VRInputError_None);
            CHECK(component(probe(), 7).component == 0);
            const char path[] = MV_TRACKPAD_TOUCH;
            CHECK(make(&a, 7, path, &handle) == vr::VRInputError_None);
            CHECK(a.last_container == 7 && a.last_path == path && a.last_out == &handle);
            CHECK(component(probe(), 7).component == handle);
            CHECK(!component(probe(), 7).known);
            a.update_result = vr::VRInputError_InvalidHandle;
            CHECK(set(&a, handle, true, -0.25) == vr::VRInputError_InvalidHandle);
            CHECK(GetLastError() == 0x5678);
            CHECK(!component(probe(), 7).known && !component(probe(), 7).updates);
            a.update_result = vr::VRInputError_None;
            uint64_t bits = 0x7ff8000000000123ULL;
            double offset;
            std::memcpy(&offset, &bits, sizeof(offset));
            CHECK(set(&a, handle, true, offset) == vr::VRInputError_None);
            CHECK(a.last_component == handle && a.last_value && a.last_offset_bits == bits);
            CHECK(GetLastError() == 0x5678);
            auto item = component(probe(), 7);
            CHECK(item.known && item.touched && item.updates == 1);
            CHECK(item.last_update_ms <= GetTickCount64());
        } else {
            const uint64_t before = GetTickCount64();
            CHECK(make(&a, 7, MV_TRACKPAD_TOUCH, &handle) == vr::VRInputError_None);
            CHECK(!component(probe(), 7).known);
            CHECK(set(&a, handle, true, -0.25) == vr::VRInputError_None);
            auto item = component(probe(), 7);
            CHECK(item.known && item.touched && item.updates == 1);
            CHECK(item.last_update_ms >= before && item.last_update_ms <= GetTickCount64());
            if (!std::strcmp(scenario, "two-tables")) {
                uint64_t second = 0;
                CHECK(slots(&a) != slots(&b));
                CHECK(make(&b, 8, MV_TRACKPAD_TOUCH, &second) == vr::VRInputError_None);
                CHECK(set(&b, second, false, 0.125) == vr::VRInputError_None);
                CHECK(b.creates == 1 && b.updates == 1 && a.creates == 1 && a.updates == 1);
                item = component(probe(), 8);
                CHECK(item.known && !item.touched && item.updates == 1);
            } else if (!std::strcmp(scenario, "shared")) {
                CHECK(slots(&a) == slots(&shared));
                shared.next_handle = 200;
                uint64_t second = 0;
                CHECK(make(&shared, 8, MV_TRACKPAD_TOUCH, &second) == vr::VRInputError_None);
                CHECK(set(&shared, second, false, 0.125) == vr::VRInputError_None);
                CHECK(shared.creates == 1 && shared.updates == 1);
                CHECK(component(probe(), 8).known && !component(probe(), 8).touched);
            } else if (!std::strcmp(scenario, "drain")) {
                a.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                a.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                CHECK(a.entered && a.release);
                std::thread caller([&] { set(&a, handle, false, 0.5); });
                CHECK(WaitForSingleObject(a.entered, 2000) == WAIT_OBJECT_0);
                std::atomic<bool> stopping{false}, stopped{false};
                std::thread cleanup([&] { stopping.store(true); stop(); stopped.store(true); });
                while (!stopping.load()) std::this_thread::yield();
                Sleep(30);
                CHECK(!stopped.load());
                SetEvent(a.release);
                caller.join();
                cleanup.join();
                CHECK(stopped.load());
                CHECK(component(probe(), 7).updates == 1); // disabled while original was blocked
                CloseHandle(a.entered);
                CloseHandle(a.release);
                a.entered = a.release = nullptr;
            } else if (!std::strcmp(scenario, "stress")) {
                a.threaded.store(true);
                constexpr unsigned thread_count = 6, iterations = 12000;
                std::vector<std::thread> threads;
                for (unsigned i = 0; i < thread_count; ++i) {
                    threads.emplace_back([&] {
                        for (unsigned n = 0; n < iterations; ++n) set(&a, handle, n % 2, 0.25);
                    });
                }
                for (auto &thread : threads) thread.join();
                CHECK(component(probe(), 7).updates == 1 + thread_count * iterations);
                threads.clear();
                std::atomic<bool> running{true};
                for (unsigned i = 0; i < thread_count; ++i) {
                    threads.emplace_back([&] {
                        while (running.load()) set(&a, handle, true, -0.125);
                    });
                }
                Sleep(20);
                stop();
                const uint64_t frozen = component(probe(), 7).updates;
                Sleep(20);
                CHECK(component(probe(), 7).updates == frozen);
                running.store(false);
                for (auto &thread : threads) thread.join();
            } else if (!std::strcmp(scenario, "external-after")) {
                const auto cached = reinterpret_cast<Update>(slot(&a, 1));
                replace(&a, 1, external);
                stop();
                CHECK(slot(&a, 1) == external);
                CHECK(set(&a, handle, false, 0.0) == vr::VRInputError_InvalidParam);
                CHECK(cached(&a, handle, false, 0.0) == vr::VRInputError_None);
                CHECK(component(probe(), 7).updates == 1);
            } else if (!std::strcmp(scenario, "pin")) {
                CHECK(FreeLibrary(library));
                HMODULE still_loaded = nullptr;
                CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                          reinterpret_cast<LPCWSTR>(start), &still_loaded));
                CHECK(still_loaded == library);
                CHECK(set(&a, handle, false, 0.0) == vr::VRInputError_None);
                CHECK(component(probe(), 7).updates == 2);
            } else {
                CHECK(!std::strcmp(scenario, "stop") || !std::strcmp(scenario, "only003") ||
                      !std::strcmp(scenario, "only004"));
            }
        }
        stop();
        const auto frozen = component(probe(), 7);
        const unsigned update_count = a.updates.load();
        if (!std::strcmp(scenario, "external-after")) replace(&a, 1, old_update);
        CHECK(set(&a, handle, false, -1.0) == vr::VRInputError_None);
        CHECK(a.updates.load() == update_count + 1);
        CHECK(component(probe(), 7).updates == frozen.updates);
        a.next_handle = 300;
        CHECK(make(&a, 9, MV_TRACKPAD_TOUCH, &handle) == vr::VRInputError_None);
        CHECK(!component(probe(), 9).component);
        CHECK(!start(&context));
    }
    stop();
    CHECK(protection(slots(&a)) == old_protection);
    std::printf("{\"scenario\":\"%s\",\"checks\":%u,\"ok\":true}\n", scenario, checks);
    return 0;
}
