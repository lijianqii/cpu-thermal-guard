#include "util.h"

#include <stdio.h>

int parse_hhmm(const char *s, int *out_min)
{
    int hh, mm;
    if (sscanf(s, "%d:%d", &hh, &mm) != 2)
        return -1;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
        return -1;
    *out_min = hh * 60 + mm;
    return 0;
}
