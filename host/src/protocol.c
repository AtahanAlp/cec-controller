#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cecctl.h"

static void copy_field(char *destination, size_t size, const char *value) {
  if (size == 0) {
    return;
  }
  snprintf(destination, size, "%s", value);
}

int cecctl_parse_reply(const char *line, const char *expected_command, cecctl_reply_t *reply) {
  if (line == NULL || expected_command == NULL || reply == NULL) {
    return -1;
  }

  const char *prefix = strstr(line, "CECCTRL/");
  if (prefix == NULL) {
    return -1;
  }

  unsigned int version = 0;
  char result[4] = {0};
  int consumed = 0;
  if (sscanf(prefix, "CECCTRL/%u %3s %n", &version, result, &consumed) != 2
      || version != CECCTL_PROTOCOL_VERSION
      || (strcmp(result, "OK") != 0 && strcmp(result, "ERR") != 0)) {
    return -1;
  }

  cecctl_reply_t parsed = {.ok = strcmp(result, "OK") == 0};
  char fields[256];
  snprintf(fields, sizeof(fields), "%s", prefix + consumed);
  char *save = NULL;
  for (char *field = strtok_r(fields, " \t\r\n", &save); field != NULL;
       field = strtok_r(NULL, " \t\r\n", &save)) {
    char *equals = strchr(field, '=');
    if (equals == NULL || equals == field || equals[1] == '\0') {
      continue;
    }
    *equals = '\0';
    const char *value = equals + 1;
    if (strcmp(field, "command") == 0) {
      copy_field(parsed.command, sizeof(parsed.command), value);
    } else if (strcmp(field, "code") == 0) {
      copy_field(parsed.code, sizeof(parsed.code), value);
    } else if (strcmp(field, "power") == 0) {
      copy_field(parsed.power, sizeof(parsed.power), value);
    } else if (strcmp(field, "firmware") == 0) {
      copy_field(parsed.firmware, sizeof(parsed.firmware), value);
    } else if (strcmp(field, "attempts") == 0) {
      errno = 0;
      char *end = NULL;
      unsigned long attempts = strtoul(value, &end, 10);
      if (errno == 0 && end != value && *end == '\0' && attempts <= UINT32_MAX) {
        parsed.attempts = (unsigned int)attempts;
      }
    }
  }

  if (strcmp(parsed.command, expected_command) != 0) {
    return -1;
  }
  *reply = parsed;
  return parsed.ok ? 0 : 1;
}
