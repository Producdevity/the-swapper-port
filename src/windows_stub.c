#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int MoveFileEx(const char *existing_name, const char *new_name, uint32_t flags) {
    (void)flags;

    if (existing_name == NULL)
        return 0;

    if (new_name == NULL)
        return unlink(existing_name) == 0;

    return rename(existing_name, new_name) == 0;
}

int DwmEnableComposition(uint32_t action) {
    (void)action;
    return 0;
}
