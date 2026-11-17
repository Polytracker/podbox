/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Other Appearances, opened from the Credits page. Two lists drawn in
 * Scrim's manner -- its top bar, and two-line rows like its 48 px art
 * rows -- over the page's blurred backdrop:
 *   - the people: "Howie Weinberg, 9 albums, 3 tracks" over their role on
 *     this album;
 *   - one person's albums and tracks, by artist and album, each over the
 *     person's role there. Select opens the album in Music, Right its
 *     Credits page.
 *
 * The data is the credits file's "#links" part, read where it lies:
 *   P<TAB>name<TAB>roles here
 *   A<TAB>artist<TAB>album<TAB>role
 *   T<TAB>artist<TAB>album<TAB>track<TAB>role
 *
 * The selected row's first line, when too long, scrolls sideways a few
 * pixels a frame; the second line ends in "...". Only these lists draw
 * like this, so nothing here touches the theme's own lists.
 *
 * Parts, in order: reading the lines, drawing, and appearances_show().
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "string-extra.h"    /* strlcpy */
#include "kernel.h"          /* HZ, current_tick */
#include "lang.h"
#include "font.h"
#include "rbpaths.h"         /* FONT_DIR */
#include "timefuncs.h"       /* get_time */
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "draw/color.h"           /* color_blend */
#include "skin/skin_albumart_color.h"  /* dynamic_colors_roles */
#include "input/action.h"
#include "system/shutdown.h"      /* default_event_handler */
#include "appearances.h"

#define AP_BAR_H     35            /* Scrim's top bar */
#define AP_LIST_X    10            /* Scrim's list viewport */
#define AP_LIST_Y    41
#define AP_LIST_W    300
#define AP_ROW_H     48            /* as Scrim's art rows */
#define AP_ROWS      4
#define AP_PEOPLE    64
#define AP_ENTRIES   256
#define AP_SCROLL_WAIT  (HZ * 3 / 2)  /* before a long line moves */
#define AP_SCROLL_STEP  2             /* pixels a frame */
#define AP_GREY      LCD_RGBPACK(0xbd, 0xbd, 0xbd)  /* Scrim's other rows */

struct ap_field
{
    const char *s;
    int n;
};

struct ap_entry
{
    bool track;
    struct ap_field artist, album, title, role;
};

struct ap_person
{
    struct ap_field name, roles;
    int first, count;          /* into the entries */
    int albums, tracks;
};

static struct
{
    const struct appearances_ctx *c;
    struct ap_person people[AP_PEOPLE];
    struct ap_entry entries[AP_ENTRIES];
    int npeople, nentries;
    int person;                /* -1 in the people list */
    int sel, top, count;
    int offset;                /* the selected first line's scroll */
    long moved;                /* when the selection last changed */
    int icons;                 /* the theme's icon font, or -1 */
    unsigned bar, accent;
} ap;

/* ---- reading the lines ------------------------------------------------ */

/* The tab-separated fields of the line at *p, at most `max`; moves *p to
 * the next line. Returns how many. */
static int ap_split(const char **p, struct ap_field *f, int max)
{
    const char *s = *p;
    int n = 0;

    while (n < max)
    {
        const char *e = s;

        while (*e && *e != '\t' && *e != '\n' && *e != '\r')
            e++;
        f[n].s = s;
        f[n].n = e - s;
        n++;
        if (*e != '\t')
        {
            s = e;
            break;
        }
        s = e + 1;
    }
    while (*s && *s != '\n')
        s++;
    *p = *s ? s + 1 : s;
    return n;
}

static int ap_cmp(const struct ap_field *a, const struct ap_field *b)
{
    int n = MIN(a->n, b->n), r = strncasecmp(a->s, b->s, n);

    return r ? r : a->n - b->n;
}

/* Before b in a person's list: by artist, then album, then track title,
 * an album before its own tracks */
static bool ap_before(const struct ap_entry *a, const struct ap_entry *b)
{
    int r = ap_cmp(&a->artist, &b->artist);

    if (!r)
        r = ap_cmp(&a->album, &b->album);
    if (!r)
        r = a->track - b->track;
    if (!r)
        r = ap_cmp(&a->title, &b->title);
    return r < 0;
}

static void ap_read(char *links)
{
    const char *p = links;
    struct ap_field f[5];
    struct ap_person *who = NULL;

    ap.npeople = ap.nentries = 0;
    while (*p)
    {
        int n = ap_split(&p, f, 5);

        if (n >= 2 && f[0].n == 1 && f[0].s[0] == 'P'
            && ap.npeople < AP_PEOPLE)
        {
            who = &ap.people[ap.npeople++];
            who->name = f[1];
            who->roles = n > 2 ? f[2] : (struct ap_field){ "", 0 };
            who->first = ap.nentries;
            who->count = who->albums = who->tracks = 0;
        }
        else if (who && n >= 3 && f[0].n == 1
                 && (f[0].s[0] == 'A' || f[0].s[0] == 'T')
                 && ap.nentries < AP_ENTRIES)
        {
            struct ap_entry *e = &ap.entries[ap.nentries];
            bool track = f[0].s[0] == 'T';
            int i;

            e->track = track;
            e->artist = f[1];
            e->album = f[2];
            e->title = track && n > 3 ? f[3] : (struct ap_field){ "", 0 };
            e->role = n > (track ? 4 : 3) ? f[track ? 4 : 3]
                                          : (struct ap_field){ "", 0 };
            /* kept in order as it goes in: a person has a few dozen */
            for (i = ap.nentries; i > who->first
                 && ap_before(e, &ap.entries[i - 1]); i--)
                ;
            if (i < ap.nentries)
            {
                struct ap_entry t = *e;

                memmove(&ap.entries[i + 1], &ap.entries[i],
                        (ap.nentries - i) * sizeof *e);
                ap.entries[i] = t;
            }
            ap.nentries++;
            who->count++;
            if (track)
                who->tracks++;
            else
                who->albums++;
        }
    }
}

/* ---- drawing ---------------------------------------------------------- */

static void ap_copy(char *buf, int size, const struct ap_field *f)
{
    int n = MIN(f->n, size - 1);

    memcpy(buf, f->s, n);
    buf[n] = '\0';
}

static int ap_width(const char *s, int font)
{
    int w = 0;

    font_getstringsize((const unsigned char *)s, &w, NULL, font);
    return w;
}

/* `s` shortened to `width` in `font`, ending in "..." when cut */
static void ap_cut(char *s, int width, int font)
{
    int n = strlen(s);

    if (ap_width(s, font) <= width)
        return;
    while (n > 0)
    {
        do
            n--;
        while (n > 0 && (s[n] & 0xc0) == 0x80);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == ','))
            n--;
        strcpy(s + n, "\xe2\x80\xa6");
        if (ap_width(s, font) <= width)
            return;
    }
}

/* "9 albums, 3 tracks" */
static void ap_counts(char *buf, int size, const struct ap_person *who)
{
    char a[32] = "", t[32] = "";

    if (who->albums)
        snprintf(a, sizeof a, "%d %s", who->albums,
                 str(who->albums == 1 ? LANG_LINER_NOTES_ALBUM
                                      : LANG_LINER_NOTES_ALBUMS));
    if (who->tracks)
        snprintf(t, sizeof t, "%d %s", who->tracks,
                 str(who->tracks == 1 ? LANG_LINER_NOTES_TRACK_ONE
                                      : LANG_LINER_NOTES_TRACKS));
    snprintf(buf, size, "%s%s%s", a, a[0] && t[0] ? ", " : "", t);
}

/* Row `i`'s two lines, and whether the first starts with a quote that
 * hangs in the margin */
static bool ap_row(int i, char *one, char *two, int size, int *icon)
{
    char a[MAX_PATH], b[MAX_PATH], r[MAX_PATH];

    if (ap.person < 0)
    {
        const struct ap_person *who = &ap.people[i];

        ap_copy(a, sizeof a, &who->name);
        ap_counts(b, sizeof b, who);
        snprintf(one, size, "%s, %s", a, b);
        ap_copy(two, size, &who->roles);
        *icon = 0;
        return false;
    }
    else
    {
        const struct ap_entry *e =
            &ap.entries[ap.people[ap.person].first + i];

        ap_copy(a, sizeof a, &e->artist);
        ap_copy(r, sizeof r, &e->role);
        if (e->track)
        {
            ap_copy(b, sizeof b, &e->title);
            snprintf(one, size, "\xe2\x80\x98%s\xe2\x80\x99, %s", b, a);
            ap_copy(b, sizeof b, &e->album);
            snprintf(two, size, r[0] ? "%s, %s %s" : "%.0s%s %s", r,
                     str(LANG_LINER_NOTES_ON_ALBUM), b);
            *icon = 'a';               /* a note */
            return true;
        }
        ap_copy(b, sizeof b, &e->album);
        snprintf(one, size, "%s, %s", b, a);
        strlcpy(two, r, size);
        *icon = 'd';                   /* a disc, as Scrim's Album rows */
        return false;
    }
}

/* Text in a box: what passes its edges is not drawn */
static void ap_text_in(struct screen *d, struct viewport *vp, int x, int y,
                       int w, int font, unsigned colour, int dx,
                       const char *s)
{
    struct viewport box = *vp;
    struct viewport *last;

    box.x = vp->x + x;
    box.y = vp->y + y;
    box.width = MIN(w, vp->width - x);
    box.height = font_get(font)->height;
    box.font = font;
    last = d->set_viewport(&box);
    d->set_drawmode(DRMODE_FG);
    d->set_foreground(colour);
    d->putsxy(-dx, 0, (const unsigned char *)s);
    d->set_drawmode(DRMODE_SOLID);
    d->set_viewport(last);
}

/* Whether rows start with an icon: a person's own list, when they are on.
 * The text then starts where it does in Scrim's icon rows. */
static bool ap_icons(void)
{
    return ap.person >= 0 && ap.icons >= 0
           && global_settings.liner_notes_icons;
}

/* How far the selected row's first line runs past its box, or 0 */
static int ap_overflow(void)
{
    char one[MAX_PATH * 2], two[MAX_PATH * 2];
    int icon, ix = ap_icons() ? 46 : 12;
    int w;

    ap_row(ap.sel, one, two, sizeof one, &icon);
    w = ap_width(one, ap.c->body_font) - (AP_LIST_W - ix);
    return w > 0 ? w : 0;
}

static void ap_draw(struct screen *d, struct viewport *vp)
{
    const struct appearances_ctx *c = ap.c;
    char one[MAX_PATH * 2], two[MAX_PATH * 2], clock[16] = "";
    bool icons = ap_icons();
    int ix = icons ? 46 : 12, body_h = font_get(c->body_font)->height;
    unsigned grey = c->backdrop ? AP_GREY : color_blend(c->bg, c->fg, 170);
    unsigned white = c->backdrop ? LCD_WHITE : c->fg;
    struct tm *tm = get_time();
    const char *title;
    char name[MAX_PATH];
    int i, w;

    d->set_viewport(vp);
    d->set_background(c->bg);
    if (c->backdrop)
        d->bitmap_part(c->backdrop, 0, 0,
                       STRIDE(SCREEN_MAIN, vp->width, 2 * vp->height),
                       0, 0, vp->width, vp->height);
    else
        d->clear_viewport();

    /* the top bar: the title, and the time where Scrim has it */
    d->set_foreground(ap.bar);
    d->fillrect(0, 0, vp->width, AP_BAR_H);
    if (ap.person < 0)
        title = str(LANG_OTHER_APPEARANCES);
    else
    {
        ap_copy(name, sizeof name, &ap.people[ap.person].name);
        title = name;
    }
    if (tm)
    {
        if (global_settings.timeformat)
            snprintf(clock, sizeof clock, "%d:%02d %s",
                     tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min,
                     tm->tm_hour < 12 ? "AM" : "PM");
        else
            snprintf(clock, sizeof clock, "%02d:%02d", tm->tm_hour,
                     tm->tm_min);
    }
    w = ap_width(clock, c->body_font);
    ap_text_in(d, vp, 10, 8, 270 - w - 10, c->head_font, LCD_BLACK, 0,
               title);
    ap_text_in(d, vp, 280 - w, 8, w, c->body_font, LCD_BLACK, 0, clock);

    for (i = ap.top; i < ap.count && i < ap.top + AP_ROWS; i++)
    {
        int y = AP_LIST_Y + (i - ap.top) * AP_ROW_H, icon, hang = 0;
        bool on = i == ap.sel;
        unsigned ink = on ? white : grey;

        if (ap_row(i, one, two, sizeof one, &icon))
            hang = ap_width("\xe2\x80\x98", c->body_font);
        if (icons && icon)
        {
            char glyph[2] = { (char)icon, '\0' };

            ap_text_in(d, vp, AP_LIST_X + 2, y + 12, 24, ap.icons, ink, 0,
                       glyph);
        }
        if (on)
        {
            d->set_foreground(ap.accent);
            d->fillrect(AP_LIST_X + ix - 10, y + 1, 3, AP_ROW_H - 2);
        }
        ap_text_in(d, vp, AP_LIST_X + ix - hang, y + 6,
                   AP_LIST_W - ix + hang, c->body_font, ink,
                   on ? ap.offset : 0, one);
        ap_cut(two, AP_LIST_W - ix, c->small_font);
        ap_text_in(d, vp, AP_LIST_X + ix, y + 6 + body_h + 3,
                   AP_LIST_W - ix, c->small_font, ink, 0, two);
    }
    d->update_viewport();
}

/* ---- the lists -------------------------------------------------------- */

static void ap_select(int sel)
{
    ap.sel = MAX(0, MIN(sel, ap.count - 1));
    if (ap.sel < ap.top)
        ap.top = ap.sel;
    else if (ap.sel >= ap.top + AP_ROWS)
        ap.top = ap.sel - AP_ROWS + 1;
    ap.offset = 0;
    ap.moved = current_tick;
}

static void ap_enter(int person, int sel)
{
    ap.person = person;
    ap.count = person < 0 ? ap.npeople : ap.people[person].count;
    ap.top = 0;
    ap_select(sel);
}

/* Right or Select, from the button behind ACTION_STD_OK */
static bool ap_right(void)
{
    int btn = 0;

    get_action_statuscode(&btn);
#ifdef BUTTON_RC_RIGHT
    if (btn & BUTTON_RC_RIGHT)
        btn |= BUTTON_RIGHT;
#endif
    return btn & BUTTON_RIGHT;
}

static unsigned ap_average_top(const struct appearances_ctx *c, int width)
{
    unsigned r = 0, g = 0, b = 0, n = 0;
    int x, y;

    if (!c->backdrop)
        return c->bg;
    for (y = 0; y < AP_BAR_H; y += 4)
        for (x = 0; x < width; x += 8, n++)
        {
            unsigned px = FB_UNPACK_SCALAR_LCD(c->backdrop[y * width + x]);

            r += RGB_UNPACK_RED(px);
            g += RGB_UNPACK_GREEN(px);
            b += RGB_UNPACK_BLUE(px);
        }
    return LCD_RGBPACK(r / n, g / n, b / n);
}

enum appearances_exit appearances_show(const struct appearances_ctx *c,
                                       char *artist_out, char *album_out,
                                       int size)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport *vp = c->vp;
    struct viewport *last = d->set_viewport(vp);
    enum appearances_exit ret = APPEARANCES_BACK;
    unsigned dominant, vivid;

    ap.c = c;
    ap_read(c->links);
    if (!ap.npeople)
        return APPEARANCES_BACK;
    ap.icons = font_load(FONT_DIR "/24x24-icons.fnt");
    /* Scrim's bar: white at 12/15 over the picture */
    ap.bar = color_blend(ap_average_top(c, vp->width), LCD_WHITE, 205);
    ap.accent = dynamic_colors_roles(&dominant, &vivid) ? vivid : LCD_WHITE;
    ap_enter(-1, 0);
    ap_draw(d, vp);

    while (1)
    {
        int over = ap_overflow(), timeout = HZ;
        int action;

        if (over && TIME_AFTER(current_tick, ap.moved + AP_SCROLL_WAIT))
        {
            /* along to the end, a wait there, then back to the start */
            ap.offset += AP_SCROLL_STEP;
            if (ap.offset > over + AP_SCROLL_STEP * HZ)
            {
                ap.offset = 0;
                ap.moved = current_tick;
            }
            ap_draw(d, vp);
            timeout = HZ / 25;
        }
        else if (over)
            timeout = HZ / 10;

        action = get_action(CONTEXT_STD, timeout);
        switch (action)
        {
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                ap_select(ap.sel + 1);
                ap_draw(d, vp);
                break;

            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                ap_select(ap.sel - 1);
                ap_draw(d, vp);
                break;

            case ACTION_STD_OK:
                if (ap.person < 0)
                {
                    if (!ap_right())
                    {
                        ap_enter(ap.sel, 0);
                        ap_draw(d, vp);
                    }
                }
                else
                {
                    const struct ap_entry *e =
                        &ap.entries[ap.people[ap.person].first + ap.sel];

                    ap_copy(artist_out, size, &e->artist);
                    ap_copy(album_out, size, &e->album);
                    ret = ap_right() ? APPEARANCES_CREDITS
                                     : APPEARANCES_ALBUM;
                    goto done;
                }
                break;

            case ACTION_STD_CANCEL:         /* Left and Menu: back a list */
            case ACTION_STD_MENU:
                if (ap.person < 0)
                    goto done;
                ap_enter(-1, ap.person);
                ap_draw(d, vp);
                break;

            case ACTION_NONE:
                break;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = APPEARANCES_USB;
                    goto done;
                }
                break;
        }
    }

  done:
    if (ap.icons >= 0)
        font_unload(ap.icons);
    d->set_viewport(last);
    return ret;
}
