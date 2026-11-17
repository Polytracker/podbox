/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Other Appearances: the people on an album who appear elsewhere in the
 * library, and for each the albums and tracks they are on. Opened from the
 * Credits page with Select.
 ****************************************************************************/

#ifndef _APPEARANCES_H
#define _APPEARANCES_H

#include <stdbool.h>
#include "lcd.h"

/* What the lists are drawn over and with, from the Credits page */
struct appearances_ctx
{
    struct viewport *vp;       /* the screen's, full size */
    char *links;               /* the credits file's lines after "#links" */
    const fb_data *backdrop;   /* the page's blurred backdrop, or NULL */
    unsigned fg, bg;           /* the page's colours, for no backdrop */
    int body_font, small_font, head_font;
};

/* How the lists were left */
enum appearances_exit
{
    APPEARANCES_BACK,          /* back to the Credits page */
    APPEARANCES_ALBUM,         /* open artist_out's album_out in Music */
    APPEARANCES_CREDITS,       /* that album's Credits page */
    APPEARANCES_USB,           /* USB was plugged in */
};

/* Shows the people list, then a person's own. For _ALBUM and _CREDITS the
 * album's artist and title are written to the two buffers. */
enum appearances_exit appearances_show(const struct appearances_ctx *c,
                                       char *artist_out, char *album_out,
                                       int size);

#endif /* _APPEARANCES_H */
