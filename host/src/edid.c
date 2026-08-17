#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cecctl.h"

#define EDID_BLOCK_SIZE 128U
#define EDID_EXTENSION_COUNT 126U
#define CTA_EXTENSION_TAG 0x02U
#define CTA_DATA_START 4U
#define CTA_VENDOR_BLOCK 0x03U

bool cecctl_valid_physical_address(uint16_t address) {
  if (address == 0) {
    return false;
  }

  bool zero_seen = false;
  for (int shift = 12; shift >= 0; shift -= 4) {
    uint8_t nibble = (uint8_t)((address >> shift) & 0x0f);
    if (nibble == 0) {
      zero_seen = true;
    } else if (zero_seen) {
      return false;
    }
  }
  return true;
}

int cecctl_parse_physical_address(const char *text, uint16_t *address) {
  if (text == NULL || address == NULL) {
    return -1;
  }

  if (strlen(text) == 7 && text[1] == '.' && text[3] == '.' && text[5] == '.') {
    uint16_t value = 0;
    for (size_t i = 0; i < 7; i += 2) {
      if (!isxdigit((unsigned char)text[i])) {
        return -1;
      }
      char digit[2] = {text[i], '\0'};
      value = (uint16_t)((value << 4) | strtoul(digit, NULL, 16));
    }
    if (!cecctl_valid_physical_address(value)) {
      return -1;
    }
    *address = value;
    return 0;
  }

  if (strlen(text) != 4) {
    return -1;
  }
  for (size_t i = 0; i < 4; i++) {
    if (!isxdigit((unsigned char)text[i])) {
      return -1;
    }
  }
  errno = 0;
  unsigned long value = strtoul(text, NULL, 16);
  if (errno != 0 || value > UINT16_MAX || !cecctl_valid_physical_address((uint16_t)value)) {
    return -1;
  }
  *address = (uint16_t)value;
  return 0;
}

void cecctl_format_physical_address(uint16_t address, char output[8]) {
  static const char digits[] = "0123456789abcdef";
  output[0] = digits[(address >> 12) & 0x0f];
  output[1] = '.';
  output[2] = digits[(address >> 8) & 0x0f];
  output[3] = '.';
  output[4] = digits[(address >> 4) & 0x0f];
  output[5] = '.';
  output[6] = digits[address & 0x0f];
  output[7] = '\0';
}

static bool checksum_valid(const uint8_t *block) {
  uint8_t sum = 0;
  for (size_t i = 0; i < EDID_BLOCK_SIZE; i++) {
    sum = (uint8_t)(sum + block[i]);
  }
  return sum == 0;
}

int cecctl_edid_physical_address(const uint8_t *edid, size_t length, uint16_t *address) {
  static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  if (edid == NULL || address == NULL || length < EDID_BLOCK_SIZE
      || memcmp(edid, header, sizeof(header)) != 0 || !checksum_valid(edid)) {
    return -1;
  }

  size_t available_extensions = (length / EDID_BLOCK_SIZE) - 1;
  size_t extension_count = edid[EDID_EXTENSION_COUNT];
  if (extension_count > available_extensions) {
    extension_count = available_extensions;
  }

  for (size_t extension = 0; extension < extension_count; extension++) {
    const uint8_t *block = edid + ((extension + 1) * EDID_BLOCK_SIZE);
    if (block[0] != CTA_EXTENSION_TAG || !checksum_valid(block)) {
      continue;
    }

    size_t data_end = block[2] == 0 ? 127U : block[2];
    if (data_end < CTA_DATA_START || data_end > 127U) {
      continue;
    }

    for (size_t offset = CTA_DATA_START; offset < data_end;) {
      uint8_t block_header = block[offset++];
      size_t payload_length = block_header & 0x1fU;
      uint8_t tag = block_header >> 5;
      if (offset + payload_length > data_end) {
        break;
      }
      if (tag == CTA_VENDOR_BLOCK && payload_length >= 5
          && block[offset] == 0x03 && block[offset + 1] == 0x0c
          && block[offset + 2] == 0x00) {
        uint16_t value = (uint16_t)((block[offset + 3] << 8) | block[offset + 4]);
        if (cecctl_valid_physical_address(value)) {
          *address = value;
          return 0;
        }
      }
      offset += payload_length;
    }
  }
  return -1;
}
