#ifndef VDHS_MODULE_CONFIG_H
#define VDHS_MODULE_CONFIG_H

#include "../../mod/native/vdhs_config.h"

static inline int vdhs_shell_requested(int mode) {
    return mode == 2;
}

#endif
