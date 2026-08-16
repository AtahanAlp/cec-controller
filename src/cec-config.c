#include <stddef.h>

#include "cec-config.h"

#if CEC_ENABLE_DDC
static const uint32_t default_edid_delay_ms = 5000;
#else
static const uint32_t default_edid_delay_ms = 0;
#endif

void cec_config_set_default(cec_config_t *config) {
  if (config == NULL) {
    return;
  }

  config->edid_delay_ms = default_edid_delay_ms;
  config->physical_address = 0x0000;
  config->logical_address = 0x0f;
  config->device_type = CEC_CONFIG_DEVICE_TYPE_PLAYBACK;
  config->osd_name[0] = '\0';
}
