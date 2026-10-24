#pragma once
#include <jni.h>

// Check module inputs before connecting; capture consumes the companion socket.
bool vd_capability_prepare(int module_dir);
void vd_capability_capture(int module_dir, int companion);
void vd_capability_companion(int client);
bool vd_capability_start(JNIEnv* env);
