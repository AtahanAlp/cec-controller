#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tclie.h"
#include "usb-cdc.h"

#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      exit(1);                                                                        \
    }                                                                                 \
  } while (0)

typedef struct {
  int calls;
  int argc;
  char command[24];
  char address[8];
} command_capture_t;

static void discard_output(void *arg, const char *str) {
  (void)arg;
  (void)str;
}

static int capture_tv_command(void *arg, int argc, const char **argv) {
  command_capture_t *capture = arg;
  capture->calls++;
  capture->argc = argc;
  snprintf(capture->command, sizeof(capture->command), "%s", argv[1]);
  if (argc == 3) {
    snprintf(capture->address, sizeof(capture->address), "%s", argv[2]);
  }
  return 0;
}

static command_capture_t run_command(const char *input) {
  command_capture_t capture = {0};
  tclie_t cli;
  const tclie_cmd_t command = {
      .name = "tv",
      .fn = capture_tv_command,
      .desc = "Control or query the TV.",
      .pattern = CEC_TV_COMMAND_PATTERN,
  };

  tclie_init(&cli, discard_output, &capture);
  CHECK(tclie_reg_cmds(&cli, &command, 1));
  tclie_in_str(&cli, input);
  return capture;
}

static void check_command(const char *input,
                          int expected_argc,
                          const char *expected_command,
                          const char *expected_address) {
  command_capture_t capture = run_command(input);
  CHECK(capture.calls == 1);
  CHECK(capture.argc == expected_argc);
  CHECK(strcmp(capture.command, expected_command) == 0);
  CHECK(strcmp(capture.address, expected_address) == 0);
}

int main(void) {
  check_command("tv protocol\r", 2, "protocol", "");
  check_command("tv standby\r", 2, "standby", "");
  check_command("tv status\r", 2, "status", "");
  check_command("tv on\r", 2, "on", "");
  check_command("tv on 1200\r", 3, "on", "1200");

  CHECK(run_command("tv standby 1200\r").calls == 0);
  CHECK(run_command("tv protocol extra\r").calls == 0);
  CHECK(run_command("tv unknown\r").calls == 0);

  puts("tv CLI tests passed");
  return 0;
}
