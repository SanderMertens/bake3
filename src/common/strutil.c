#include "strutil.h"

#include <ctype.h>

char* bake_ltrim(char *str) {
    while (str && *str && isspace((unsigned char)*str)) {
        str++;
    }
    return str;
}

void bake_rtrim(char *str) {
    if (!str) {
        return;
    }

    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[--len] = '\0';
    }
}
