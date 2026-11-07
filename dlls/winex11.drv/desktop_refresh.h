/* Recover the desktop mode when Xwayland's synthetic dot clock is clamped.
 *
 * The RandR wire clock is only 32 bits. At 7680x2160 and 240 Hz its generated
 * blanking timings exceed that limit and advertise about 158 Hz instead.
 * The launcher supplies the current compositor mode; other modes and ordinary
 * clocks keep their normal RandR frequency.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <stdlib.h>

static unsigned int desktop_refresh( unsigned int width, unsigned int height, unsigned long clock )
{
    const char *setting, *next;
    char *end;
    unsigned long value;
    double rate;

    if (clock != 0xffffffffUL || !(setting = getenv( "FERESTRE_DESKTOP_MODE" ))) return 0;
    value = strtoul( setting, &end, 10 );
    if (end == setting || *end != 'x' || value != width) return 0;
    next = end + 1;
    value = strtoul( next, &end, 10 );
    if (end == next || *end != '@' || value != height) return 0;
    next = end + 1;
    rate = strtod( next, &end );
    if (end == next || *end || !(rate >= 1.0 && rate <= 1000.0)) return 0;
    return (unsigned int)(rate + 0.5);
}
