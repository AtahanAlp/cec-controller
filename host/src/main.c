#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cecctl.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_CONFIG "/etc/cec-controller.conf"
#define DEFAULT_DEVICE "/dev/cec-controller"
#define DEFAULT_STATE_FILE "/var/lib/cec-controller/physical-address"
#define DEFAULT_SYSFS_ROOT "/sys/class/drm"
#define MAX_EDID_SIZE (128U * 256U)

typedef struct {
  char config[PATH_MAX];
  char device[PATH_MAX];
  char connector[PATH_MAX];
  char state_file[PATH_MAX];
  char sysfs_root[PATH_MAX];
  char physical_address[16];
} options_t;

static void usage(FILE *stream) {
  fprintf(stream,
          "Usage: cecctl [options] {on|standby|status|protocol|detect}\n"
          "  --device PATH              controller device (default: %s)\n"
          "  --connector NAME           DRM connector basename\n"
          "  --physical-address A.B.C.D override EDID discovery\n"
          "  --config PATH              config file (default: %s)\n",
          DEFAULT_DEVICE,
          DEFAULT_CONFIG);
}

static void copy_string(char *destination, size_t size, const char *source) {
  if (snprintf(destination, size, "%s", source) >= (int)size) {
    fprintf(stderr, "cecctl: value is too long: %s\n", source);
    exit(2);
  }
}

static char *trim(char *text) {
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  char *end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'
                        || end[-1] == '\n')) {
    *--end = '\0';
  }
  return text;
}

static int load_config(options_t *options) {
  FILE *file = fopen(options->config, "r");
  if (file == NULL) {
    return errno == ENOENT ? 0 : -1;
  }

  char line[PATH_MAX + 64];
  unsigned int line_number = 0;
  while (fgets(line, sizeof(line), file) != NULL) {
    line_number++;
    char *entry = trim(line);
    if (*entry == '\0' || *entry == '#') {
      continue;
    }
    char *equals = strchr(entry, '=');
    if (equals == NULL) {
      fprintf(stderr, "cecctl: %s:%u: expected key=value\n", options->config, line_number);
      fclose(file);
      return -1;
    }
    *equals = '\0';
    char *key = trim(entry);
    char *value = trim(equals + 1);
    if (strcmp(key, "device") == 0) {
      copy_string(options->device, sizeof(options->device), value);
    } else if (strcmp(key, "connector") == 0) {
      copy_string(options->connector, sizeof(options->connector), value);
    } else if (strcmp(key, "physical_address") == 0) {
      copy_string(options->physical_address, sizeof(options->physical_address), value);
    } else if (strcmp(key, "state_file") == 0) {
      copy_string(options->state_file, sizeof(options->state_file), value);
    } else {
      fprintf(stderr, "cecctl: %s:%u: unknown key '%s'\n", options->config, line_number, key);
      fclose(file);
      return -1;
    }
  }
  int result = ferror(file) ? -1 : 0;
  fclose(file);
  return result;
}

static int read_file(const char *path, uint8_t *buffer, size_t capacity, size_t *length) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  size_t used = 0;
  while (used < capacity) {
    ssize_t count = read(fd, buffer + used, capacity - used);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      int saved_errno = errno;
      close(fd);
      errno = saved_errno;
      return -1;
    }
    if (count == 0) {
      break;
    }
    used += (size_t)count;
  }
  int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  if (used == capacity) {
    return -1;
  }
  *length = used;
  return 0;
}

static bool connector_is_connected(const char *directory) {
  char path[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/status", directory) >= (int)sizeof(path)) {
    return false;
  }
  char status[32];
  size_t length = 0;
  if (read_file(path, (uint8_t *)status, sizeof(status) - 1, &length) != 0) {
    return false;
  }
  status[length] = '\0';
  return strncmp(status, "connected", 9) == 0;
}

static int address_from_connector(const char *directory, uint16_t *address) {
  char path[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/edid", directory) >= (int)sizeof(path)) {
    return -1;
  }
  uint8_t *edid = malloc(MAX_EDID_SIZE);
  if (edid == NULL) {
    return -1;
  }
  size_t length = 0;
  int result = read_file(path, edid, MAX_EDID_SIZE, &length);
  if (result == 0) {
    result = cecctl_edid_physical_address(edid, length, address);
  }
  free(edid);
  return result;
}

static int discover_address(const options_t *options, uint16_t *address, char *connector_name,
                            size_t connector_name_size) {
  if (options->connector[0] != '\0') {
    const char *basename = strrchr(options->connector, '/');
    basename = basename == NULL ? options->connector : basename + 1;
    char directory[PATH_MAX];
    if (options->connector[0] == '/') {
      copy_string(directory, sizeof(directory), options->connector);
    } else if (snprintf(directory, sizeof(directory), "%s/%s", options->sysfs_root,
                        options->connector) >= (int)sizeof(directory)) {
      return -1;
    }
    if (address_from_connector(directory, address) != 0) {
      return -1;
    }
    copy_string(connector_name, connector_name_size, basename);
    return 0;
  }

  DIR *directory = opendir(options->sysfs_root);
  if (directory == NULL) {
    return -1;
  }
  unsigned int matches = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strstr(entry->d_name, "-HDMI-A-") == NULL) {
      continue;
    }
    char connector[PATH_MAX];
    if (snprintf(connector, sizeof(connector), "%s/%s", options->sysfs_root, entry->d_name)
        >= (int)sizeof(connector) || !connector_is_connected(connector)) {
      continue;
    }
    uint16_t candidate = 0;
    if (address_from_connector(connector, &candidate) == 0) {
      matches++;
      *address = candidate;
      copy_string(connector_name, connector_name_size, entry->d_name);
    }
  }
  closedir(directory);
  if (matches > 1) {
    errno = EEXIST;
    return -1;
  }
  if (matches == 0) {
    errno = ENODEV;
    return -1;
  }
  return 0;
}

static int load_cached_address(const char *path, uint16_t *address) {
  char buffer[32];
  size_t length = 0;
  if (read_file(path, (uint8_t *)buffer, sizeof(buffer) - 1, &length) != 0) {
    return -1;
  }
  buffer[length] = '\0';
  return cecctl_parse_physical_address(trim(buffer), address);
}

static void cache_address(const char *path, uint16_t address) {
  char temporary[PATH_MAX];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid())
      >= (int)sizeof(temporary)) {
    return;
  }
  int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    return;
  }
  char formatted[8];
  cecctl_format_physical_address(address, formatted);
  char line[10];
  int length = snprintf(line, sizeof(line), "%s\n", formatted);
  bool success = write(fd, line, (size_t)length) == length && fsync(fd) == 0;
  int saved_errno = errno;
  close(fd);
  if (!success || rename(temporary, path) != 0) {
    unlink(temporary);
  }
  errno = saved_errno;
}

static int resolve_address(const options_t *options, uint16_t *address, bool quiet) {
  if (options->physical_address[0] != '\0') {
    if (cecctl_parse_physical_address(options->physical_address, address) != 0) {
      fprintf(stderr, "cecctl: invalid physical address '%s'\n", options->physical_address);
      return -1;
    }
    if (!quiet) {
      char formatted[8];
      cecctl_format_physical_address(*address, formatted);
      printf("physical_address=%s source=config\n", formatted);
    }
    return 0;
  }

  char connector[PATH_MAX];
  if (discover_address(options, address, connector, sizeof(connector)) == 0) {
    cache_address(options->state_file, *address);
    if (!quiet) {
      char formatted[8];
      cecctl_format_physical_address(*address, formatted);
      printf("physical_address=%s source=edid connector=%s\n", formatted, connector);
    }
    return 0;
  }
  int discovery_errno = errno;
  if (load_cached_address(options->state_file, address) == 0) {
    if (!quiet) {
      char formatted[8];
      cecctl_format_physical_address(*address, formatted);
      printf("physical_address=%s source=cache\n", formatted);
    }
    return 0;
  }
  if (discovery_errno == EEXIST) {
    fprintf(stderr, "cecctl: multiple HDMI connectors expose CEC addresses; set connector=\n");
  } else {
    fprintf(stderr, "cecctl: no usable HDMI EDID and no cached address; set physical_address=\n");
  }
  return -1;
}

static int64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return ((int64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

static int open_controller(const char *path, unsigned int wait_ms) {
  int64_t deadline = monotonic_ms() + wait_ms;
  do {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
      struct termios attributes;
      if (tcgetattr(fd, &attributes) == 0) {
        cfmakeraw(&attributes);
        cfsetispeed(&attributes, B115200);
        cfsetospeed(&attributes, B115200);
        attributes.c_cflag |= CLOCAL | CREAD;
        tcsetattr(fd, TCSANOW, &attributes);
      }
      tcflush(fd, TCIOFLUSH);
      return fd;
    }
    struct timespec delay = {.tv_nsec = 100000000L};
    nanosleep(&delay, NULL);
  } while (monotonic_ms() < deadline);
  return -1;
}

static int write_command(int fd, const char *command) {
  size_t length = strlen(command);
  size_t written = 0;
  while (written < length) {
    ssize_t count = write(fd, command + written, length - written);
    if (count > 0) {
      written += (size_t)count;
      continue;
    }
    if (count < 0 && errno != EINTR && errno != EAGAIN) {
      return -1;
    }
    struct pollfd poll_fd = {.fd = fd, .events = POLLOUT};
    if (poll(&poll_fd, 1, 500) <= 0) {
      return -1;
    }
  }
  return 0;
}

static int read_reply(int fd, const char *expected_command, unsigned int timeout_ms,
                      cecctl_reply_t *reply) {
  char line[512];
  size_t used = 0;
  int64_t deadline = monotonic_ms() + timeout_ms;
  while (monotonic_ms() < deadline) {
    int remaining = (int)(deadline - monotonic_ms());
    struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
    int ready = poll(&poll_fd, 1, remaining > 0 ? remaining : 0);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0 || (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      break;
    }
    char buffer[128];
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    for (ssize_t i = 0; i < count; i++) {
      if (buffer[i] == '\n') {
        line[used] = '\0';
        int parsed = cecctl_parse_reply(line, expected_command, reply);
        if (parsed >= 0) {
          return parsed;
        }
        used = 0;
      } else if (buffer[i] != '\r') {
        if (used + 1 < sizeof(line)) {
          line[used++] = buffer[i];
        } else {
          used = 0;
        }
      }
    }
  }
  errno = ETIMEDOUT;
  return -1;
}

static int transact(int fd, const char *wire_command, const char *expected_command,
                    unsigned int timeout_ms, cecctl_reply_t *reply) {
  if (write_command(fd, wire_command) != 0) {
    return -1;
  }
  return read_reply(fd, expected_command, timeout_ms, reply);
}

static void print_reply(const cecctl_reply_t *reply) {
  if (reply->ok) {
    printf("command=%s result=ok", reply->command);
    if (reply->power[0] != '\0') {
      printf(" power=%s", reply->power);
    }
    if (reply->firmware[0] != '\0') {
      printf(" firmware=%s", reply->firmware);
    }
    if (reply->attempts != 0) {
      printf(" attempts=%u", reply->attempts);
    }
    putchar('\n');
  } else {
    fprintf(stderr, "cecctl: command=%s result=error code=%s attempts=%u\n", reply->command,
            reply->code[0] == '\0' ? "unknown" : reply->code, reply->attempts);
  }
}

int main(int argc, char **argv) {
  options_t options = {0};
  copy_string(options.config, sizeof(options.config), DEFAULT_CONFIG);
  copy_string(options.device, sizeof(options.device), DEFAULT_DEVICE);
  copy_string(options.state_file, sizeof(options.state_file), DEFAULT_STATE_FILE);
  copy_string(options.sysfs_root, sizeof(options.sysfs_root), DEFAULT_SYSFS_ROOT);

  for (int i = 1; i + 1 < argc; i++) {
    if (strcmp(argv[i], "--config") == 0) {
      copy_string(options.config, sizeof(options.config), argv[i + 1]);
    }
  }
  if (load_config(&options) != 0) {
    perror("cecctl: cannot read config");
    return 3;
  }

  const char *command = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (argv[i][0] != '-') {
      if (command != NULL) {
        usage(stderr);
        return 2;
      }
      command = argv[i];
      continue;
    }
    if (i + 1 >= argc) {
      usage(stderr);
      return 2;
    }
    const char *value = argv[++i];
    if (strcmp(argv[i - 1], "--config") == 0) {
      continue;
    } else if (strcmp(argv[i - 1], "--device") == 0) {
      copy_string(options.device, sizeof(options.device), value);
    } else if (strcmp(argv[i - 1], "--connector") == 0) {
      copy_string(options.connector, sizeof(options.connector), value);
    } else if (strcmp(argv[i - 1], "--physical-address") == 0) {
      copy_string(options.physical_address, sizeof(options.physical_address), value);
    } else if (strcmp(argv[i - 1], "--sysfs-root") == 0) {
      copy_string(options.sysfs_root, sizeof(options.sysfs_root), value);
    } else if (strcmp(argv[i - 1], "--state-file") == 0) {
      copy_string(options.state_file, sizeof(options.state_file), value);
    } else {
      usage(stderr);
      return 2;
    }
  }

  if (command == NULL) {
    usage(stderr);
    return 2;
  }
  if (strcmp(command, "detect") == 0) {
    uint16_t address = 0;
    return resolve_address(&options, &address, false) == 0 ? 0 : 3;
  }
  if (strcmp(command, "on") != 0 && strcmp(command, "standby") != 0
      && strcmp(command, "status") != 0 && strcmp(command, "protocol") != 0) {
    usage(stderr);
    return 2;
  }

  uint16_t address = 0;
  if (strcmp(command, "on") == 0 && resolve_address(&options, &address, false) != 0) {
    return 3;
  }

  int fd = open_controller(options.device, 5000);
  if (fd < 0) {
    fprintf(stderr, "cecctl: cannot open %s: %s\n", options.device, strerror(errno));
    return 4;
  }
  cecctl_reply_t reply;
  int result = transact(fd, "tv protocol\n", "protocol", 2500, &reply);
  if (result != 0) {
    fprintf(stderr, "cecctl: controller protocol probe failed: %s\n",
            result > 0 ? reply.code : strerror(errno));
    close(fd);
    return result > 0 ? 5 : 4;
  }
  if (strcmp(command, "protocol") == 0) {
    print_reply(&reply);
    close(fd);
    return 0;
  }

  char wire_command[64];
  unsigned int timeout_ms = 5000;
  if (strcmp(command, "on") == 0) {
    snprintf(wire_command, sizeof(wire_command), "tv on %04x\n", address);
    timeout_ms = 10000;
  } else {
    snprintf(wire_command, sizeof(wire_command), "tv %s\n", command);
  }
  result = transact(fd, wire_command, command, timeout_ms, &reply);
  close(fd);
  if (result < 0) {
    fprintf(stderr, "cecctl: %s command timed out or disconnected\n", command);
    return 4;
  }
  print_reply(&reply);
  return result == 0 ? 0 : 5;
}
