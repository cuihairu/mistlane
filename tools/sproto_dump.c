/*
 * sproto_dump.c — decode a compiled sproto .spb schema into a text inventory.
 *
 * Used to recover the game-fogs business protocol contract from the committed
 * binary schema (game-fogs/game/proto/sproto.spb); the original .sproto text
 * source, generation script and client bindings are absent. See
 * docs/protocol-contract-recovery.md.
 *
 * Build (against the vendored sproto core that produced the file):
 *   gcc -O2 -I<path>/3rd/sproto -o sproto_dump sproto_dump.c <path>/3rd/sproto/sproto.c -lm
 * Run:
 *   ./sproto_dump game/proto/sproto.spb
 *
 * Output is the reference decoder's own debug dump: '=== N types ===' with one
 * 'Name.request'/'Name.response' (or bare 'Name') header per type followed by
 * '\tname (tag) type' field lines, then '=== N protocol ===' with one
 * '\tName (id) request:Type response:Type' line per RPC.
 */
#include <stdio.h>
#include <stdlib.h>
#include "sproto.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.spb\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 1; }
    fclose(f);
    struct sproto *s = sproto_create(buf, (size_t)sz);
    if (!s) { fprintf(stderr, "sproto_create failed\n"); return 1; }
    sproto_dump(s);
    sproto_release(s);
    return 0;
}
