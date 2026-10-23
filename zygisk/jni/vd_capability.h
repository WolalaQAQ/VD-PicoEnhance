#pragma once
#include <jni.h>

// Capture privileged inputs before specialization; bootstrap only after boot.
void vd_capability_prepare(int module_dir);
bool vd_capability_start(JNIEnv* env);
