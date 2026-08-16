#include <stdio.h>
#include <string.h>

#include <hardware/flash.h>
#include <hardware/sync.h>

#include "crc/crc32.h"

#include "cec-config.h"
#include "nvs.h"

typedef struct __attribute__((packed)) {
  uint8_t version;
  uint32_t length;
} cec_config_header_nvs_t;

typedef struct __attribute__((packed)) {
  uint32_t edid_delay_ms;
  uint16_t physical_address;
  uint8_t logical_address;
  uint8_t device_type;
  char osd_name[CEC_OSD_NAME_MAX_LEN + 1];
} cec_config_nvs_t;

typedef struct __attribute__((aligned(FLASH_PAGE_SIZE))) {
  cec_config_header_nvs_t header;
  uint32_t header_crc;
  cec_config_nvs_t config;
  uint32_t config_crc;
} pico_cec_nvs_t;

extern uint32_t CEC_NVS_BASE_ADDR[];
extern uint32_t __CEC_NVS_LEN[];

#define CEC_NVS_LEN ((uint32_t)(&__CEC_NVS_LEN))

static const uint8_t CEC_CONFIG_VERSION = 0x04;

static uint32_t nvs_get_flash_address(void) {
  return ((uint32_t)CEC_NVS_BASE_ADDR - XIP_BASE);
}

static bool load_config(const pico_cec_nvs_t *nvs, cec_config_t *config) {
  if (nvs->header.length != sizeof(nvs->config)
      || crc32((unsigned char *)&nvs->config, sizeof(nvs->config)) != nvs->config_crc) {
    return false;
  }

  config->edid_delay_ms = nvs->config.edid_delay_ms;
  config->physical_address = nvs->config.physical_address;
  config->logical_address = nvs->config.logical_address;
  config->device_type = nvs->config.device_type;
  if (config->device_type == CEC_CONFIG_DEVICE_TYPE_TV) {
    config->device_type = CEC_CONFIG_DEVICE_TYPE_PLAYBACK;
  }
  memcpy(config->osd_name, nvs->config.osd_name, sizeof(nvs->config.osd_name));
  return true;
}

bool nvs_read_config(cec_config_t *config) {
  const pico_cec_nvs_t *nvs = (const pico_cec_nvs_t *)(CEC_NVS_BASE_ADDR);
  cec_config_set_default(config);

  if (crc32((unsigned char *)&nvs->header, sizeof(nvs->header)) != nvs->header_crc
      || nvs->header.version != CEC_CONFIG_VERSION) {
    return false;
  }
  return load_config(nvs, config);
}

void nvs_load_config(cec_config_t *config) {
  nvs_read_config(config);

  if (config->osd_name[0] == '\0') {
    strncpy(config->osd_name, CEC_OSD_NAME, CEC_OSD_NAME_MAX_LEN);
    config->osd_name[CEC_OSD_NAME_MAX_LEN] = '\0';
  }
}

bool nvs_save_config(const cec_config_t *config) {
  pico_cec_nvs_t nvs = {0};

  if (sizeof(nvs) > CEC_NVS_LEN) {
    return false;
  }

  nvs.header.version = CEC_CONFIG_VERSION;
  nvs.header.length = sizeof(nvs.config);
  nvs.header_crc = crc32((unsigned char *)&nvs.header, sizeof(nvs.header));

  nvs.config.edid_delay_ms = config->edid_delay_ms;
  nvs.config.physical_address = config->physical_address;
  nvs.config.logical_address = config->logical_address;
  nvs.config.device_type = config->device_type;
  memcpy(nvs.config.osd_name, config->osd_name, sizeof(nvs.config.osd_name));
  nvs.config_crc = crc32((unsigned char *)&nvs.config, sizeof(nvs.config));

  unsigned int sectors = sizeof(nvs) / FLASH_SECTOR_SIZE;
  unsigned int erase_size = sizeof(nvs) % FLASH_SECTOR_SIZE == 0
                                ? sectors * FLASH_SECTOR_SIZE
                                : (sectors + 1) * FLASH_SECTOR_SIZE;

  uint32_t irqs = save_and_disable_interrupts();
  flash_range_erase(nvs_get_flash_address(), erase_size);
  flash_range_program(nvs_get_flash_address(), (uint8_t *)&nvs, sizeof(nvs));
  restore_interrupts(irqs);
  return true;
}
