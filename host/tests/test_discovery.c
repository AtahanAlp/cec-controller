#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void checksum(uint8_t *block) {
  unsigned int sum = 0;
  for (size_t i = 0; i < 127; i++) {
    sum += block[i];
  }
  block[127] = (uint8_t)(0U - sum);
}

static void write_all(int fd, const void *buffer, size_t length) {
  const uint8_t *bytes = buffer;
  size_t written = 0;
  while (written < length) {
    ssize_t count = write(fd, bytes + written, length - written);
    assert(count > 0);
    written += (size_t)count;
  }
}

static void create_fixture(const char *directory) {
  char connector[PATH_MAX];
  char path[PATH_MAX];
  assert(snprintf(connector, sizeof(connector), "%s/card0-HDMI-A-1", directory)
         < (int)sizeof(connector));
  assert(mkdir(connector, 0700) == 0);

  assert(snprintf(path, sizeof(path), "%s/status", connector) < (int)sizeof(path));
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(fd >= 0);
  write_all(fd, "connected\n", 10);
  assert(close(fd) == 0);

  uint8_t edid[256] = {0};
  static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  memcpy(edid, header, sizeof(header));
  edid[126] = 1;
  checksum(edid);
  uint8_t *cta = edid + 128;
  cta[0] = 0x02;
  cta[1] = 0x03;
  cta[2] = 10;
  cta[4] = (3U << 5) | 5U;
  cta[5] = 0x03;
  cta[6] = 0x0c;
  cta[7] = 0x00;
  cta[8] = 0x20;
  cta[9] = 0x00;
  checksum(cta);

  assert(snprintf(path, sizeof(path), "%s/edid", connector) < (int)sizeof(path));
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(fd >= 0);
  write_all(fd, edid, sizeof(edid));
  assert(close(fd) == 0);
}

static void run_detect(const char *program, const char *directory, const char *state_file,
                       const char *expected) {
  int output[2];
  assert(pipe(output) == 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(output[0]);
    assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output[1]);
    execl(program, program, "--config", "/nonexistent", "--sysfs-root", directory,
          "--state-file", state_file, "detect", (char *)NULL);
    _exit(20);
  }
  close(output[1]);
  char buffer[512];
  ssize_t length = read(output[0], buffer, sizeof(buffer) - 1);
  assert(length >= 0);
  buffer[length] = '\0';
  close(output[0]);
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(strstr(buffer, expected) != NULL);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  char directory[] = "/tmp/cecctl-discovery-XXXXXX";
  assert(mkdtemp(directory) != NULL);
  create_fixture(directory);

  char state_file[PATH_MAX];
  assert(snprintf(state_file, sizeof(state_file), "%s/cache", directory)
         < (int)sizeof(state_file));
  run_detect(argv[1], directory, state_file,
             "physical_address=2.0.0.0 source=edid connector=card0-HDMI-A-1");

  char path[PATH_MAX];
  assert(snprintf(path, sizeof(path), "%s/card0-HDMI-A-1/status", directory)
         < (int)sizeof(path));
  assert(unlink(path) == 0);
  run_detect(argv[1], directory, state_file, "physical_address=2.0.0.0 source=cache");

  assert(snprintf(path, sizeof(path), "%s/card0-HDMI-A-1/edid", directory)
         < (int)sizeof(path));
  assert(unlink(path) == 0);
  assert(snprintf(path, sizeof(path), "%s/card0-HDMI-A-1", directory) < (int)sizeof(path));
  assert(rmdir(path) == 0);
  assert(unlink(state_file) == 0);
  assert(rmdir(directory) == 0);
  puts("cecctl discovery and cache integration passed");
  return 0;
}
