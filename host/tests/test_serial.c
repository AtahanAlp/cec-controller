#define _XOPEN_SOURCE 600

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_line(int fd, char *line, size_t size) {
  size_t used = 0;
  while (used + 1 < size) {
    char byte;
    ssize_t count = read(fd, &byte, 1);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && errno == EIO) {
      usleep(10000);
      continue;
    }
    if (count <= 0) {
      return -1;
    }
    if (byte == '\n') {
      line[used] = '\0';
      return 0;
    }
    if (byte != '\r') {
      line[used++] = byte;
    }
  }
  return -1;
}

static void fake_controller(int master) {
  char line[128];
  if (read_line(master, line, sizeof(line)) != 0 || strcmp(line, "tv protocol") != 0) {
    _exit(10);
  }
  const char protocol[] = "boot noise\r\nCECCTRL/1 OK command=protocol firmware=test\r\n";
  if (write(master, protocol, sizeof(protocol) - 1) != (ssize_t)(sizeof(protocol) - 1)) {
    _exit(11);
  }
  if (read_line(master, line, sizeof(line)) != 0 || strcmp(line, "tv on 1000") != 0) {
    _exit(12);
  }
  const char result[] = "CECCTRL/1 OK command=on attempts=1\r\n";
  if (write(master, result, sizeof(result) - 1) != (ssize_t)(sizeof(result) - 1)) {
    _exit(13);
  }
  _exit(0);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  assert(master >= 0);
  assert(grantpt(master) == 0);
  assert(unlockpt(master) == 0);
  char *slave = ptsname(master);
  assert(slave != NULL);

  pid_t server = fork();
  assert(server >= 0);
  if (server == 0) {
    fake_controller(master);
  }

  pid_t client = fork();
  assert(client >= 0);
  if (client == 0) {
    close(master);
    execl(argv[1], argv[1], "--config", "/nonexistent", "--device", slave,
          "--physical-address", "1.0.0.0", "on", (char *)NULL);
    _exit(20);
  }

  int client_status = 0;
  int server_status = 0;
  assert(waitpid(client, &client_status, 0) == client);
  assert(waitpid(server, &server_status, 0) == server);
  close(master);
  assert(WIFEXITED(client_status) && WEXITSTATUS(client_status) == 0);
  assert(WIFEXITED(server_status) && WEXITSTATUS(server_status) == 0);
  puts("cecctl serial integration passed");
  return 0;
}
