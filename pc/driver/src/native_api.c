#include "native_api.h"

/* Reviewed original's resolved IAT, not new imports or symbol discovery. */
void mv_native_bind(MvNativeApi *a, uintptr_t b) {
    a->create_thread = *(void *(**)(void *, size_t, MvThreadMain, void *, uint32_t, uint32_t *))(b + 0x7D348);
    a->create_event = *(void *(**)(void *, const wchar_t *, uint32_t, uint32_t))(b + 0x7D1F0);
    a->set_event = *(int32_t (**)(void *))(b + 0x7D040);
    a->wait = *(uint32_t (**)(void *, uint32_t))(b + 0x7D118);
    a->close = *(int32_t (**)(void *))(b + 0x7D038);
    a->clock_ms = *(uint64_t (**)(void))(b + 0x7D1B8);
    a->module = *(void *(**)(const wchar_t *))(b + 0x7D0F0);
    a->open_file = *(void *(**)(const wchar_t *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *))(b + 0x7D110);
    a->read_file = *(int32_t (**)(void *, void *, uint32_t, uint32_t *, void *))(b + 0x7D4C0);
    a->last_error = *(uint32_t (**)(void))(b + 0x7D128);
    a->process_id = *(uint32_t (**)(void))(b + 0x7D2B8);
}
