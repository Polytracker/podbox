/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The artist bio screen. A page about the playing track's artist -- banner
 * photo, name, dates, and the biography beneath -- scrolled with the wheel
 * like a web page, the photo scrolling away with the rest.
 *
 * How it is built:
 *   - A plain core screen, entered as artist_bio() and returning a GO_TO_*
 *     code. Reached from the WPS context menu and hotkeys, and as
 *     artist_bio_of() from the menu of an artist or album in a Music list.
 *   - The page is files the PC tool writes into /Artwork/Bios: <name>.txt
 *     (the name, a dates line, a blank line, then paragraphs) and <name>.jpg,
 *     a 320 x 160 banner already cropped around the faces. A file is the
 *     artist's when its name matches the album artist or the artist the way
 *     /Artwork/People matches, so case, accents and a leading "The" do not
 *     matter.
 *   - It owns the whole screen with the theme off, on one solid colour, so the
 *     banner's foot can fade into it and the name sit over the fade.
 *   - The text is wrapped once, on opening, into a table of lines at fixed
 *     page positions; a frame draws the lines that fall on screen.
 *   - Both buffers are immovable and freed while the settings menu runs, then
 *     the page is read again, since a setting may change any of it.
 *
 * Parts, in order: finding the file, colours and fonts, reading the page,
 * drawing, scrolling, and the artist_bio() entry point.
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
#include "input/action.h"
#include "system/activity.h"
#include "system/shutdown.h"      /* default_event_handler */
#include "root_menu.h"            /* GO_TO_*, MENU_ATTACHED_USB */
#include "widgets/menu.h"         /* do_menu */
#include "screens/settings/exported_settings.h"  /* artist_bio_menu */
#include "artist_bio.h"

#define AB_MARGIN     10
#define AB_TEXT_MAX   (24 * 1024)  /* about six times the longest seen */
#define AB_LINES_MAX  1024
#define AB_BANNER_MAX 240          /* tallest banner drawn, in pixels */
#define AB_FADE       64           /* rows at the banner's foot that fade */
#define AB_STEP       8            /* pixels per wheel click, at rest */
#define AB_ACCEL_MAX  14           /* each adds half a step */
/* The 6G's panel tears: each frame shows a seam as wide as that frame's
 * move, and nothing can be waited on to avoid it. Small moves at a high
 * frame rate keep the seam to a few pixels. */
#define AB_GLIDE_MAX  1            /* most pixels the page moves a frame */
#define AB_BAR_W      3

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

struct ab_line
{
    const char *text;          /* into the text buffer, not terminated */
    int y;                     /* top, from the top of the page */
    unsigned short len;
    unsigned char font;        /* enum ab_font */
};

static struct
{
    struct viewport vp;
    unsigned fg, bg, dim;
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
    bool notes;                /* liner notes rather than a bio */
    bool playing;              /* for the playing track: Right swaps page */
    int backdrop_handle;       /* Blurred Art: the screen behind the text */
    fb_data *backdrop;         /* NULL without one */
    char names[2][MAX_PATH];   /* album artist, artist */
    char album[MAX_PATH];      /* for liner notes */
    char stem[MAX_PATH - 24];  /* the file's name, without ".txt" */
} ab;

static const char *ab_dir(void)
{
    return ab.notes ? LINER_NOTES_DIR : ARTIST_BIO_DIR;
}

/* ---- finding the file ------------------------------------------------- */

static bool ab_keys_match(unsigned a, unsigned a2, unsigned b, unsigned b2)
{
    return (a && (a == b || a == b2)) || (a2 && (a2 == b || a2 == b2));
}

/* The playing track's album artist, artist and album, any possibly "" */
static void ab_who(void)
{
    struct mp3entry *id3 = audio_current_track();

    ab.names[0][0] = ab.names[1][0] = ab.album[0] = '\0';
    if (!id3)
        return;
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

/* A liner notes file is "<artist> - <album>". Either part can hold " - "
 * itself, so every split is tried against the album first. */
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

/* The .txt named for the album artist, else for the artist -- and for
 * liner notes, for the album -- into ab.stem. The name the PC tool would
 * have written is tried first; the folder is read only when that misses
 * (a name differing in case, accents or a leading "The"). */
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
    if (ab.notes)
    {
        akey = art_name_hash(ab.album);
        aalt = art_name_alt_hash(ab.album);
        if (!akey)
            return false;
    }
    i = ab.names[0][0] ? 0 : 1;             /* the album artist ranks first */
    if (ab_try(ab.names[i], ab.notes ? ab.album : NULL))
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
        r = ab.notes ? ab_rank_notes(e->d_name, len, key, alt, akey, aalt,
                                     best)
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
        l->font = font;
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

/* The banner above the bio, decoded and, on a solid background, its foot
 * faded into it. Leaves ab.banner.data NULL when there is none. */
static void ab_load_banner(void)
{
    char path[MAX_PATH];
    struct image_src src = { .pos = 0, .len = -1, .flags = 0 };
    int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT
                 | FORMAT_DITHER;
    struct bitmap bm;
    int need, used, x, y;
    fb_data *px;

    memset(&ab.banner, 0, sizeof ab.banner);
    snprintf(path, sizeof path, "%s/%s.jpg", ab_dir(), ab.stem);
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
    if (!global_settings.artist_bio_banner)
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
    ab.banner = bm;
}

/* Reads ab.stem's text and lays it out. False if there is none to show. */
static bool ab_load_text(void)
{
    char path[MAX_PATH];
    char *text, *p, *name, *sub, *para;
    int fd, len, width = ab.vp.width - 2 * AB_MARGIN;
    int body_h, y;
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

    len = ALIGN_UP((int)size + 1, 4);
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
    name = ab_line(&p);
    sub = ab_line(&p);
    if (!name || !*name)
        return false;

    /* the name sits over the foot of the banner, else at the top */
    if (ab.banner.data)
    {
        int n = text_wrap_count(name, width, ab.font[AB_FONT_NAME]);

        y = ab.banner.height - n * font_get(ab.font[AB_FONT_NAME])->height - 3;
        ab_wrap(name, AB_FONT_NAME, y, width);
        y = ab.banner.height - 1;
    }
    else
        y = ab_wrap(name, AB_FONT_NAME, AB_MARGIN + 4, width) - 2;

    if (sub && *sub)
        y = ab_wrap(sub, AB_FONT_SUB, y, width);

    body_h = font_get(ab.font[AB_FONT_BODY])->height;
    y += body_h / 2 + 4;
    while ((para = ab_line(&p)))
    {
        if (!*para)
            continue;
        /* "# " marks a quiet line, a source or the ratings; one that
         * ordinary text follows is that text's heading */
        if (para[0] == '#' && para[1] == ' ' && ab_text_follows(p))
            y = ab_wrap(para + 2, AB_FONT_HEAD, y, width) + body_h / 4;
        else if (para[0] == '#' && para[1] == ' ')
            y = ab_wrap(para + 2, AB_FONT_SUB, y, width) + body_h / 2;
        else
            y = ab_wrap(para, AB_FONT_BODY, y, width) + body_h / 2;
    }
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
    if (!ab_find())
        return;
    if (global_settings.artist_bio_banner
        || ab_blurred())
        ab_load_banner();
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
    const char *who = ab.notes ? ab.album
                      : ab.names[0][0] ? ab.names[0] : ab.names[1];
    int h = font_get(ab.font[AB_FONT_BODY])->height;
    int width = ab.vp.width - 2 * AB_MARGIN;
    int y = ab.vp.height / 2 - h;

    ab.vp.font = ab.font[AB_FONT_BODY];
    d->set_foreground(ab.fg);
    text_wrap_draw(d, str(ab.notes ? LANG_LINER_NOTES_NONE
                                   : LANG_ARTIST_BIO_NONE),
                   AB_MARGIN, y, width, 'c');
    if (who[0])
    {
        d->set_foreground(ab.dim);
        text_wrap_draw(d, who, AB_MARGIN, y + h + 4, width, 'c');
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

    d->set_drawmode(DRMODE_FG);         /* the name lies over the photo */
    for (i = 0; i < ab.nlines; i++)
    {
        const struct ab_line *l = &ab.lines[i];
        int y = l->y - ab.pos;
        int len = MIN(l->len, (int)sizeof buf - 1);

        if (y >= ab.vp.height)
            break;
        if (y + font_get(ab.font[l->font])->height <= 0)
            continue;
        memcpy(buf, l->text, len);
        buf[len] = '\0';
        ab.vp.font = ab.font[l->font];
        d->set_foreground(l->font == AB_FONT_SUB ? ab.dim : ab.fg);
        d->putsxy(AB_MARGIN, y, (const unsigned char *)buf);
    }
    d->set_drawmode(DRMODE_SOLID);

    if (ab_max_pos() > 0)
    {
        int vh = ab.vp.height;
        int th = MAX(vh * vh / ab.height, 12);
        int ty = ab.pos * (vh - th) / ab_max_pos();

        d->set_foreground(ab.dim);
        d->fillrect(ab.vp.width - AB_BAR_W - 2, ty, AB_BAR_W, th);
    }

  out:
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

/* Right, on the playing track's page: the bio and the album's notes swap.
 * Nothing without an album to have notes for, or an artist for a bio. */
static void ab_swap(void)
{
    bool notes = !ab.notes;

    ab_who();
    if (notes ? !ab.album[0] : !ab.names[0][0] && !ab.names[1][0])
        return;
    ab_close();
    ab.notes = notes;
    ab.pos = ab.target = 0;
    ab_open();
    ab_draw();
}

/* ---- the entry point -------------------------------------------------- */

/* Hold-Menu settings, the same menu as Settings > Appearance > Viewers >
 * Artist Bio. The buffers are let go first: they are immovable and the menu
 * allocates. */
static int ab_menu(void)
{
    int sel = 0, ret;

    ab_close();
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    ret = do_menu(&artist_bio_menu, &sel, NULL, false);
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);

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

            case ACTION_STD_OK:             /* Right, not Select: swap page */
            {
                int btn = 0;

                get_action_statuscode(&btn);
#ifdef BUTTON_RC_RIGHT
                if (btn & BUTTON_RC_RIGHT)
                    btn |= BUTTON_RIGHT;
#endif
                if (ab.playing && (btn & BUTTON_RIGHT))
                    ab_swap();
                break;
            }

            case ACTION_STD_CANCEL:         /* Left, Menu and Play leave */
            case ACTION_STD_MENU:
            case ACTION_TREE_WPS:
                goto done;

            case ACTION_STD_QUICKSCREEN:    /* hold Menu: the settings */
                if (ab_menu() == MENU_ATTACHED_USB)
                {
                    ret = GO_TO_ROOT;
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

int artist_bio(void)
{
    ab.notes = false;
    ab.playing = true;
    ab_who();
    return ab_run();
}

int artist_bio_of(const char *name)
{
    ab.notes = false;
    ab.playing = false;
    strlcpy(ab.names[0], name ? name : "", sizeof ab.names[0]);
    ab.names[1][0] = ab.album[0] = '\0';
    return ab_run();
}

int liner_notes(void)
{
    ab.notes = true;
    ab.playing = true;
    ab_who();
    return ab_run();
}

int liner_notes_of(const char *artist, const char *album)
{
    ab.notes = true;
    ab.playing = false;
    strlcpy(ab.names[0], artist ? artist : "", sizeof ab.names[0]);
    ab.names[1][0] = '\0';
    strlcpy(ab.album, album ? album : "", sizeof ab.album);
    return ab_run();
}
