#ifndef CECCTL_H
#define CECCTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CECCTL_PROTOCOL_VERSION 1
#define CECCTL_REPLY_FIELD_LEN 64

typedef struct {
  bool ok;
  char command[CECCTL_REPLY_FIELD_LEN];
  char code[CECCTL_REPLY_FIELD_LEN];
  char power[CECCTL_REPLY_FIELD_LEN];
  char firmware[CECCTL_REPLY_FIELD_LEN];
  unsigned int attempts;
} cecctl_reply_t;

bool cecctl_valid_physical_address(uint16_t address);
int cecctl_parse_physical_address(const char *text, uint16_t *address);
void cecctl_format_physical_address(uint16_t address, char output[8]);
int cecctl_edid_physical_address(const uint8_t *edid, size_t length, uint16_t *address);
int cecctl_parse_reply(const char *line, const char *expected_command, cecctl_reply_t *reply);

#endif
