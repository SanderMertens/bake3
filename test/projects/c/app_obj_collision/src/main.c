#include <stdio.h>

void sub_helper(void);
void sub_other(void);

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    sub_helper();
    sub_other();
    return 0;
}
