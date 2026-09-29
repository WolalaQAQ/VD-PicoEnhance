// VD compat layer (vdhs_layer.c) <-> vdhs_mark.c / vdhs_payload.c.
#pragma once

// vdhs_layer.c
void vdhs_layer_start(void);   // spawn the hook installer (idempotent)
int  vdhs_layer_active(void);  // 1 once all loader hooks are installed
int  vdhs_layer_marked(void);  // 1 once xrMarkApiClass succeeded from a hook

// vdhs_hand.c
#include <stdint.h>
int vdhs_hand_mesh_ready(void);   // 1 when hand_mesh_fb.bin next to libvdhs.so is valid
int vdhs_hand_gipa_self(const char* name, void (**fn)(void));   // 1 = served by the layer
int vdhs_hand_gipa_wrap(const char* name, void (**fn)(void));   // 1 = downstream wrapped
int vdhs_hand_inject_managed(const int32_t* values, int count); // add to XR._supportedExtensions
int vdhs_hand_fix_permissions(void); // DynamicSettings.FaceTrackingPermission -> PICO eye perm; 0 ok
int vdhs_hand_fix_inputtab(void);    // InputTab._eyeTrackingPermission -> PICO eye perm; 1 changed
void vdhs_hand_capture_settings(void); // remember saved PreferEyeGazeInput before the UI clears it
float vdhs_hand_cfg(const char* key);  // hand_gesture.txt value (pt_style, pt_bg_fix, gaze_log, pt_hole, ...)
// 1 when VD itself would show hand passthrough in SteamVR (cached ~1 s; must
// be called on a Mono-attached thread). `chains` receives VD's VR stream
// swapchain handles (_vrChains / _vrExtrapolatedChains).
int vdhs_hand_pt_gate(uint64_t* chains, int max, int* nchains, int hint_on);

// Latest valid locate snapshot (stage/VD CurrentSpace) for the zero-copy alpha
// hole. Returns the number of joints copied; 0 when there is no recent,
// non-frozen, active locate for that hand (hand: 1 = left, 2 = right).
typedef struct { float x, y, z, radius; uint64_t flags; } vdhs_pt_joint;
int vdhs_pt_hand_snapshot(int hand, vdhs_pt_joint* out, int max);

// vdhs_pt.c
int  vdhs_pt_install(uintptr_t loader_base);        // hook the 4 swapchain loader exports
void vdhs_pt_on_end_frame(const void* frame_end_info); // record VR views, OR layer flags
uint64_t vdhs_pt_current_space(void);   // space of VD's last projection layer (= Session.CurrentSpace), 0 if none

// vdhs_mark.c
int   vdhs_mark_now(void);      // negotiate + xrMarkApiClass; 0 ok, -1 retryable
void* vdhs_runtime_gipa(void);  // runtime xrGetInstanceProcAddr after negotiate, or NULL
void  vdhs_runtime_reset(void); // drop the cached runtime GIPA (runtime was reloaded)
