#ifndef GATEWAY_IP_H
#define GATEWAY_IP_H

#include <stdio.h>
#include <string.h>

/* One IPv4 address in <directory of worker_path>/gateway-ip.txt.
 * The diagnostic gateway is not the same address on every car, so a missing
 * or invalid file leaves dest empty and the caller sends nothing. */
static int load_gateway_ip(const char *worker_path, char *dest, size_t dest_len) {
    char path[192];
    const char *slash;
    size_t dirlen;
    FILE *file;
    char line[64];
    unsigned a, b, c, d;
    int used = 0;
    if (!worker_path || !dest || dest_len < 8) return 0;
    dest[0] = 0;
    slash = strrchr(worker_path, '/');
    if (!slash) return 0;
    dirlen = (size_t)(slash - worker_path);
    if (dirlen + sizeof "/gateway-ip.txt" > sizeof path) return 0;
    memcpy(path, worker_path, dirlen);
    memcpy(path + dirlen, "/gateway-ip.txt", sizeof "/gateway-ip.txt");
    file = fopen(path, "r");
    if (!file) return 0;
    if (!fgets(line, (int)sizeof line, file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    if (sscanf(line, "%u.%u.%u.%u%n", &a, &b, &c, &d, &used) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    while (line[used] == ' ' || line[used] == '\t' || line[used] == '\r' || line[used] == '\n') ++used;
    if (line[used] != 0) return 0;
    if (snprintf(dest, dest_len, "%u.%u.%u.%u", a, b, c, d) < 7) {
        dest[0] = 0;
        return 0;
    }
    return 1;
}

#endif
