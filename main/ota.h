#pragma once
#include <stdbool.h>

// Starts the background updater: it checks OTA_MANIFEST_URL now and then and installs a newer
// build into the spare app slot, then reboots into it. A bad new build rolls back by itself
// (see ota_confirm_boot).
void ota_start(void);

// True while an update is downloading/installing; percent is 0..100 (or -1 when unknown).
bool ota_active(void);
int ota_percent(void);

// Call once the new firmware has proven it runs: cancels the automatic rollback.
void ota_confirm_boot(void);
