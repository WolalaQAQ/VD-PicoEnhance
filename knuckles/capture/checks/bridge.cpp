#include <openvr_driver.h>
#include "input_capture.h"

extern "C" {
__declspec(dllexport) bool test_start(vr::IVRDriverContext *context) {
    return mv_capture_start(context);
}
__declspec(dllexport) void test_stop() { mv_capture_stop(); }
__declspec(dllexport) MvProbe *test_probe() { return mv_capture_probe(); }
// An owned cross-image hook used only to verify conflict preservation.
__declspec(dllexport) vr::EVRInputError test_external_update(void *, uint64_t, bool, double) {
    return vr::VRInputError_InvalidParam;
}
}
