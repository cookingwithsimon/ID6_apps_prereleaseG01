#include "gateway_ip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int expect(const char *input, const char *want) {
    char dir[64];
    char worker[96];
    char file[96];
    char got[16];
    FILE *out;
    int ok;
    snprintf(dir, sizeof dir, "build/gw-%d", getpid());
    if (system("mkdir -p build") != 0) return 0;
    snprintf(worker, sizeof worker, "%s/worker", dir);
    snprintf(file, sizeof file, "%s/gateway-ip.txt", dir);
    /* A fresh directory per case keeps a rejected line from leaking. */
    {
        char cmd[192];
        snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
        if (system(cmd) != 0) return 0;
    }
    if (input) {
        out = fopen(file, "w");
        if (!out) return 0;
        fputs(input, out);
        fclose(out);
    }
    ok = load_gateway_ip(worker, got, sizeof got);
    if (!want) return !ok && got[0] == 0;
    return ok && strcmp(got, want) == 0;
}

int main(void) {
    int failed = 0;
    if (!expect("169.254.10.3\n", "169.254.10.3")) failed++;
    if (!expect("  10.0.0.1\r\n", "10.0.0.1")) failed++;
    if (!expect("169.254.21.3 extra\n", 0)) failed++;
    if (!expect("169.254.21\n", 0)) failed++;
    if (!expect("999.1.2.3\n", 0)) failed++;
    if (!expect(0, 0)) failed++;
    if (failed) {
        fprintf(stderr, "gateway-ip checks failed: %d\n", failed);
        return 1;
    }
    puts("gateway-ip ok");
    return 0;
}
