#ifndef MV_STANDALONE_INPUT_CAPTURE_H
#define MV_STANDALONE_INPUT_CAPTURE_H

#include "input_probe.h"
namespace vr { class IVRDriverContext; }

// Provider lifecycle calls are serialized, outside DllMain/input callbacks.
// One attempt per process/module instance; false means do not start the worker.
// Must precede the target's fresh CreateBooleanComponent calls.
bool mv_capture_start(vr::IVRDriverContext *context);

// Permanently disable recording and drain admitted callbacks before SDK teardown.
// Successful hooks remain pinned, forwarding originals until process exit.
void mv_capture_stop();

// Persistent storage. Do not reset/free it while callbacks or the worker run.
MvProbe *mv_capture_probe();

#endif
