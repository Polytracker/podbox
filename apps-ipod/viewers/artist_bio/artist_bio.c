/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Liner Notes pages: Bio, Album, Track and Credits, about what is
 * playing, scrolled with the wheel like a web page. Left and Right go from
 * page to page; a row of the pages' names stays at the top.
 *
 * How it is built:
 *   - A plain core screen, entered as artist_bio() or liner_notes() and
 *     returning a GO_TO_* code. Reached from the WPS context menu and
 *     hotkeys, and as artist_bio_of() and liner_notes_of() from the menu of
 *     an artist or album in a Music list.
 *   - Each page is a file the PC tool writes: /Artwork/Bios/<artist>.txt,
 *     /Artwork/Notes/<artist> - <album>.txt, /Artwork/Tracks/<artist> -
 *     <title>.txt and /Artwork/Credits/<artist> - <album>.txt. Bio and Album
 *     have a 320 x 160 banner .jpg beside them; Track and Credits are drawn
 *     over the album's banner, blurred. A file is the artist's when its name
 *     matches the album artist or the artist the way /Artwork/People matches,
 *     so case, accents and a leading "The" do not matter.
 *   - Credits rows are "Name<TAB>Role, Role [2, 5]": the bracketed track
 *     numbers are drawn small and grey, joined to the word before them.
 *   - It owns the whole screen with the theme off, on one solid colour, so the
 *     banner's foot can fade into it and the name sit over the fade.
 *   - The text is wrapped once, on opening, into a table of lines at fixed
 *     page positions; a frame draws the lines that fall on screen.
 *   - Both buffers are immovable and freed while the settings menu runs, then
 *     the page is read again, since a setting may change any of it.
 *
 * Parts, in order: the pages, finding the file, colours and fonts, reading
 * the page, drawing, scrolling, and the entry points.
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "string-extra.h"    /* strlcpy */
#include "config.h"
#include "file.h"            /* open, ffilesize, MAX_PATH */
#include "dir.h"             /* opendir */
#include "kernel.h"          /* HZ, current_tick */
#include "lang.h"
#include "font.h"
#include "rbpaths.h"         /* FONT_DIR */
#include "core_alloc.h"      /* core_alloc_ex, buflib_ops_locked */
#include "audio.h"           /* audio_current_track */
#include "metadata.h"
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"        /* viewportmanager_theme_enable/undo */
#include "draw/color.h"           /* color_blend, color_contrast */
#include "draw/text_wrap.h"
#include "draw/image_load.h"
#include "draw/img_filter.h"
#include "skin/skin_albumart_color.h"  /* dynamic_colors_resolve */
#include "metadata/art_name.h"
#include "metadata/tag_trim.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/shutdown.h"      /* default_event_handler */
#include "root_menu.h"            /* GO_TO_*, MENU_ATTACHED_USB */
#include "widgets/menu.h"         /* do_menu */
#include "screens/settings/exported_settings.h"  /* artist_bio_menu */
#include "artist_bio.h"

#define AB_MARGIN     10
#define AB_TEXT_MAX   (24 * 1024)  /* about six times the longest seen */
#define AB_LINES_MAX  2048         /* a credits row takes a few, per line */
#define AB_BANNER_MAX 240          /* tallest banner drawn, in pixels */
#define AB_FADE       64           /* rows at the banner's foot that fade */
#define AB_STEP       8            /* pixels per wheel click, at rest */
#define AB_ACCEL_MAX  6            /* each adds half a step */
#define AB_GLIDE_MAX  4            /* most pixels the page moves a frame */
/* How far the target may run ahead of the page. The page stops within this
 * many pixels of the wheel stopping, so it follows the finger. */
#define AB_LEAD_MAX   32
#define AB_BAR_W      3
#define AB_STRIP      20           /* the row of page names at the top */
#define AB_STRIP_GAP  16           /* between two page names */
#define AB_SHADE      32           /* banner rows darkened under the names */
#define AB_ROW_GAP    4            /* between two people on Credits */

/* Blurred Art: Scrim's Now Playing backdrop. `scrim` brings the picture's
 * highlights down under white text, as much as this picture needs; the
 * darker one also takes a light picture down to a dark one. */
#define AB_BLUR_CHAIN        "blur12+scrim:ffffff+dither"
#define AB_BLUR_CHAIN_DARKER "blur12+darker+scrim:ffffff+dither"

static bool ab_blurred(void)
{
    return global_settings.artist_bio_background == ARTIST_BIO_BG_BLURRED
        || global_settings.artist_bio_background == ARTIST_BIO_BG_BLURRED_DARK;
}

enum ab_font { AB_FONT_NAME, AB_FONT_SUB, AB_FONT_BODY, AB_FONT_HEAD, AB_FONTS };

/* Scrim's fonts, by file; one that is not there is the UI font */
static const char *const ab_font_files[AB_FONTS] = {
    FONT_DIR "/26-noto-sans-medium.fnt",
    FONT_DIR "/14-noto-sans.fnt",
    NULL,
    FONT_DIR "/18-noto-sans-bold.fnt",
};

enum ab_page { AB_BIO, AB_ALBUM, AB_TRACK, AB_CREDITS, AB_PAGES };

/* What a line is drawn in: the text colour, the grey of the quiet lines, or
 * the softer text colour of a credit's roles */
enum ab_ink { AB_INK_FG, AB_INK_DIM, AB_INK_SOFT };

struct ab_line
{
    const char *text;          /* into the text buffer, not terminated */
    int y;                     /* top, from the top of the page */
    short x;                   /* from the margin */
    unsigned short len;
    unsigned char font;        /* enum ab_font */
    unsigned char ink;         /* enum ab_ink */
};

static struct
{
    struct viewport vp;
    struct viewport tvp;       /* the text's, under the page names */
    unsigned fg, bg, dim, soft;
    int font[AB_FONTS];        /* font ids drawn with */
    int loaded[AB_FONTS];      /* font_load() handles, -1 for none */
    int text_handle;           /* the text, then the line table */
    int banner_handle;
    struct ab_line *lines;
    int nlines;
    struct bitmap banner;      /* .data NULL with no banner */
    int height;                /* of the whole page */
    int pos, target;           /* scroll offset, and where it is heading */
    bool found;
    int page;                  /* enum ab_page */
    int backdrop_handle;       /* Blurred Art: the screen behind the text */
    fb_data *backdrop;         /* NULL without one */
    char names[2][MAX_PATH];   /* album artist, artist */
    char album[MAX_PATH];      /* for Album and Credits */
    char title[MAX_PATH];      /* for Track */
    char stem[MAX_PATH - 24];  /* the file's name, without ".txt" */
} ab;

static const char *const ab_dirs[AB_PAGES] = {
    ARTIST_BIO_DIR, LINER_NOTES_DIR, TRACK_NOTES_DIR, CREDITS_DIR
};

static const char *ab_dir(void)
{
    return ab_dirs[ab.page];
}

/* ---- the pages -------------------------------------------------------- */

/* Whether a page is switched on and has something to look for */
static bool ab_page_ok(int page)
{
    const struct user_settings *gs = &global_settings;

    switch (page)
    {
        case AB_BIO:
            return gs->liner_notes_bio && (ab.names[0][0] || ab.names[1][0]);
        case AB_ALBUM:
            return gs->liner_notes_album && ab.album[0];
        case AB_TRACK:
            return gs->liner_notes_track && ab.title[0];
        case AB_CREDITS:
            return gs->liner_notes_credits && ab.album[0];
    }
    return false;
}

/* The next page shown after `page` in direction dir, or -1 */
static int ab_page_step(int page, int dir)
{
    for (page += dir; page >= 0 && page < AB_PAGES; page += dir)
        if (ab_page_ok(page))
            return page;
    return -1;
}

/* The second half of a file's name: the album, or the title for Track */
static const char *ab_second(void)
{
    return ab.page == AB_BIO ? NULL : ab.page == AB_TRACK ? ab.title
                                                         : ab.album;
}

/* `name`, without "(2018 Remaster)" and the like when Trim Titles is on */
static const char *ab_trim(const char *name, char *buf, int size)
{
    return global_settings.liner_notes_trim ? tag_trim(name, buf, size)
                                            : name;
}

/* ---- finding the file ------------------------------------------------- */

static bool ab_keys_match(unsigned a, unsigned a2, unsigned b, unsigned b2)
{
    return (a && (a == b || a == b2)) || (a2 && (a2 == b || a2 == b2));
}

/* The playing track's album artist, artist, album and title, any
 * possibly "" */
static void ab_who(void)
{
    struct mp3entry *id3 = audio_current_track();

    ab.names[0][0] = ab.names[1][0] = ab.album[0] = ab.title[0] = '\0';
    if (!id3)
        return;
    if (id3->title)
        strlcpy(ab.title, id3->title, sizeof ab.title);
    if (id3->album)
        strlcpy(ab.album, id3->album, sizeof ab.album);
    if (id3->albumartist)
        strlcpy(ab.names[0], id3->albumartist, sizeof ab.names[0]);
    if (id3->artist)
        strlcpy(ab.names[1], id3->artist, sizeof ab.names[1]);
}

/* Which of the names a file's artist part is, 0 or 1, or `best` for neither.
 * Lower is better: the album artist before the artist. */
static int ab_rank(const char *name, size_t len, const unsigned key[2],
                   const unsigned alt[2], int best)
{
    unsigned k = art_name_hash_n(name, len);
    unsigned k2 = art_name_alt_hash_n(name, len);
    int i;

    for (i = 0; i < best; i++)
        if (key[i] && ab_keys_match(k, k2, key[i], alt[i]))
            return i;
    return best;
}

/* An Album, Track or Credits file is "<artist> - <album or title>". Either
 * part can hold " - " itself, so every split is tried against the second
 * part first. */
static int ab_rank_notes(const char *name, size_t len, const unsigned key[2],
                         const unsigned alt[2], unsigned akey, unsigned aalt,
                         int best)
{
    const char *p, *end = name + len;

    for (p = name; (p = strstr(p, " - ")) && p + 3 <= end; p++)
    {
        size_t blen = end - (p + 3);

        if (ab_keys_match(art_name_hash_n(p + 3, blen),
                          art_name_alt_hash_n(p + 3, blen), akey, aalt))
        {
            int r = ab_rank(name, p - name, key, alt, best);

            if (r < best)
                return r;
        }
    }
    return best;
}

/* The file name the PC tool writes for a name ("<artist>" or "<artist> -
 * <album>"), into ab.stem if that file is there: characters Windows forbids
 * become '_', and leading and trailing dots and spaces go. */
static bool ab_try(const char *artist, const char *album)
{
    char name[sizeof ab.stem], path[MAX_PATH];
    const char *parts[2] = { artist, album };
    size_t n = 0, start = 0;
    int i, fd;

    for (i = 0; i < 2 && parts[i]; i++)
    {
        const char *p;

        if (i && n + 3 < sizeof name)
        {
            memcpy(name + n, " - ", 3);
            n += 3;
        }
        for (p = parts[i]; *p && n + 1 < sizeof name; p++)
            name[n++] = strchr("<>:\"/\\|?*", *p) ? '_' : *p;
        if (*p)
            return false;                   /* too long to be a file */
    }
    while (start < n && (name[start] == '.' || name[start] == ' '))
        start++;
    while (n > start && (name[n - 1] == '.' || name[n - 1] == ' '))
        n--;
    if (n == start)
        return false;
    name[n] = '\0';
    snprintf(path, sizeof path, "%s/%s.txt", ab_dir(), name + start);
    if ((fd = open(path, O_RDONLY)) < 0)
        return false;
    close(fd);
    strlcpy(ab.stem, name + start, sizeof ab.stem);
    return true;
}

/* The page's .txt named for the album artist, else for the artist -- and
 * but for Bio, for the album or title -- into ab.stem. The name the PC tool
 * would have written is tried first; the folder is read only when that
 * misses (a name differing in case, accents or a leading "The"). */
static bool ab_find(void)
{
    unsigned key[2], alt[2], akey = 0, aalt = 0;
    int best = 2, i;
    DIR *d;
    struct dirent *e;

    for (i = 0; i < 2; i++)
    {
        key[i] = art_name_hash(ab.names[i]);
        alt[i] = art_name_alt_hash(ab.names[i]);
    }
    if (!key[0] && !key[1])
        return false;
    if (ab_second())
    {
        akey = art_name_hash(ab_second());
        aalt = art_name_alt_hash(ab_second());
        if (!akey)
            return false;
    }
    i = ab.names[0][0] ? 0 : 1;             /* the album artist ranks first */
    if (ab_try(ab.names[i], ab_second()))
        return true;
    if (!(d = opendir(ab_dir())))
        return false;
    while (best > 0 && (e = readdir(d)))
    {
        const char *dot = strrchr(e->d_name, '.');
        size_t len;
        int r;

        if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".txt"))
            continue;
        len = dot - e->d_name;
        if (len >= sizeof ab.stem)
            continue;
        r = ab_second() ? ab_rank_notes(e->d_name, len, key, alt, akey,
                                        aalt, best)
                        : ab_rank(e->d_name, len, key, alt, best);
        if (r < best)
        {
            memcpy(ab.stem, e->d_name, len);
            ab.stem[len] = '\0';
            best = r;
        }
    }
    closedir(d);
    return best < 2;
}

/* ---- colours and fonts ------------------------------------------------ */

static void ab_colours(void)
{
    if (ab_blurred())
    {
        /* white over the scrimmed picture, black where there is none */
        ab.fg = LCD_WHITE;
        ab.bg = LCD_BLACK;
        ab.dim = LCD_RGBPACK(200, 200, 200);
        ab.soft = LCD_RGBPACK(225, 225, 225);
        ab.vp.fg_pattern = ab.fg;
        ab.vp.bg_pattern = ab.bg;
        return;
    }
    if (global_settings.artist_bio_background == ARTIST_BIO_BG_CUSTOM)
    {
        unsigned bg = global_settings.artist_bio_custom_color;

        ab.bg = bg;
        ab.fg = color_contrast(LCD_WHITE, bg) >= color_contrast(LCD_BLACK, bg)
                ? LCD_WHITE : LCD_BLACK;
    }
    else
    {
        ab.fg = dynamic_colors_resolve(global_settings.fg_color);
        ab.bg = dynamic_colors_resolve(global_settings.bg_color);
    }
    ab.dim = color_blend(ab.bg, ab.fg, 170);
    ab.soft = color_blend(ab.bg, ab.fg, 220);
    ab.vp.fg_pattern = ab.fg;
    ab.vp.bg_pattern = ab.bg;
}

static void ab_fonts_load(void)
{
    int ui = screens[SCREEN_MAIN].getuifont();
    int i;

    for (i = 0; i < AB_FONTS; i++)
    {
        ab.loaded[i] = ab_font_files[i] ? font_load(ab_font_files[i]) : -1;
        ab.font[i] = ab.loaded[i] >= 0 ? ab.loaded[i] : ui;
    }
    ab.vp.font = ab.font[AB_FONT_BODY];
}

static void ab_fonts_unload(void)
{
    int i;

    for (i = 0; i < AB_FONTS; i++)
    {
        if (ab.loaded[i] >= 0)
            font_unload(ab.loaded[i]);
        ab.loaded[i] = -1;
    }
}

/* ---- reading the page ------------------------------------------------- */

/* Ends the line at *p, strips a trailing \r and moves *p to the next one.
 * NULL at the end of the text. */
static char *ab_line(char **p)
{
    char *s = *p, *e = s;

    if (!*s)
        return NULL;
    while (*e && *e != '\n')
        e++;
    *p = *e ? e + 1 : e;
    *e = '\0';
    if (e > s && e[-1] == '\r')
        e[-1] = '\0';
    return s;
}

/* Whether the next paragraph after p is ordinary text, not a "# " line */
static bool ab_text_follows(const char *p)
{
    while (*p == '\n' || *p == '\r')
        p++;
    return *p && !(p[0] == '#' && p[1] == ' ');
}

/* Wraps `text` in `font` to `width` into the line table, from page position
 * y down. Returns the position below the last line. */
static int ab_wrap(const char *text, enum ab_font font, int y, int width)
{
    int id = ab.font[font];
    struct font *pf = font_get(id);

    font_lock(id, true);
    while (*text && ab.nlines < AB_LINES_MAX)
    {
        const char *end, *next = text_wrap_next(text, width, pf, &end);
        struct ab_line *l = &ab.lines[ab.nlines++];

        l->text = text;
        l->len = end - text;
        l->y = y;
        l->x = 0;
        l->font = font;
        l->ink = font == AB_FONT_SUB ? AB_INK_DIM : AB_INK_FG;
        y += pf->height;
        text = next;
    }
    font_lock(id, false);
    return y;
}

/* Blurred Art: the whole screen, from the banner, scrimmed for white text,
 * and below it the same again upside down. The page scrolls over the pair
 * repeated, so the backdrop moves with the banner and never meets an edge.
 * Leaves ab.backdrop NULL if there is no room. */
static void ab_make_backdrop(const struct bitmap *bm)
{
    static struct img_filter f;
    struct img_levels lv;
    int w = ab.vp.width, h = ab.vp.height;

    if (!img_filter_compile(global_settings.artist_bio_background
                            == ARTIST_BIO_BG_BLURRED_DARK
                            ? AB_BLUR_CHAIN_DARKER : AB_BLUR_CHAIN,
                            IMG_CLASSES_ALL, &f))
        return;
    int y;

    ab.backdrop_handle = core_alloc_ex(2 * w * h * sizeof(fb_data),
                                       &buflib_ops_locked);
    if (ab.backdrop_handle <= 0)
    {
        ab.backdrop_handle = 0;
        return;
    }
    ab.backdrop = core_get_data(ab.backdrop_handle);
    img_filter_measure((const fb_data *)bm->data, bm->width, bm->height, &lv);
    img_filter_adapt(&f, &lv, 255);
    img_filter_render(ab.backdrop, w, h, (const fb_data *)bm->data,
                      bm->width, bm->height, &f);
    for (y = 0; y < h; y++)
        memcpy(ab.backdrop + (2 * h - 1 - y) * w, ab.backdrop + y * w,
               w * sizeof(fb_data));
}

/* The first banner row of the fade at its foot */
static int ab_fade_start(int height)
{
    return height > AB_FADE ? height - AB_FADE : 0;
}

/* How far banner row y has faded into what is behind it, 0 to 256 */
static int ab_fade(int y, int height)
{
    int start = ab_fade_start(height);

    return y < start ? 0 : (y - start + 1) * 256 / (height - start);
}

/* The banner at the top of Bio and Album, decoded and, on a solid
 * background, its foot faded into it; its top is darkened under the page
 * names. With `backdrop_only` (Track and Credits, drawn over the album's
 * banner blurred) it is decoded for the backdrop and let go. Leaves
 * ab.banner.data NULL when there is none. */
static void ab_load_banner(const char *dir, const char *stem,
                           bool backdrop_only)
{
    char path[MAX_PATH];
    struct image_src src = { .pos = 0, .len = -1, .flags = 0 };
    int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT
                 | FORMAT_DITHER;
    struct bitmap bm;
    int need, used, x, y;
    fb_data *px;

    memset(&ab.banner, 0, sizeof ab.banner);
    snprintf(path, sizeof path, "%s/%s.jpg", dir, stem);
    if ((src.fd = open(path, O_RDONLY)) < 0)
        return;

    /* room for the largest banner, the decode and the scaler's lines; what
     * the bitmap does not use is handed back once it is decoded */
    need = BM_SIZE(ab.vp.width, AB_BANNER_MAX, FORMAT_NATIVE, false)
           + IMAGE_DECODE_OVERHEAD + 3 * 8 * ab.vp.width + 64;
    ab.banner_handle = core_alloc_ex(need, &buflib_ops_locked);
    if (ab.banner_handle <= 0)
    {
        ab.banner_handle = 0;
        close(src.fd);
        return;
    }

    memset(&bm, 0, sizeof bm);
    bm.data = core_get_data(ab.banner_handle);
    bm.width = ab.vp.width;
    bm.height = AB_BANNER_MAX;
    used = image_load(&src, &bm, need, format, NULL);
    close(src.fd);
    if (used <= 0)
    {
        core_free(ab.banner_handle);
        ab.banner_handle = 0;
        return;
    }
    core_shrink(ab.banner_handle, bm.data, used);

    if (ab_blurred())
        ab_make_backdrop(&bm);
    if (backdrop_only || !global_settings.artist_bio_banner)
    {
        /* loaded only for the backdrop */
        core_free(ab.banner_handle);
        ab.banner_handle = 0;
        return;
    }

    /* the foot fades into what lies under it on the page: the backdrop's
     * own rows, which scroll with it, or most of the way to the solid
     * background, so the photo still shows faintly under the name */
    px = (fb_data *)bm.data;
    for (y = ab_fade_start(bm.height); y < bm.height; y++)
    {
        int t = ab.backdrop ? ab_fade(y, bm.height)
                            : ab_fade(y, bm.height) * 230 / 256;
        fb_data *row = px + y * STRIDE(SCREEN_MAIN, bm.width, bm.height);
        const fb_data *under = ab.backdrop ? ab.backdrop + y * ab.vp.width
                               + (ab.vp.width - bm.width) / 2 : NULL;

        for (x = 0; x < bm.width; x++)
            row[x] = FB_SCALARPACK_LCD(
                color_blend(FB_UNPACK_SCALAR_LCD(row[x]),
                            under ? FB_UNPACK_SCALAR_LCD(under[x]) : ab.bg,
                            t));
    }
    for (y = 0; y < AB_SHADE && y < bm.height; y++)
    {
        fb_data *row = px + y * STRIDE(SCREEN_MAIN, bm.width, bm.height);
        int t = (AB_SHADE - y) * 170 / AB_SHADE;

        for (x = 0; x < bm.width; x++)
            row[x] = FB_SCALARPACK_LCD(
                color_blend(FB_UNPACK_SCALAR_LCD(row[x]), LCD_BLACK, t));
    }
    ab.banner = bm;
}

/* One line of a page in `font` and `ink` at (x, y), from `len` bytes of
 * `text`. False when the table is full. */
static bool ab_put(const char *text, int len, enum ab_font font,
                   enum ab_ink ink, int x, int y)
{
    struct ab_line *l;

    if (ab.nlines >= AB_LINES_MAX)
        return false;
    l = &ab.lines[ab.nlines++];
    l->text = text;
    l->len = len;
    l->font = font;
    l->ink = ink;
    l->x = x;
    l->y = y;
    return true;
}

/* The width of `len` bytes of `text` in `font` */
static int ab_width(const char *text, int len, int font)
{
    char buf[128];
    int w = 0;

    len = MIN(len, (int)sizeof buf - 1);
    memcpy(buf, text, len);
    buf[len] = '\0';
    font_getstringsize((const unsigned char *)buf, &w, NULL, font);
    return w;
}

/* A piece of a credits row: a span of the text in one style, drawn `gap`
 * pixels after the piece before it */
struct ab_piece
{
    const char *text;
    int len, gap, width;
    unsigned char font, ink;
    bool joined;               /* never the first piece on a line */
};

/* Lays out a credits row, "Name<TAB>Role, Role [1-3, 5], Role", from page
 * position y: the name in the text colour, then a dash and the roles in the
 * softer colour, each bracket's track numbers small and grey and joined to
 * the word before them. Wrapped lines carry on at the margin. Returns the
 * position below the row. */
static int ab_wrap_person(char *row, int y, int width)
{
    static const char dash[] = "\xe2\x80\x93";  /* an en dash */
    struct ab_piece pc[96];
    int n = 0, i, x = 0, body = ab.font[AB_FONT_BODY];
    int space = ab_width(" ", 1, body);
    int lh = font_get(body)->height;
    int drop = font_get(body)->ascent - font_get(ab.font[AB_FONT_SUB])->ascent;
    char *tab = strchr(row, '\t'), *p;
    bool roles = false;

    if (!tab)
        return ab_wrap(row, AB_FONT_BODY, y, width) + AB_ROW_GAP;
    *tab = '\0';

    /* the name a word at a time, the dash, then the roles a word at a time,
     * each bracket one piece joined to the word before it */
    for (p = row; n < (int)ARRAYLEN(pc) - 1; )
    {
        struct ab_piece *q = &pc[n];
        int gap = n ? space : 0;

        while (*p == ' ')
            p++;
        if (!*p)
        {
            if (roles)
                break;
            /* the end of the name: the dash, and on into the roles */
            *q = (struct ab_piece){ dash, sizeof dash - 1, space, 0,
                                    AB_FONT_BODY, AB_INK_SOFT, true };
            n++;
            p = tab + 1;
            roles = true;
            continue;
        }
        if (*p == '[')
        {
            char *close = strchr(p, ']');

            if (!close)
                close = p + strlen(p);
            *q = (struct ab_piece){ p + 1, close - p - 1, gap, 0,
                                    AB_FONT_SUB, AB_INK_DIM, true };
            p = *close ? close + 1 : close;
            if (*p == ',')                  /* the comma ending that role */
            {
                n++;
                q = &pc[n];
                *q = (struct ab_piece){ p, 1, 0, 0, AB_FONT_BODY,
                                        AB_INK_SOFT, true };
                p++;
            }
        }
        else
        {
            char *e = p;

            while (*e && *e != ' ' && *e != '[')
                e++;
            *q = (struct ab_piece){ p, e - p, gap, 0, AB_FONT_BODY,
                                    roles ? AB_INK_SOFT : AB_INK_FG, false };
            p = e;
        }
        n++;
    }

    for (i = 0; i < n; i++)
        pc[i].width = ab_width(pc[i].text, pc[i].len, ab.font[pc[i].font]);

    /* a piece and those joined to it move to the next line together */
    for (i = 0; i < n; )
    {
        int j = i + 1, w = pc[i].width;

        while (j < n && pc[j].joined)
            w += pc[j].gap + pc[j].width, j++;
        if (x && x + pc[i].gap + w > width)
        {
            x = 0;
            y += lh;
            pc[i].gap = 0;
        }
        for (; i < j; i++)
        {
            struct ab_line *last = ab.nlines ? &ab.lines[ab.nlines - 1] : NULL;
            int yy = y + (pc[i].font == AB_FONT_SUB ? drop : 0);

            x += x ? pc[i].gap : 0;
            /* a word in the style of the last piece, just after it in the
             * text, on the same line, extends it */
            if (last && last->y == yy && last->font == pc[i].font
                && last->ink == pc[i].ink && pc[i].text > last->text
                && pc[i].text - (last->text + last->len) <= 1
                && pc[i].text != dash && last->text != dash)
                last->len = pc[i].text + pc[i].len - last->text;
            else if (!ab_put(pc[i].text, pc[i].len, pc[i].font, pc[i].ink,
                             x, yy))
                return y + lh;
            x += pc[i].width;
        }
    }
    return y + lh + AB_ROW_GAP;
}

/* Bio and Album: the name, a facts line, then paragraphs. "# " marks a
 * quiet line, a source or the ratings; one that ordinary text follows is
 * that text's heading. */
static int ab_layout_banner_page(char *p, int width)
{
    char *name = ab_line(&p), *sub = ab_line(&p), *para;
    int body_h = font_get(ab.font[AB_FONT_BODY])->height, y;

    if (!name || !*name)
        return -1;
    /* the name sits over the foot of the banner, else under the names */
    if (ab.banner.data)
    {
        int n = text_wrap_count(name, width, ab.font[AB_FONT_NAME]);

        y = ab.banner.height - n * font_get(ab.font[AB_FONT_NAME])->height - 3;
        ab_wrap(name, AB_FONT_NAME, y, width);
        y = ab.banner.height - 1;
    }
    else
        y = ab_wrap(name, AB_FONT_NAME, AB_STRIP + 4, width) - 2;

    if (sub && *sub)
        y = ab_wrap(sub, AB_FONT_SUB, y, width);

    y += body_h / 2 + 4;
    while ((para = ab_line(&p)))
    {
        if (!*para)
            continue;
        if (para[0] == '#' && para[1] == ' ' && ab_text_follows(p))
            y = ab_wrap(para + 2, AB_FONT_HEAD, y, width) + body_h / 4;
        else if (para[0] == '#' && para[1] == ' ')
            y = ab_wrap(para + 2, AB_FONT_SUB, y, width) + body_h / 2;
        else
            y = ab_wrap(para, AB_FONT_BODY, y, width) + body_h / 2;
    }
    return y;
}

/* The value of a "#key<TAB>value" line, or NULL */
static char *ab_tag(char *line, const char *key)
{
    size_t n = strlen(key);

    return !strncmp(line, key, n) && line[n] == '\t' ? line + n + 1 : NULL;
}

/* "Track 3, Let's Dance (2018 Remaster)" with the album trimmed */
static char *ab_trim_facts(char *facts, char *buf, int size)
{
    char *comma = strstr(facts, ", ");
    char album[MAX_PATH];

    if (!global_settings.liner_notes_trim || !comma)
        return facts;
    snprintf(buf, size, "%.*s, %s", (int)(comma - facts), facts,
             ab_trim(comma + 2, album, sizeof album));
    return buf;
}

/* Track and Credits: a title, grey lines, then the page. Track's is
 * paragraphs; Credits' is "## Group" headings over people, and stops at
 * "#links", which is Other Appearances'. The trimmed title and facts are
 * kept in the text buffer's spare room at `spare`. */
static int ab_layout_text_page(char *p, int width, char *spare, int room)
{
    int body_h = font_get(ab.font[AB_FONT_BODY])->height;
    int y = AB_STRIP + 2;
    char *line, *v;
    bool titled = false;

    if (ab.page == AB_CREDITS)
    {
        const char *t = ab_trim(ab.album, spare, room);

        if (t != spare)
            strlcpy(spare, t, room);
        y = ab_wrap(spare, AB_FONT_NAME, y, width);
        room -= strlen(spare) + 1;
        spare += strlen(spare) + 1;
        titled = true;
    }
    while ((line = ab_line(&p)))
    {
        if (!*line)
            continue;
        if ((v = ab_tag(line, "#title")) && !titled)
        {
            const char *t = ab_trim(v, spare, room);

            y = ab_wrap(t, AB_FONT_NAME, y, width);
            if (t == spare)
            {
                room -= strlen(spare) + 1;
                spare += strlen(spare) + 1;
            }
            titled = true;
        }
        else if ((v = ab_tag(line, "#facts")))
        {
            char *t = ab_trim_facts(v, spare, room);

            y = ab_wrap(t, AB_FONT_SUB, y, width);
            if (t == spare)
            {
                room -= strlen(spare) + 1;
                spare += strlen(spare) + 1;
            }
        }
        else if ((v = ab_tag(line, "#status")))
            y = ab_wrap(v, AB_FONT_SUB, y, width);
        else if ((v = ab_tag(line, "#sources")))
            y = ab_wrap(v, AB_FONT_SUB, y + body_h / 2, width);
        else if (!strcmp(line, "#links"))
            break;
        else if (line[0] == '#' && line[1] == '#' && line[2] == ' ')
            y = ab_wrap(line + 3, AB_FONT_HEAD, y + body_h / 2, width)
                + body_h / 4;
        else if (line[0] == '#')
            continue;                       /* a line for later versions */
        else if (ab.page == AB_CREDITS)
            y = ab_wrap_person(line, y, width);
        else
            y = ab_wrap(line, AB_FONT_BODY, y + body_h / 2, width);
    }
    return y;
}

/* Reads ab.stem's text and lays it out. False if there is none to show. */
static bool ab_load_text(void)
{
    char path[MAX_PATH];
    char *text, *p;
    int fd, len, width = ab.vp.width - 2 * AB_MARGIN;
    int y;
    off_t size;
    bool truncated;

    snprintf(path, sizeof path, "%s/%s.txt", ab_dir(), ab.stem);
    if ((fd = open(path, O_RDONLY)) < 0)
        return false;
    size = ffilesize(fd);
    truncated = size > AB_TEXT_MAX;
    if (truncated)
        size = AB_TEXT_MAX;
    if (size <= 0)
    {
        close(fd);
        return false;
    }

    /* the text, room for a trimmed title and facts line, the line table */
    len = ALIGN_UP((int)size + 1 + 2 * MAX_PATH, 4);
    ab.text_handle = core_alloc_ex(len + AB_LINES_MAX * sizeof(struct ab_line),
                                   &buflib_ops_locked);
    if (ab.text_handle <= 0)
    {
        ab.text_handle = 0;
        close(fd);
        return false;
    }
    text = core_get_data(ab.text_handle);
    ab.lines = (struct ab_line *)(text + len);
    size = read(fd, text, size);
    close(fd);
    if (size <= 0)
        return false;
    if (truncated)
    {
        /* end on a whole character */
        while (size > 0 && (text[size - 1] & 0xc0) == 0x80)
            size--;
        if (size > 0 && (unsigned char)text[size - 1] >= 0xc0)
            size--;
    }
    text[size] = '\0';

    p = text;
    if (!strncmp(p, "\xef\xbb\xbf", 3))
        p += 3;
    if (ab.page == AB_BIO || ab.page == AB_ALBUM)
        y = ab_layout_banner_page(p, width);
    else
        y = ab_layout_text_page(p, width, text + size + 1, 2 * MAX_PATH);
    if (y < 0)
        return false;
    ab.height = y + AB_MARGIN;
    return true;
}

static void ab_free_buffers(void)
{
    if (ab.text_handle > 0)
        core_free(ab.text_handle);
    if (ab.banner_handle > 0)
        core_free(ab.banner_handle);
    if (ab.backdrop_handle > 0)
        core_free(ab.backdrop_handle);
    ab.text_handle = ab.banner_handle = ab.backdrop_handle = 0;
    ab.backdrop = NULL;
    ab.lines = NULL;
    ab.nlines = 0;
    ab.banner.data = NULL;
}

static void ab_close(void)
{
    ab_free_buffers();
    ab.found = false;
    ab_fonts_unload();
}

/* Reads everything the settings and the files decide */
static void ab_open(void)
{
    ab_colours();
    ab_fonts_load();
    ab.nlines = 0;
    ab.height = 0;
    ab.banner.data = NULL;
    ab.tvp = ab.vp;
    ab.tvp.y += AB_STRIP;
    ab.tvp.height -= AB_STRIP;
    if ((ab.page == AB_TRACK || ab.page == AB_CREDITS) && ab_blurred())
    {
        /* drawn over the album's banner, blurred */
        int page = ab.page;

        ab.page = AB_ALBUM;
        if (ab_find())
            ab_load_banner(LINER_NOTES_DIR, ab.stem, true);
        ab.page = page;
    }
    if (!ab_find())
        return;
    if ((ab.page == AB_BIO || ab.page == AB_ALBUM)
        && (global_settings.artist_bio_banner || ab_blurred()))
        ab_load_banner(ab_dir(), ab.stem, false);
    ab.found = ab_load_text();
    if (!ab.found)
    {
        ab_free_buffers();
        ab.height = 0;
    }
}

/* ---- drawing ---------------------------------------------------------- */

static int ab_max_pos(void)
{
    return ab.height > ab.vp.height ? ab.height - ab.vp.height : 0;
}

static void ab_draw_empty(struct screen *d)
{
    static const int none[AB_PAGES] = {
        LANG_ARTIST_BIO_NONE, LANG_LINER_NOTES_NONE, LANG_TRACK_NOTES_NONE,
        LANG_CREDITS_NONE
    };
    const char *who = ab.page == AB_BIO ? ab.names[0][0] ? ab.names[0]
                                                        : ab.names[1]
                      : ab.page == AB_TRACK ? ab.title : ab.album;
    int h = font_get(ab.font[AB_FONT_BODY])->height;
    int width = ab.vp.width - 2 * AB_MARGIN;
    int y = ab.vp.height / 2 - h;

    ab.vp.font = ab.font[AB_FONT_BODY];
    d->set_foreground(ab.fg);
    text_wrap_draw(d, str(none[ab.page]), AB_MARGIN, y, width, 'c');
    if (who[0])
    {
        d->set_foreground(ab.dim);
        text_wrap_draw(d, who, AB_MARGIN, y + h + 4, width, 'c');
    }
}

/* The names of the pages shown, across the top, this one's in the text
 * colour. Over the banner on Bio and Album; the text scrolls under them. */
static void ab_draw_strip(struct screen *d)
{
    static const int names[AB_PAGES] = {
        LANG_LINER_NOTES_BIO, LANG_ID3_ALBUM, LANG_TRACK, LANG_CREDITS
    };
    int font = ab.font[AB_FONT_SUB], width[AB_PAGES], total = 0, x, y, i;

    for (i = 0; i < AB_PAGES; i++)
    {
        width[i] = 0;
        if (ab_page_ok(i))
        {
            font_getstringsize((const unsigned char *)str(names[i]),
                               &width[i], NULL, font);
            total += width[i] + AB_STRIP_GAP;
        }
    }
    x = (ab.vp.width - total + AB_STRIP_GAP) / 2;
    y = (AB_STRIP - font_get(font)->height) / 2 + 1;
    ab.vp.font = font;
    for (i = 0; i < AB_PAGES; i++)
    {
        if (!width[i])
            continue;
        if (ab.banner.data && ab.pos < AB_STRIP)
        {
            /* a shadow, so the names read on a light photo */
            d->set_foreground(LCD_BLACK);
            d->putsxy(x + 1, y + 1, (const unsigned char *)str(names[i]));
        }
        d->set_foreground(i == ab.page ? ab.fg : ab.dim);
        d->putsxy(x, y, (const unsigned char *)str(names[i]));
        x += width[i] + AB_STRIP_GAP;
    }
}

/* The backdrop pair from page row ab.pos down: to the pair's end, then
 * from its start again */
static void ab_draw_backdrop(struct screen *d)
{
    int w = ab.vp.width, h = ab.vp.height;
    int stride = STRIDE(SCREEN_MAIN, w, 2 * h);
    int top = ab.pos % (2 * h), first = MIN(h, 2 * h - top);

    d->bitmap_part(ab.backdrop, 0, top, stride, 0, 0, w, first);
    if (first < h)
        d->bitmap_part(ab.backdrop, 0, 0, stride, 0, first, w, h - first);
}

static void ab_draw(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport *last = d->set_viewport(&ab.vp);
    char buf[256];
    int i;

    d->set_background(ab.bg);
    if (ab.backdrop)
        ab_draw_backdrop(d);
    else
        d->clear_viewport();

    if (!ab.found)
    {
        ab_draw_empty(d);
        goto out;
    }

    if (ab.banner.data && ab.pos < ab.banner.height)
        d->bitmap_part(ab.banner.data, 0, ab.pos,
                       STRIDE(SCREEN_MAIN, ab.banner.width, ab.banner.height),
                       (ab.vp.width - ab.banner.width) / 2, 0,
                       ab.banner.width, ab.banner.height - ab.pos);

    d->set_viewport(&ab.tvp);
    d->set_drawmode(DRMODE_FG);         /* the name lies over the photo */
    for (i = 0; i < ab.nlines; i++)
    {
        const struct ab_line *l = &ab.lines[i];
        int y = l->y - ab.pos;
        int len = MIN(l->len, (int)sizeof buf - 1);

        if (y >= ab.vp.height)
            break;
        if (y + font_get(ab.font[l->font])->height <= AB_STRIP)
            continue;
        memcpy(buf, l->text, len);
        buf[len] = '\0';
        ab.tvp.font = ab.font[l->font];
        d->set_foreground(l->ink == AB_INK_DIM ? ab.dim
                          : l->ink == AB_INK_SOFT ? ab.soft : ab.fg);
        d->putsxy(AB_MARGIN + l->x, y - AB_STRIP, (const unsigned char *)buf);
    }
    d->set_drawmode(DRMODE_SOLID);
    d->set_viewport(&ab.vp);

    if (ab_max_pos() > 0)
    {
        int vh = ab.vp.height - AB_STRIP;
        int th = MAX(vh * vh / ab.height, 12);
        int ty = AB_STRIP + ab.pos * (vh - th) / ab_max_pos();

        d->set_foreground(ab.dim);
        d->fillrect(ab.vp.width - AB_BAR_W - 2, ty, AB_BAR_W, th);
    }

  out:
    d->set_drawmode(DRMODE_FG);
    ab_draw_strip(d);
    d->set_drawmode(DRMODE_SOLID);
    ab.vp.font = ab.font[AB_FONT_BODY];
    d->update_viewport();
    d->set_viewport(last);
}

/* ---- scrolling -------------------------------------------------------- */

/* One wheel click. Clicks close together go further each time, so a quick
 * turn crosses a long bio and a slow one reads line by line. */
static void ab_scroll(int dir)
{
    static long last;
    static int accel, last_dir;

    if (dir == last_dir && TIME_BEFORE(current_tick, last + HZ / 6))
        accel = MIN(accel + 1, AB_ACCEL_MAX);
    else
        accel = 0;
    last = current_tick;
    last_dir = dir;

    ab.target += dir * (AB_STEP + accel * AB_STEP / 2);
    ab.target = MAX(ab.pos - AB_LEAD_MAX, MIN(ab.target, ab.pos + AB_LEAD_MAX));
    ab.target = MAX(0, MIN(ab.target, ab_max_pos()));
}

/* A quarter of the way to the target each frame, at least a pixel and at
 * most AB_GLIDE_MAX */
static void ab_glide(void)
{
    int diff = ab.target - ab.pos;
    int move = diff / 4;

    if (!move)
        move = diff > 0 ? 1 : -1;
    ab.pos += MAX(-AB_GLIDE_MAX, MIN(move, AB_GLIDE_MAX));
}

/* Left and Right: the page before or after this one, if one is shown.
 * False when there is none. */
static bool ab_go(int dir)
{
    int page = ab_page_step(ab.page, dir);

    if (page < 0)
        return false;
    ab_close();
    ab.page = page;
    ab.pos = ab.target = 0;
    ab_open();
    ab_draw();
    return true;
}

/* The button behind an ACTION_STD_OK or _CANCEL, for telling Right from
 * Select and Left from Menu */
static int ab_button(void)
{
    int btn = 0;

    get_action_statuscode(&btn);
#ifdef BUTTON_RC_RIGHT
    if (btn & BUTTON_RC_RIGHT)
        btn |= BUTTON_RIGHT;
#endif
#ifdef BUTTON_RC_LEFT
    if (btn & BUTTON_RC_LEFT)
        btn |= BUTTON_LEFT;
#endif
    return btn;
}

/* ---- the entry point -------------------------------------------------- */

/* ab_menu(): the pages were all switched off */
#define AB_MENU_EMPTY (MENU_ATTACHED_USB + 1)

/* Hold-Menu settings, the same menu as Settings > Appearance > Viewers >
 * Liner Notes. The buffers are let go first: they are immovable and the menu
 * allocates. False when the pages were switched off there, leaving none to
 * show. */
static int ab_menu(void)
{
    int sel = 0, ret;

    ab_close();
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    ret = do_menu(&artist_bio_menu, &sel, NULL, false);
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);

    if (!global_settings.liner_notes)
        return AB_MENU_EMPTY;
    if (!ab_page_ok(ab.page))
    {
        int page = ab_page_step(ab.page, 1);

        if (page < 0)
            page = ab_page_step(ab.page, -1);
        if (page < 0)
            return AB_MENU_EMPTY;
        ab.page = page;
        ab.target = 0;
    }
    ab_open();
    ab.target = MIN(ab.target, ab_max_pos());
    ab.pos = ab.target;
    ab_draw();
    return ret;
}

/* The screen itself, for the names already in ab.names */
static int ab_run(void)
{
    int ret = GO_TO_PREVIOUS;

    ab.text_handle = ab.banner_handle = ab.backdrop_handle = 0;
    ab.backdrop = NULL;
    ab.loaded[0] = ab.loaded[1] = ab.loaded[2] = ab.loaded[3] = -1;
    ab.pos = ab.target = 0;

    push_current_activity(ACTIVITY_ARTISTBIO);
    viewportmanager_theme_enable(SCREEN_MAIN, false, &ab.vp);
    ab_open();
    ab_draw();

    while (1)
    {
        int action, timeout = HZ;

        if (ab.pos != ab.target)
        {
            ab_glide();
            ab_draw();
            timeout = 1;
        }

        action = get_action(CONTEXT_STD, timeout);
        switch (action)
        {
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                ab_scroll(1);
                break;

            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                ab_scroll(-1);
                break;

            case ACTION_STD_OK:             /* Right: the next page */
                if (ab_button() & BUTTON_RIGHT)
                    ab_go(1);
                break;

            case ACTION_STD_CANCEL:         /* Left: the page before, or out */
                if ((ab_button() & BUTTON_LEFT) && ab_go(-1))
                    break;
                goto done;

            case ACTION_STD_MENU:           /* Menu and Play leave */
            case ACTION_TREE_WPS:
                goto done;

            case ACTION_STD_QUICKSCREEN:    /* hold Menu: the settings */
                switch (ab_menu())
                {
                    case MENU_ATTACHED_USB:
                        ret = GO_TO_ROOT;
                        goto done;
                    case AB_MENU_EMPTY:             /* nothing left to show */
                        goto done;
                }
                break;

            case ACTION_NONE:
                break;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;
        }
    }

  done:
    ab_close();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    pop_current_activity();
    return ret;
}

bool liner_notes_page_on(bool bio)
{
    const struct user_settings *gs = &global_settings;

    return gs->liner_notes
           && (bio ? gs->liner_notes_bio
                   : gs->liner_notes_album || gs->liner_notes_track
                     || gs->liner_notes_credits);
}

/* Opens at `page`, else the nearest page shown after it, else before it.
 * With none shown, the page itself says there is nothing, if it is on. */
static int ab_start(int page)
{
    static const bool *const on[AB_PAGES] = {
        &global_settings.liner_notes_bio, &global_settings.liner_notes_album,
        &global_settings.liner_notes_track,
        &global_settings.liner_notes_credits
    };
    int p = page;

    if (!global_settings.liner_notes)
        return GO_TO_PREVIOUS;
    if (!ab_page_ok(p))
    {
        p = ab_page_step(page, 1);
        if (p < 0)
            p = ab_page_step(page, -1);
        if (p < 0)
        {
            if (!*on[page])
                return GO_TO_PREVIOUS;
            p = page;
        }
    }
    ab.page = p;
    return ab_run();
}

int artist_bio(void)
{
    ab_who();
    return ab_start(AB_BIO);
}

int artist_bio_of(const char *name)
{
    strlcpy(ab.names[0], name ? name : "", sizeof ab.names[0]);
    ab.names[1][0] = ab.album[0] = ab.title[0] = '\0';
    return ab_start(AB_BIO);
}

int liner_notes(void)
{
    ab_who();
    return ab_start(AB_ALBUM);
}

int liner_notes_of(const char *artist, const char *album)
{
    strlcpy(ab.names[0], artist ? artist : "", sizeof ab.names[0]);
    ab.names[1][0] = ab.title[0] = '\0';
    strlcpy(ab.album, album ? album : "", sizeof ab.album);
    return ab_start(AB_ALBUM);
}
