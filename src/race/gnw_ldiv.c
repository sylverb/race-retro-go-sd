/* Freestanding ldiv for RACE tlcs900h (firmware NGP linked libc; we don't). */
#include <stdlib.h>

ldiv_t ldiv(long numer, long denom)
{
    ldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}
