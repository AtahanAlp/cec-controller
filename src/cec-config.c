#include <stddef.h>
#include <string.h>
#include <ctype.h>

#include "cec-config.h"
#include "cec-validate.h"

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

bool cec_config_valid(const cec_config_t *config) {
  if (!config || config->edid_delay_ms > 30000 || config->logical_address > 15
      || config->device_type == CEC_CONFIG_DEVICE_TYPE_RESERVED
      || config->device_type == CEC_CONFIG_DEVICE_TYPE_TV
      || config->device_type > CEC_CONFIG_DEVICE_TYPE_AUDIO_SYSTEM
      || (config->physical_address && !cec_physical_address_valid(config->physical_address))
      || !memchr(config->osd_name, 0, sizeof(config->osd_name))) return false;
  for (const char *p = config->osd_name; *p; p++) if (!isprint((unsigned char)*p)) return false;
  return true;
}
