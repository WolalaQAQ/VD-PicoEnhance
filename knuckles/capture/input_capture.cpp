#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <openvr_driver.h>
#include "input_capture.h"

static_assert(sizeof(void *) == 8, "The capture ABI is Windows x64 only");

namespace {
using Create = vr::EVRInputError (*)(void *, vr::PropertyContainerHandle_t,
                                    const char *, vr::VRInputComponentHandle_t *);
using Update = vr::EVRInputError (*)(void *, vr::VRInputComponentHandle_t, bool, double);
constexpr uint64_t closed = uint64_t{1} << 63;
std::atomic<uint64_t> gate{closed};
MvProbe probe{};
bool attempted = false;

// One immutable dispatch record per distinct public interface vtable, not per
// object. It remains valid even when an external hook caches our wrapper.
struct Table {
    void **slots = nullptr;
    std::atomic<void *> original[2]{};
};
Table tables[3];

bool admit() {
    uint64_t state = gate.load(std::memory_order_acquire);
    while (!(state & closed)) {
        if (gate.compare_exchange_weak(state, state + 1,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) return true;
    }
    return false;
}

struct Callback {
    const bool admitted = admit();
    ~Callback() { if (admitted) gate.fetch_sub(1, std::memory_order_release); }
    bool recording() const {
        return admitted && !(gate.load(std::memory_order_acquire) & closed);
    }
};

template<unsigned N>
vr::EVRInputError create(void *self, vr::PropertyContainerHandle_t container,
                         const char *path, vr::VRInputComponentHandle_t *out) {
    const Callback callback;
    const auto original = reinterpret_cast<Create>(
        tables[N].original[0].load(std::memory_order_acquire));
    const auto result = original(self, container, path, out);
    const DWORD error = GetLastError();
    if (callback.recording() && result == vr::VRInputError_None && out)
        mv_created(&probe, container, path, *out, static_cast<int32_t>(result));
    SetLastError(error);
    return result;
}

template<unsigned N>
vr::EVRInputError update(void *self, vr::VRInputComponentHandle_t component,
                         bool value, double offset) {
    const Callback callback;
    const auto original = reinterpret_cast<Update>(
        tables[N].original[1].load(std::memory_order_acquire));
    const auto result = original(self, component, value, offset);
    const DWORD error = GetLastError();
    if (callback.recording() && result == vr::VRInputError_None)
        mv_updated(&probe, component, value, static_cast<int32_t>(result), GetTickCount64());
    SetLastError(error);
    return result;
}

void *const wrappers[3][2] = {
    {reinterpret_cast<void *>(&create<0>), reinterpret_cast<void *>(&update<0>)},
    {reinterpret_cast<void *>(&create<1>), reinterpret_cast<void *>(&update<1>)},
    {reinterpret_cast<void *>(&create<2>), reinterpret_cast<void *>(&update<2>)}
};

void *read_slot(void **slot) {
    // An aligned pointer read does not require write access to a read-only vtable.
    return *reinterpret_cast<void *volatile *>(slot);
}

bool readable(const MEMORY_BASIC_INFORMATION &region) {
    if (region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD)) return false;
    const DWORD p = region.Protect & 0xff;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

bool span(void *address, size_t bytes, MEMORY_BASIC_INFORMATION &region) {
    if (!VirtualQuery(address, &region, sizeof(region)) || !readable(region)) return false;
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const auto base = reinterpret_cast<uintptr_t>(region.BaseAddress);
    return begin >= base && bytes <= region.RegionSize &&
           begin - base <= region.RegionSize - bytes;
}

bool prepare(void *object, Table &table) {
    MEMORY_BASIC_INFORMATION region{};
    if ((reinterpret_cast<uintptr_t>(object) % alignof(void *)) ||
        !span(object, sizeof(void *), region)) return false;
    table.slots = *static_cast<void ***>(object);
    if ((reinterpret_cast<uintptr_t>(table.slots) % alignof(void *)) ||
        !span(table.slots, 2 * sizeof(void *), region) || region.Type != MEM_IMAGE)
        return false;
    const DWORD protection = region.Protect & 0xff;
    if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY) return false;
    const void *owner = region.AllocationBase;
    // Reject clone tables and cross-image/trampoline targets rather than silently
    // replacing an observable external hook. No instructions are inspected.
    for (unsigned slot = 0; slot != 2; ++slot) {
        void *target = read_slot(&table.slots[slot]);
        MEMORY_BASIC_INFORMATION code{};
        if (!VirtualQuery(target, &code, sizeof(code)) || code.State != MEM_COMMIT ||
            code.Type != MEM_IMAGE || code.AllocationBase != owner ||
            (code.Protect & PAGE_GUARD)) return false;
        const DWORD p = code.Protect & 0xff;
        if (p != PAGE_EXECUTE && p != PAGE_EXECUTE_READ &&
            p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) return false;
        table.original[slot].store(target, std::memory_order_release);
    }
    return true;
}

// Only called during start/failed-start rollback, never during Cleanup.
bool exchange_table(unsigned index, bool install) {
    Table &table = tables[index];
    DWORD previous = 0;
    if (!VirtualProtect(table.slots, 2 * sizeof(void *), PAGE_READWRITE, &previous))
        return false;
    bool success = true;
    for (unsigned slot = 0; slot != 2; ++slot) {
        void *original = table.original[slot].load(std::memory_order_acquire);
        void *expected = install ? original : wrappers[index][slot];
        void *replacement = install ? wrappers[index][slot] : original;
        if (InterlockedCompareExchangePointer(&table.slots[slot], replacement, expected) != expected)
            success = false; // Another owner wins; never force a slot back.
    }
    if (install && !success) {
        for (unsigned slot = 0; slot != 2; ++slot)
            InterlockedCompareExchangePointer(&table.slots[slot],
                table.original[slot].load(std::memory_order_acquire), wrappers[index][slot]);
    }
    DWORD ignored = 0;
    const bool restored = VirtualProtect(table.slots, 2 * sizeof(void *), previous, &ignored) != FALSE;
    return success && restored;
}
} // namespace

bool mv_capture_start(vr::IVRDriverContext *context) {
    if (attempted) return false;
    attempted = true;
    if (!context) return false;
    unsigned count = 0;
    // These versions share the Boolean Create/Update prefix. Keep the older
    // tables: a runtime may expose all three while its drivers use different ones.
    const char *const versions[] = {
        "IVRDriverInput_005", "IVRDriverInput_004", "IVRDriverInput_003"
    };
    for (const char *version : versions) {
        vr::EVRInitError error = vr::VRInitError_None;
        void *object = context->GetGenericInterface(version, &error);
        if (!object || error != vr::VRInitError_None) continue;
        if (!prepare(object, tables[count])) return false;
        bool duplicate = false;
        for (unsigned index = 0; index != count; ++index)
            duplicate |= tables[count].slots == tables[index].slots;
        if (duplicate) continue;
        ++count;
    }
    if (!count) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&mv_capture_start), &module)) return false;
    mv_reset(&probe);
    for (unsigned index = 0; index != count; ++index) {
        if (!exchange_table(index, true)) {
            // Also consider the current table: protection restoration may have
            // failed after its CAS succeeded. All cached wrappers stay pinned.
            for (unsigned undo = 0; undo <= index; ++undo) exchange_table(undo, false);
            return false;
        }
    }
    for (unsigned index = 0; index != count; ++index) {
        for (unsigned slot = 0; slot != 2; ++slot) {
            if (read_slot(&tables[index].slots[slot]) != wrappers[index][slot]) {
                for (unsigned undo = 0; undo != count; ++undo) exchange_table(undo, false);
                return false;
            }
        }
    }
    gate.store(0, std::memory_order_release);
    return true;
}

void mv_capture_stop() {
    attempted = true;
    gate.fetch_or(closed, std::memory_order_acq_rel);
    while (gate.load(std::memory_order_acquire) != closed) Sleep(0);
}

MvProbe *mv_capture_probe() { return &probe; }
