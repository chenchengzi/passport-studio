// installer/main/installer_flash.h
// ESP-IDF implementations of the installer_core Flash, hash, and image hooks.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "inst_session.h"

const inst_ops_t *installer_flash_ops(void);

// True when the current partition table names an application slot whose
// image passes ESP-IDF verification.
bool installer_flash_app_bootable(void);
