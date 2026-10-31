#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cecctl.h"

static bool number(const char *text, unsigned base, uint64_t maximum, uint64_t *value) {
  if (!*text || *text == '-' || *text == '+') return false;
  for (const char *p = text; *p; p++)
    if (!(base == 16 ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p))) return false;
  errno = 0; char *end;
  *value = strtoull(text, &end, (int)base);
  return !errno && !*end && *value <= maximum;
}
static bool copy(char *out, size_t size, const char *text) {
  size_t length = strlen(text);
  if (!length || length >= size) return false;
  memcpy(out, text, length + 1); return true;
}

int cecctl_parse_reply(const char *line, const char *command, cecctl_reply_t *reply) {
  if (!line || !command || !reply || strlen(line) >= 512) return -1;
  while (*line == ' ' || *line == '\t' || *line == '>') line++;
  if (strncmp(line, "CECCTRL/", 8) != 0) return -1;
  if (strncmp(line, "CECCTRL/2 ", 10) != 0) return -2;
  line += 10;
  bool ok;
  if (strncmp(line, "OK ", 3) == 0) { ok = true; line += 3; }
  else if (strncmp(line, "ERR ", 4) == 0) { ok = false; line += 4; }
  else return -1;
  static const char *keys[] = { "command", "session", "id", "code", "power", "value", "verified",
    "source", "routing", "attempts", "elapsed_ms", "phase", "firmware", "on_ms", "standby_ms",
    "status_ms", "gpio", "ready", "busy" };
  enum { COMMAND, SESSION, ID, CODE, POWER, VALUE, VERIFIED, SOURCE, ROUTING, ATTEMPTS,
         ELAPSED, PHASE, FIRMWARE, ON, STANDBY, STATUS, GPIO, READY, BUSY };
  uint32_t seen = 0;
  cecctl_reply_t result = { .ok = ok };
  char fields[512];
  if (!copy(fields, sizeof(fields), line)) return -1;
  char *save;
  for (char *field = strtok_r(fields, " \t\r\n", &save); field; field = strtok_r(NULL, " \t\r\n", &save)) {
    char *value = strchr(field, '=');
    if (!value || value == field || !value[1]) return -1;
    *value++ = 0;
    unsigned key;
    for (key = 0; key < sizeof(keys)/sizeof(keys[0]); key++) if (strcmp(field, keys[key]) == 0) break;
    if (key == sizeof(keys)/sizeof(keys[0])) continue;
    if (seen & (1u << key)) return -1;
    seen |= 1u << key;
    uint64_t n = 0;
    switch (key) {
      case COMMAND: if (!copy(result.command, sizeof(result.command), value)) return -1; break;
      case SESSION:
        if (strlen(value) != 16 || !number(value, 16, UINT64_MAX, &n) || !n) return -1;
        if (!copy(result.session, sizeof(result.session), value)) return -1;
        break;
      case ID: if (strlen(value) != 16 || !number(value, 16, UINT64_MAX, &result.id) || !result.id) return -1; break;
      case CODE: if (!copy(result.code, sizeof(result.code), value)) return -1; break;
      case POWER: if (!copy(result.power, sizeof(result.power), value)) return -1; break;
      case SOURCE: if (!copy(result.source, sizeof(result.source), value)) return -1; break;
      case ROUTING: if (!copy(result.routing, sizeof(result.routing), value)) return -1; break;
      case PHASE: if (!copy(result.phase, sizeof(result.phase), value)) return -1; break;
      case FIRMWARE: if (!copy(result.firmware, sizeof(result.firmware), value)) return -1; break;
      default:
        if (!number(value, 10, UINT32_MAX, &n)) return -1;
        switch (key) {
          case VALUE: result.value = (unsigned)n; break;
          case VERIFIED: result.verified = (unsigned)n; break;
          case ATTEMPTS: result.attempts = (unsigned)n; break;
          case ELAPSED: result.elapsed_ms = (unsigned)n; break;
          case ON: result.on_ms = (unsigned)n; break;
          case STANDBY: result.standby_ms = (unsigned)n; break;
          case STATUS: result.status_ms = (unsigned)n; break;
          case GPIO: result.gpio = (unsigned)n; break;
          case READY: result.ready = (unsigned)n; break;
          case BUSY: result.busy = (unsigned)n; break;
          default: return -1;
        }
    }
  }
  if (!(seen & (1u << COMMAND)) || !(seen & (1u << SESSION)) || strcmp(result.command, command)) return -1;
  if (!strcmp(command, "protocol")) {
    uint32_t required = (1u << FIRMWARE) | (1u << ON) | (1u << STANDBY) | (1u << STATUS)
                      | (1u << GPIO) | (1u << READY) | (1u << BUSY);
    if (!ok || (seen & required) != required || result.on_ms < 1000 || result.on_ms > 30000
        || result.standby_ms < 1000 || result.standby_ms > 10000 || result.status_ms < 1000
        || result.status_ms > 3000 || result.gpio > 29 || result.ready > 1 || result.busy > 1) return -1;
  } else {
    if (!(seen & (1u << ID)) || !(seen & (1u << CODE))) return -1;
    bool power_command = !strcmp(command, "on") || !strcmp(command, "standby") || !strcmp(command, "status");
    if (ok && power_command) {
      uint32_t required = (1u << POWER) | (1u << VALUE) | (1u << VERIFIED) | (1u << SOURCE)
                        | (1u << ROUTING) | (1u << ATTEMPTS) | (1u << ELAPSED) | (1u << PHASE);
      static const char *states[] = { "on", "standby", "transitioning_on", "transitioning_standby" };
      if ((seen & required) != required || strcmp(result.code, "ok") || result.value > 3
          || strcmp(result.power, states[result.value]) || result.verified != 1) return -1;
      if (!strcmp(command, "on") && (result.value != 0 || strcmp(result.source, "announced")
                                    || strcmp(result.routing, "confirmed"))) return -1;
      if (!strcmp(command, "standby") && result.value != 1) return -1;
    }
    if (!ok && !strcmp(result.code, "ok")) return -1;
    if (seen & (1u << POWER)) {
      static const char *states[] = { "on", "standby", "transitioning_on", "transitioning_standby" };
      if (!(seen & (1u << VALUE)) || (result.value > 3 && result.value != 255)
          || strcmp(result.power, result.value == 255 ? "unknown" : states[result.value])) return -1;
    }
    if (!strcmp(result.code, "input_unconfirmed") &&
        (!result.verified || strcmp(result.power, "on") || !(seen & (1u << SOURCE)) || !(seen & (1u << ROUTING)))) return -1;
    if ((seen & (1u << SOURCE)) && strcmp(result.source, "none") && strcmp(result.source, "announced")) return -1;
    if ((seen & (1u << ROUTING)) && strcmp(result.routing, "confirmed") && strcmp(result.routing, "unconfirmed")) return -1;
    if (result.verified > 1 || result.attempts > 3 || result.elapsed_ms > 30050) return -1;
  }
  *reply = result;
  return ok ? 0 : 1;
}
