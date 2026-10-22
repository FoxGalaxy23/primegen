/* Проверка Public Suffix List: psl_cli host...  — печатает «host: suffix=0|1 registrable=...»
   Сборка: gcc -O2 -Imods/nal/common tools/psl_cli.c mods/nal/common/psl.c -o /tmp/psl_cli */
#include "psl.h"
#include <stdio.h>
int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) { char r[256]; int ok = psl_registrable_domain(argv[i], r, sizeof r); printf("%s: suffix=%d registrable=%s\n", argv[i], psl_is_public_suffix(argv[i]), ok ? r : "-"); }
    return 0;
}
