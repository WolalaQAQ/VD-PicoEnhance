#pragma once
#include <jni.h>

// Module directory access occurs before specialization; no server-main wait.
void vd_capability_prepare(int module_dir);
void vd_capability_start(JNIEnv* env);
