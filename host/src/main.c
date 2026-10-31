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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
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

#ifndef CECCTL_VERSION
#define CECCTL_VERSION "unknown"
#endif

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
          "\n"
          "Commands:\n"
          "  on                         wake the TV and select the PC input\n"
          "  standby                    place the TV in standby\n"
          "  status                     query the TV power state\n"
          "  protocol                   probe controller compatibility\n"
          "  detect                     resolve the PC input physical address\n"
          "\n"
          "Options:\n"
          "  --device PATH              controller device (default: %s)\n"
          "  --connector NAME           DRM connector basename\n"
          "  --physical-address A.B.C.D override EDID discovery\n"
          "  --config PATH              config file (default: %s)\n"
          "  --state-file PATH          physical-address cache\n"
          "  --sysfs-root PATH          DRM sysfs root\n"
          "  --help                     show this help\n"
          "  --version                  show the client version\n",
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
    errno = EOVERFLOW;
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

static uint64_t display_identity(const options_t *options, const char *connector) {
  char path[PATH_MAX];
  const char *directory = options->connector[0] == '/' ? options->connector : NULL;
  int length = directory ? snprintf(path, sizeof(path), "%s/edid", directory)
                         : snprintf(path, sizeof(path), "%s/%s/edid", options->sysfs_root, connector);
  if (length < 0 || length >= (int)sizeof(path)) return 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
  uint8_t bytes[128]; ssize_t count = read(fd, bytes, sizeof(bytes)); close(fd);
  if (count != sizeof(bytes)) return 0;
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < sizeof(bytes); i++) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
  return hash;
}
static int load_cached_address(const options_t *options, uint16_t *address) {
  char buffer[256], connector[128], identity[17], physical[5], extra;
  size_t length;
  if (read_file(options->state_file, (uint8_t *)buffer, sizeof(buffer)-1, &length)) return -1;
  buffer[length] = 0;
  if (sscanf(buffer, "CECCTL_CACHE/2 address=%4s connector=%127s display=%16s %c",
              physical, connector, identity, &extra) != 3
      || cecctl_parse_physical_address(physical, address)) return -1;
  for (const char *p = connector; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')
          || *p == '-' || *p == '_')) return -1;
  const char *selected = strrchr(options->connector, '/');
  selected = selected ? selected + 1 : options->connector;
  if (*selected && strcmp(selected, connector)) return -1;
  char directory[PATH_MAX]; struct stat info;
  if (snprintf(directory, sizeof(directory), "%s/%s", options->sysfs_root, connector) >= (int)sizeof(directory)
      || stat(options->connector[0] == '/' ? options->connector : directory, &info) || !S_ISDIR(info.st_mode)) return -1;
  if (!*selected) {
    DIR *root = opendir(options->sysfs_root); if (!root) return -1;
    struct dirent *entry; bool other = false;
    while ((entry = readdir(root))) {
      if (!strstr(entry->d_name, "-HDMI-A-") || !strcmp(entry->d_name, connector)) continue;
      if (snprintf(directory, sizeof(directory), "%s/%s", options->sysfs_root, entry->d_name) >= (int)sizeof(directory)) continue;
      if (connector_is_connected(directory)) { other = true; break; }
    }
    closedir(root); if (other) return -1;
  }
  char *end; errno = 0;
  uint64_t saved = strtoull(identity, &end, 16);
  if (strlen(identity) != 16 || *end || errno || !saved) return -1;
  uint64_t current = display_identity(options, connector);
  return current && current != saved ? -1 : 0;
}

static void cache_address(const char *path, uint16_t address, const char *connector, uint64_t identity) {
  char temporary[PATH_MAX];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid())
      >= (int)sizeof(temporary)) {
    return;
  }
  int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    return;
  }
  char line[256];
  int length = snprintf(line, sizeof(line), "CECCTL_CACHE/2 address=%04x connector=%s display=%016llx\n",
                         address, connector, (unsigned long long)identity);
  if (length < 0 || length >= (int)sizeof(line) || !identity) { close(fd); unlink(temporary); return; }
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
      if (!quiet) fprintf(stderr, "cecctl: invalid physical address '%s'\n", options->physical_address);
      errno = EINVAL;
      return -1;
    }
    if (!quiet) {
      char formatted[8];
      cecctl_format_physical_address(*address, formatted);
      printf("physical_address=%s source=config\n", formatted);
    }
    return 0;
  }

  errno = ENODEV;
  char connector[PATH_MAX];
  if (discover_address(options, address, connector, sizeof(connector)) == 0) {
    cache_address(options->state_file, *address, connector, display_identity(options, connector));
    if (!quiet) {
      char formatted[8];
      cecctl_format_physical_address(*address, formatted);
      printf("physical_address=%s source=edid connector=%s\n", formatted, connector);
    }
    return 0;
  }
  int discovery_errno = errno;
  if (discovery_errno != EEXIST && load_cached_address(options, address) == 0) {
    if (!quiet) {
      char formatted[8];
      cecctl_format_physical_address(*address, formatted);
      printf("physical_address=%s source=cache\n", formatted);
    }
    return 0;
  }
  if (quiet) { errno = discovery_errno; return -1; }
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
    perror("cecctl: monotonic clock"); exit(4);
  }
  return ((int64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

static bool serial_exclusive;
static void close_controller(int fd) {
  if (serial_exclusive) ioctl(fd, TIOCNXCL);
  close(fd); serial_exclusive = false;
}
static void pause_ms(unsigned duration) {
  struct timespec delay = { .tv_sec = duration / 1000, .tv_nsec = (long)(duration % 1000) * 1000000L };
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}
static int open_controller(const char *path, int64_t deadline) {
  int saved = ENODEV;
  while (monotonic_ms() < deadline) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { saved = errno; if (saved == EACCES) break; pause_ms(100); continue; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      saved = errno; close(fd); pause_ms(50); continue;
    }
    if (ioctl(fd, TIOCEXCL) != 0) { saved = errno; close(fd); break; }
    serial_exclusive = true;
    struct termios attributes;
    if (tcgetattr(fd, &attributes) != 0) { saved = errno; close_controller(fd); break; }
    cfmakeraw(&attributes);
    cfsetispeed(&attributes, B115200); cfsetospeed(&attributes, B115200);
    attributes.c_cflag |= CLOCAL | CREAD;
    attributes.c_cflag &= ~(HUPCL | CRTSCTS);
    if (tcsetattr(fd, TCSANOW, &attributes) != 0 || tcflush(fd, TCIOFLUSH) != 0) {
      saved = errno; close_controller(fd); break;
    }
    int dtr = TIOCM_DTR;
    if (ioctl(fd, TIOCMBIS, &dtr) != 0 && errno != ENOTTY && errno != EINVAL) {
      saved = errno; close_controller(fd); break;
    }
    return fd;
  }
  errno = saved; return -1;
}
static int write_command(int fd, const char *command, int64_t deadline) {
  size_t length = strlen(command), used = 0;
  while (used < length && monotonic_ms() < deadline) {
    ssize_t count = write(fd, command + used, length - used);
    if (count > 0) { used += (size_t)count; continue; }
    if (count < 0 && errno != EINTR && errno != EAGAIN) return -1;
    struct pollfd descriptor = { .fd = fd, .events = POLLOUT };
    int64_t remaining = deadline - monotonic_ms();
    int result = poll(&descriptor, 1, remaining > 0 ? (int)remaining : 0);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
  }
  if (used == length) return 0;
  errno = ETIMEDOUT; return -1;
}
static int read_reply(int fd, const char *command, int64_t deadline,
                       uint64_t id, const char *session, cecctl_reply_t *reply) {
  char line[512]; size_t used = 0; bool oversized = false;
  while (monotonic_ms() < deadline) {
    int64_t remaining = deadline - monotonic_ms();
    struct pollfd descriptor = { .fd = fd, .events = POLLIN };
    int ready = poll(&descriptor, 1, remaining > 0 ? (int)remaining : 0);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) break;
    if (!(descriptor.revents & POLLIN) && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      errno = ENODEV; return -1;
    }
    char bytes[128]; ssize_t count = read(fd, bytes, sizeof(bytes));
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count <= 0) { errno = ENODEV; return -1; }
    for (ssize_t i = 0; i < count; i++) {
      if (bytes[i] == '\n') {
        if (!oversized) {
          line[used] = 0;
          int parsed = cecctl_parse_reply(line, command, reply);
          if (parsed == -2) { errno = EPROTONOSUPPORT; return -1; }
          if (parsed >= 0 && (!id || reply->id == id)) {
            if (session && strcmp(reply->session, session)) { errno = ECONNRESET; return -1; }
            return parsed;
          }
        }
        used = 0; oversized = false;
      } else if (bytes[i] == 0) {
        oversized = true;
      } else if (bytes[i] != '\r' && !oversized) {
        if (used + 1 < sizeof(line)) line[used++] = bytes[i];
        else oversized = true;
      }
    }
  }
  errno = ETIMEDOUT; return -1;
}
static int transact(int fd, const char *wire, const char *command, int64_t deadline,
                     uint64_t id, const char *session, cecctl_reply_t *reply) {
  if (write_command(fd, wire, deadline) != 0) return -1;
  return read_reply(fd, command, deadline, id, session, reply);
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
    if (!strcmp(reply->command, "protocol")) printf(" gpio=GP%u ready=%u busy=%u", reply->gpio, reply->ready, reply->busy);
    if (reply->id) printf(" id=%016llx verified=%u source=%s routing=%s elapsed_ms=%u", (unsigned long long)reply->id, reply->verified, reply->source, reply->routing, reply->elapsed_ms);
    if (reply->attempts != 0) {
      printf(" attempts=%u", reply->attempts);
    }
    putchar('\n');
  } else {
    fprintf(stderr, "cecctl: command=%s result=%s code=%s id=%016llx power=%s verified=%u source=%s routing=%s attempts=%u elapsed_ms=%u phase=%s\n", reply->command,
            strcmp(reply->code, "input_unconfirmed") == 0 ? "partial" : "error",
            reply->code, (unsigned long long)reply->id, reply->power, reply->verified, reply->source, reply->routing,
            reply->attempts, reply->elapsed_ms, reply->phase);
  }
}

int main(int argc, char **argv) {
  options_t options = {0};
  copy_string(options.config, sizeof(options.config), DEFAULT_CONFIG);
  copy_string(options.device, sizeof(options.device), DEFAULT_DEVICE);
  copy_string(options.state_file, sizeof(options.state_file), DEFAULT_STATE_FILE);
  copy_string(options.sysfs_root, sizeof(options.sysfs_root), DEFAULT_SYSFS_ROOT);

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--version") == 0) {
      printf("cecctl %s\n", CECCTL_VERSION);
      return 0;
    }
  }

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

  int64_t invocation_deadline = monotonic_ms() + (strcmp(command, "on") == 0 ? 40000
                                             : strcmp(command, "standby") == 0 ? 20000 : 12000);
  int64_t startup_deadline = monotonic_ms() + 5000;
  uint16_t address = 0;
  if (strcmp(command, "on") == 0) {
    while (resolve_address(&options, &address, true) != 0) {
      if (errno == EEXIST || errno == EINVAL || monotonic_ms() + 100 >= startup_deadline) {
        resolve_address(&options, &address, false); return 3;
      }
      pause_ms(100);
    }
  }
  int fd = open_controller(options.device, startup_deadline);
  if (fd < 0) {
    fprintf(stderr, "cecctl: cannot own %s: %s\n", options.device, strerror(errno)); return 4;
  }
  cecctl_reply_t probe;
  int64_t probe_deadline = monotonic_ms() + 2000;
  int result = transact(fd, "tv protocol\n", "protocol", probe_deadline, 0, NULL, &probe);
  if (result != 0) {
    fprintf(stderr, "cecctl: protocol probe failed: %s%s\n", strerror(errno),
      errno == EPROTONOSUPPORT ? "; upgrade both firmware and client to protocol 2" : "");
    close_controller(fd); return 4;
  }
  if (strcmp(command, "protocol") == 0) { print_reply(&probe); close_controller(fd); return 0; }
  uint64_t id;
  if (getrandom(&id, sizeof(id), GRND_NONBLOCK) != (ssize_t)sizeof(id)) {
    fprintf(stderr, "cecctl: cannot generate request identity\n"); close_controller(fd); return 4;
  }
  if (!id) id = 1;
  char wire[96];
  if (!strcmp(command, "on")) snprintf(wire, sizeof(wire), "tv on %04x %016llx\n", address, (unsigned long long)id);
  else snprintf(wire, sizeof(wire), "tv %s %016llx\n", command, (unsigned long long)id);
  unsigned operation_ms = !strcmp(command, "on") ? probe.on_ms
                         : !strcmp(command, "standby") ? probe.standby_ms : probe.status_ms;
  cecctl_reply_t reply;
  do {
    if (monotonic_ms() + operation_ms + 500 >= invocation_deadline) {
      fprintf(stderr, "cecctl: controller busy beyond available operation budget\n"); close_controller(fd); return 4;
    }
    int64_t deadline = monotonic_ms() + operation_ms + 500;
    result = transact(fd, wire, command, deadline, id, probe.session, &reply);
    if (result > 0 && !strcmp(reply.code, "busy")) pause_ms(100);
    else break;
  } while (monotonic_ms() < invocation_deadline);
  if (result < 0 && errno == ETIMEDOUT && monotonic_ms() + 1000 < invocation_deadline) {
    char lookup[64];
    snprintf(lookup, sizeof(lookup), "tv result %016llx\n", (unsigned long long)id);
    /* A result lookup cannot repeat a power side effect after an uncertain timeout. */
    result = transact(fd, lookup, command, monotonic_ms() + 1000, id, probe.session, &reply);
  }
  close_controller(fd);
  if (result < 0) {
    fprintf(stderr, "cecctl: %s id=%016llx failed: %s (TV state may be uncertain)\n", command, (unsigned long long)id, strerror(errno)); return 4;
  }
  print_reply(&reply);
  return result == 0 ? 0 : strcmp(reply.code, "input_unconfirmed") == 0 ? 6 : 5;
}
