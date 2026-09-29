#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C"
{
#endif

    int bus_init();

    int bus_deinit();

    int bus_put_message(char *buffer, int length);

    int bus_get_message(char **buffer, int *length);

    unsigned int bus_get_empty_count();

    unsigned int bus_get_full_count();

#ifdef __cplusplus
}
#endif

#endif