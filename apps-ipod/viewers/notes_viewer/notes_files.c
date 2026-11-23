/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The page index: every page of the notes, where it is and the keys of its
 * name. A page is a .md or .txt file anywhere in /Notes -- People/ a person,
 * Albums/<Artist - Album> an album, any other folder anything else -- or
 * one beside the music, named as though it were in /Notes: bio.md in a
 * folder is the person the folder is named for, and notes.md in
 * <Artist>/<Album> the page "Artist - Album". Names are matched by the keys
 * /Artwork's pictures are (art_name.h), so capitals, accents, symbols, "The"
 * and "Surname, First" do not matter, and a page in /Notes wins over the
 * same page beside the music.
 *
 * The index is built by the file index's walk (file_index.c), which passes
 * every one of these files, saved, and read at boot: so a lookup reads no
 * folder, and the context menus' notes rows cost nothing to ask about.
 * Until a walk has written it, there are no pages. A walk stages what it
 * finds in a file and the index in memory is replaced only when the walk
 * completes, so the pages answer throughout it; for the moment of the swap
 * notes_page_get() returns NULL, which every caller checks.
 *
 * The index takes the memory its pages need and no more: one block, the
 * pages then their paths, allocated when it is read at boot and again only
 * when a walk finds more than it holds. The pages are sorted by key, so a
 * page named by its key or its place (notes_page_at()) is found by halving.
 * Without a page there is no block.
 *
 * A wiki a scraper wrote says so with NOTES_MARKER in /Notes. Then /Notes
 * is not walked and its pages are not in the index: each is reached by its
 * file. A labelled link names the file, and a library album's or artist's
 * page is the file its names give (notes_file_name()). The page opened that
 * way is the file page, NOTES_PAGE_FILE, one at a time.
 *
 * Parts, in order:
 *   - the index: built by the walk, saved, read at boot, asked
 *   - a scraper's wiki: the file page
 *   - the lookups for a track and an album
 *   - a page's front matter
 ****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>                 /* qsort */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "system.h"                 /* MIN */
#include "string-extra.h"
#include "file.h"
#include "dir.h"
#include "database/tagcache.h"
#include "database/path_key.h"
#include "system/library_files.h"
#include "system/strutil.h"         /* BOM_UTF_8 */
#include "settings/settings.h"
#include "metadata/art_name.h"
#include "metadata/name_index.h"   /* name_index_pages_changed */
#include "system/debug_log.h"
#include "core_alloc.h"
#include "notes_files.h"
#include "notes_links.h"            /* notes_is_artist */

/* ---- the index --------------------------------------------------------- */

#define NW_FILE      LIB_CACHE_DIR "/notes.dat"
#define NW_STAGE     LIB_CACHE_DIR "/notes.new"  /* a walk's pages so far */
#define NW_MAGIC     0x4e4f5434     /* "NOT4": sorted by key */
#define NW_PAGES_MAX 65535          /* a page's number fits the names index */
#define NW_POOL_MAX  (8 * 1024 * 1024)

static struct
{
    bool loaded;                /* the index answers; else there are none */
    bool scraped;               /* NOTES_MARKER was there at the last load
                                   or walk */
    unsigned int generation;    /* counts each index loaded or replaced */
    int npages, pool_used;
    int handle;                 /* the block, 0 for none */
    size_t room;                /* its bytes */
    struct notes_page *pages;   /* sorted by key */
    char *pool;                 /* their paths */
} nw;

struct nw_header
{
    uint32_t magic;
    int32_t npages, pool_used;
};

/* The size and time of the file the walk is passing */
static uint32_t nw_size, nw_mtime;

/* The walk's staging file: each page as a struct notes_page, its `path`
 * the length of the path that follows it */
static int nw_stage = -1;
static int nw_staged, nw_staged_pool;

/* A text file's extension: 2 for .md, 1 for .txt, 0 for anything else */
static int notes_ext_rank(const char *dot)
{
    if (!dot)
        return 0;
    if (!strcasecmp(dot, ".md"))
        return 2;
    return !strcasecmp(dot, ".txt");
}

size_t notes_bare_len(const char *name, size_t len)
{
    size_t i;

    if (len < 4 || name[len - 1] != ')')
        return len;
    for (i = len - 1; i > 0 && name[i] != '('; i--)
        ;
    return i > 1 && name[i - 1] == ' ' ? i - 1 : len;
}

/* The key of "artist - title" given as two runs of bytes; the title's alone
 * with no artist */
static unsigned int nw_work_key(const char *artist, size_t alen,
                                const char *title, size_t tlen)
{
    static char a[MAX_PATH], t[MAX_PATH];

    snprintf(a, sizeof(a), "%.*s", (int)alen, artist);
    snprintf(t, sizeof(t), "%.*s", (int)tlen, title);
    return art_name_work_hash(alen ? a : NULL, t);
}

/* Whether the index read from the file holds together: every path inside
 * the pool, which ends in a NUL, every kind one there is, and the keys in
 * order */
static bool nw_valid(int npages, int pool_used)
{
    int i;

    if (npages && (!pool_used || nw.pool[pool_used - 1]))
        return false;
    for (i = 0; i < npages; i++)
        if (nw.pages[i].path >= (uint32_t)pool_used
            || nw.pages[i].kind < NOTES_PAGE_PERSON
            || nw.pages[i].kind > NOTES_PAGE_OTHER
            || (i && nw.pages[i].key < nw.pages[i - 1].key))
            return false;
    return true;
}

/* A block for `npages` pages and `pool` bytes of their paths, the one held
 * when it is big enough, nw.pages and nw.pool laid out in it; false when
 * there is no room, the block held left as it was. It never moves: its
 * pages are handed out by pointer. */
static bool nw_room(int npages, int pool)
{
    size_t need = ALIGN_UP(npages * sizeof(struct notes_page) + pool, 4);
    int h;

    if (need > nw.room || !nw.handle)
    {
        if (!need)
            return true;
        h = core_alloc_ex(need, &buflib_ops_locked);
        if (h <= 0)
        {
            debug_log(DEBUG_LOG_TASKS, "notes: no room for %d pages", npages);
            return false;
        }
        if (nw.handle)
            core_free(nw.handle);
        nw.handle = h;
        nw.room = need;
    }
    nw.pages = core_get_data(nw.handle);
    nw.pool = (char *)(nw.pages + npages);
    return true;
}

bool notes_where_load(void)
{
    struct nw_header h;
    int fd = open(NW_FILE, O_RDONLY);
    bool ok;

    nw.loaded = false;
    nw.generation++;
    nw.scraped = file_exists(NOTES_DIR "/" NOTES_MARKER);
    if (fd < 0)
        return false;
    ok = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && h.magic == NW_MAGIC
         && h.npages >= 0 && h.npages <= NW_PAGES_MAX
         && h.pool_used >= 0 && h.pool_used <= NW_POOL_MAX
         && nw_room(h.npages, h.pool_used)
         && read(fd, nw.pages, h.npages * sizeof(nw.pages[0]))
            == (ssize_t)(h.npages * sizeof(nw.pages[0]))
         && read(fd, nw.pool, h.pool_used) == h.pool_used
         && nw_valid(h.npages, h.pool_used);
    close(fd);
    if (ok)
    {
        nw.npages = h.npages;
        nw.pool_used = h.pool_used;
        nw.loaded = true;
    }
    return ok;
}

unsigned int notes_pages_generation(void)
{
    return nw.generation;
}

void notes_where_begin(void)
{
    nw.scraped = file_exists(NOTES_DIR "/" NOTES_MARKER);
    if (nw_stage >= 0)
        close(nw_stage);
    nw_stage = open(NW_STAGE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    nw_staged = nw_staged_pool = 0;
}

static void nw_add(int kind, uint32_t key, uint32_t alt, uint32_t part,
                   uint32_t by, uint64_t dir, const char *path, int rank)
{
    struct notes_page pg;
    size_t size = strlen(path) + 1;

    if (!key || nw_stage < 0 || nw_staged == NW_PAGES_MAX
        || nw_staged_pool + size > NW_POOL_MAX)
        return;
    memset(&pg, 0, sizeof(pg));
    pg.key = key;
    pg.alt = alt;
    pg.part = part;
    pg.by = by;
    pg.dir = dir;
    pg.path = size;
    pg.size = nw_size;
    pg.mtime = nw_mtime;
    pg.kind = kind;
    pg.rank = rank;
    if (write(nw_stage, &pg, sizeof(pg)) != (ssize_t)sizeof(pg)
        || write(nw_stage, path, size) != (ssize_t)size)
    {
        close(nw_stage);            /* a short index would lose pages */
        nw_stage = -1;
        return;
    }
    nw_staged++;
    nw_staged_pool += size;
}

static int nw_by_key(const void *a, const void *b)
{
    const struct notes_page *x = a, *y = b;

    return x->key < y->key ? -1 : x->key > y->key;
}

/* The staging file into the index in memory, sorted; false when it cannot
 * be read whole, or there is no room for it */
static bool nw_unstage(void)
{
    int fd = open(NW_STAGE, O_RDONLY), i;
    bool ok = fd >= 0 && nw_room(nw_staged, nw_staged_pool);

    nw.npages = nw.pool_used = 0;
    for (i = 0; ok && i < nw_staged; i++)
    {
        struct notes_page *pg = &nw.pages[i];
        uint32_t size;

        ok = read(fd, pg, sizeof(*pg)) == (ssize_t)sizeof(*pg)
             && (size = pg->path) > 0
             && size <= (uint32_t)(nw_staged_pool - nw.pool_used)
             && read(fd, nw.pool + nw.pool_used, size) == (ssize_t)size;
        if (!ok)
            break;
        nw.pool[nw.pool_used + size - 1] = '\0';
        pg->path = nw.pool_used;
        nw.pool_used += size;
        nw.npages++;
    }
    if (fd >= 0)
        close(fd);
    if (ok)
        qsort(nw.pages, nw.npages, sizeof(nw.pages[0]), nw_by_key);
    return ok;
}

/* The index's keys of a page in /Notes: `rel` is its path under /Notes,
 * `leaf` its file name and `len` that without the extension */
static void nw_keys(const char *rel, const char *leaf, size_t len,
                    struct notes_page *pg)
{
    const char *slash = strchr(rel, '/');
    size_t flen = slash && slash < leaf ? (size_t)(slash - rel) : 0;
    size_t bare = notes_bare_len(leaf, len);
    uint32_t part = bare < len ? art_name_hash_n(leaf, bare) : 0;

    memset(pg, 0, sizeof(*pg));
    if (flen == 6 && !strncasecmp(rel, "People", 6))
    {
        pg->kind = NOTES_PAGE_PERSON;
        pg->key = art_name_hash_n(leaf, len);
        pg->alt = art_name_alt_hash_n(leaf, len);
        pg->part = part;
    }
    else if (flen == 6 && !strncasecmp(rel, "Albums", 6))
    {
        const char *dash = notes_dash(leaf, len);

        pg->kind = NOTES_PAGE_ALBUM;
        if (dash)
        {
            pg->key = nw_work_key(leaf, dash - leaf, dash + 3,
                                  leaf + len - dash - 3);
            pg->part = art_name_hash_n(dash + 3, leaf + len - dash - 3);
            pg->by = art_name_hash_n(leaf, dash - leaf);
        }
        else
        {
            pg->key = nw_work_key(NULL, 0, leaf, len);
            pg->part = art_name_hash_n(leaf, len);
        }
    }
    else
    {
        pg->kind = NOTES_PAGE_OTHER;
        pg->key = art_name_hash_n(leaf, len);
        pg->alt = art_name_alt_hash_n(leaf, len);
        pg->part = part;
        pg->by = flen ? art_name_hash_n(rel, flen) : 0;
    }
}

static void nw_found_in_notes(const char *path, const char *rel,
                              const char *leaf, size_t len, int rank)
{
    struct notes_page pg;

    nw_keys(rel, leaf, len, &pg);
    nw_add(pg.kind, pg.key, pg.alt, pg.part, pg.by, 0, path, rank);
}

/* bio.* or notes.* beside the music, in the folder `folder` (`flen` bytes
 * of `p`, its last component `name`) */
static void nw_found_beside(const char *path, const char *p, size_t flen,
                            bool bio, int rank)
{
    static char dir[MAX_PATH];
    const char *name = p + flen, *parent;
    uint64_t key;

    while (name > p && name[-1] != '/')
        name--;
    if (name == p + flen)
        return;
    snprintf(dir, sizeof(dir), "%.*s", (int)flen, p);
    key = path_key(dir);
    if (bio)
    {
        nw_add(NOTES_PAGE_PERSON, art_name_hash_n(name, p + flen - name),
               art_name_alt_hash_n(name, p + flen - name), 0, 0, key, path,
               rank);
        return;
    }
    for (parent = name - 1; parent > p && parent[-1] != '/'; parent--)
        ;
    nw_add(NOTES_PAGE_ALBUM,
           nw_work_key(parent, name > parent ? name - 1 - parent : 0,
                       name, p + flen - name),
           0, art_name_hash_n(name, p + flen - name),
           name > parent ? art_name_hash_n(parent, name - 1 - parent) : 0,
           key, path, rank);
}

void notes_where_found(const char *path, uint32_t size, uint32_t mtime)
{
    const char *p = path_key_strip(path), *leaf = strrchr(p, '/'), *dot;
    size_t len;
    int rank;

    nw_size = size;
    nw_mtime = mtime;
    if (!leaf)
        return;
    leaf++;
    dot = strrchr(leaf, '.');
    if (!(rank = notes_ext_rank(dot)) || leaf[0] == '.')
        return;
    len = dot - leaf;
    if (!strncasecmp(p, NOTES_DIR "/", sizeof(NOTES_DIR)))
        nw_found_in_notes(path, p + sizeof(NOTES_DIR), leaf, len, rank);
    else if ((len == 5 && !strncasecmp(leaf, "notes", 5))
             || (len == 3 && !strncasecmp(leaf, "bio", 3)))
        nw_found_beside(path, p, leaf - 1 - p, len == 3, rank);
}

void notes_where_end(bool completed)
{
    struct nw_header h;
    bool staged = nw_stage >= 0;
    bool ok = false;
    int fd;

    if (staged)
        close(nw_stage);
    nw_stage = -1;
    /* An interrupted walk, or one whose staging failed, keeps the last
     * complete index */
    if (!completed || !staged)
    {
        remove(NW_STAGE);
        if (completed)
            debug_log(DEBUG_LOG_TASKS, "notes: %s not written", NW_STAGE);
        return;
    }

    nw.loaded = false;
    nw.generation++;
    if (!nw_unstage())
    {
        remove(NW_STAGE);
        debug_log(DEBUG_LOG_TASKS, "notes: %s not read", NW_STAGE);
        notes_where_load();
        return;
    }
    nw.loaded = true;
    remove(NW_STAGE);

    h.magic = NW_MAGIC;
    h.npages = nw.npages;
    h.pool_used = nw.pool_used;
    fd = open(NW_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        ok = write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h)
            && write(fd, nw.pages, h.npages * sizeof(nw.pages[0]))
               == (ssize_t)(h.npages * sizeof(nw.pages[0]))
            && write(fd, nw.pool, h.pool_used) == h.pool_used;
        close(fd);
        if (ok)
            rename(NW_FILE ".tmp", NW_FILE);
        else
            remove(NW_FILE ".tmp");
    }
    /* Unsaved, the next boot walks again to make it */
    if (!ok)
        debug_log(DEBUG_LOG_TASKS, "notes: %s not written", NW_FILE);
    name_index_pages_changed();
}

int notes_page_count(void)
{
    return nw.loaded && global_settings.liner_notes ? nw.npages : 0;
}

/* ---- a scraper's wiki: the file page ----------------------------------- */

static struct notes_page nw_file;
static char nw_file_path[MAX_PATH];
static bool nw_file_valid;

bool notes_scraped(void)
{
    return nw.scraped && global_settings.liner_notes;
}

void notes_file_name(char *buf, size_t size, const char *name)
{
    size_t n = 0;

    for (; *name && n + 1 < size; name++)
        buf[n++] = strchr("\\/:*?\"<>|", *name) ? '_' : *name;
    while (n && (buf[n - 1] == '.' || buf[n - 1] == ' '))
        n--;
    buf[n] = '\0';
}

/* The page file at `len` bytes of `rel`, its place under /Notes with or
 * without its extension: its path into `path` (.md, else .txt) and its
 * keys into `pg`; false when there is no such file. With `look` false the
 * file is not looked for, and taken to be .md. */
static bool nw_file_look(const char *rel, size_t len, char *path,
                         struct notes_page *pg, bool look)
{
    static char want[MAX_PATH];
    const char *leaf, *dot;
    int rank;

    if (!global_settings.liner_notes
        || len + sizeof(NOTES_DIR) + 5 > sizeof(want))
        return false;
    memcpy(want, rel, len);
    want[len] = '\0';
    leaf = strrchr(want, '/');
    leaf = leaf ? leaf + 1 : want;
    if ((dot = strrchr(leaf, '.')) && notes_ext_rank(dot))
        want[len = dot - want] = '\0';
    if (!*leaf || leaf == want)
        return false;
    for (rank = 2; rank > 0; rank--)
    {
        snprintf(path, MAX_PATH, NOTES_DIR "/%s%s", want,
                 rank == 2 ? ".md" : ".txt");
        if (!look || file_exists(path))
            break;
    }
    if (!rank)
        return false;
    nw_keys(want, leaf, want + len - leaf, pg);
    pg->rank = rank;
    return true;
}

static bool nw_file_find(const char *rel, size_t len, char *path,
                         struct notes_page *pg)
{
    return nw_file_look(rel, len, path, pg, true);
}

bool notes_file_path(const char *rel, size_t len, char *path)
{
    struct notes_page pg;

    return nw_file_find(rel, len, path, &pg);
}

bool notes_file_keys(const char *rel, size_t len, struct notes_page *pg)
{
    static char path[MAX_PATH];

    /* A scraper links only to pages it wrote: a link costs no lookup, and
     * its file is looked for when it is followed */
    return nw_file_look(rel, len, path, pg, !notes_scraped());
}

int notes_page_file(const char *rel, size_t len)
{
    nw_file_valid = nw_file_find(rel, len, nw_file_path, &nw_file);
    return nw_file_valid ? NOTES_PAGE_FILE : -1;
}

const struct notes_page *notes_page_get(int i)
{
    if (i == NOTES_PAGE_FILE)
        return nw_file_valid && global_settings.liner_notes ? &nw_file : NULL;
    return i >= 0 && i < notes_page_count() ? &nw.pages[i] : NULL;
}

const char *notes_page_path(const struct notes_page *pg)
{
    return pg == &nw_file ? nw_file_path : nw.pool + pg->path;
}

int notes_page_first(uint32_t key)
{
    int lo = 0, hi = notes_page_count();

    while (lo < hi)
    {
        int mid = (lo + hi) / 2;

        if (nw.pages[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int notes_page_at(const char *rel, size_t len)
{
    static char want[MAX_PATH];
    const char *leaf, *dot;
    struct notes_page k;
    int i, best = -1, n = notes_page_count();

    if (len >= sizeof(want) - sizeof(NOTES_DIR) - 1)
        return -1;
    memcpy(want, rel, len);
    want[len] = '\0';
    leaf = strrchr(want, '/');
    leaf = leaf ? leaf + 1 : want;
    dot = strrchr(leaf, '.');
    if (notes_ext_rank(dot))
        len = dot - want;
    want[len] = '\0';
    if (!*leaf)
        return -1;
    nw_keys(want, leaf, want + len - leaf, &k);
    for (i = notes_page_first(k.key); i < n && nw.pages[i].key == k.key; i++)
    {
        const struct notes_page *pg = &nw.pages[i];
        const char *p;

        if (pg->kind != k.kind || pg->dir
            || (best >= 0 && !notes_page_better(pg, &nw.pages[best])))
            continue;
        /* The key settles it but for a clash: the path says */
        p = path_key_strip(notes_page_path(pg)) + sizeof(NOTES_DIR);
        dot = strrchr(p, '.');
        if (dot && (size_t)(dot - p) == len && !strncasecmp(p, want, len))
            best = i;
    }
    return best;
}

bool notes_page_better(const struct notes_page *a, const struct notes_page *b)
{
    if (!b)
        return true;
    if (!a->dir != !b->dir)
        return !a->dir;             /* /Notes wins over beside the music */
    return a->rank > b->rank;
}

/* Whether either of `key` and `alt` is either key of the person `pg` */
static bool nw_person_is(const struct notes_page *pg, unsigned int key,
                         unsigned int alt)
{
    return (key && (pg->key == key || pg->alt == key || pg->part == key))
           || (alt && (pg->key == alt || pg->alt == alt));
}

/* The best page in /Notes of `kind` matching `key` or `alt`: for a person
 * either key of its name, its name without the qualifier among them; for an
 * album its artist and title, else its title */
static const struct notes_page *nw_match(int kind, unsigned int key,
                                         unsigned int alt)
{
    const struct notes_page *found = NULL;
    int i, n = notes_page_count();

    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = &nw.pages[i];

        if (pg->kind != kind || pg->dir || !notes_page_better(pg, found))
            continue;
        if (kind == NOTES_PAGE_PERSON ? nw_person_is(pg, key, alt)
                                      : (key && (pg->key == key
                                                 || pg->part == key)))
            found = pg;
    }
    return found;
}

/* ---- the lookups for a track and an album ------------------------------ */

/* The best page beside the music in the first `dirlen` bytes of `dir` */
static const struct notes_page *nw_in_dir(const char *dir, size_t dirlen,
                                          int kind)
{
    static char folder[MAX_PATH];
    const struct notes_page *found = NULL;
    int i, n = notes_page_count();
    uint64_t key;

    snprintf(folder, sizeof(folder), "%.*s", (int)dirlen, dir);
    key = path_key(folder);
    for (i = 0; i < n; i++)
        if (nw.pages[i].dir == key && nw.pages[i].kind == kind
            && notes_page_better(&nw.pages[i], found))
            found = &nw.pages[i];
    return found;
}

void notes_page_name(const struct notes_page *pg, char *name, size_t size)
{
    const char *path = notes_page_path(pg), *end = strrchr(path, '/');
    const char *start;

    if (pg->dir)
    {
        for (start = end; start > path && start[-1] != '/'; start--)
            ;
        snprintf(name, size, "%.*s", (int)(end - start), start);
        return;
    }
    start = end + 1;
    end = strrchr(start, '.');
    snprintf(name, size, "%.*s", (int)(end - start), start);
}

/* The files beside the track at `track`: the album's notes in its folder,
 * the artist's bio in the folder above when that is named for one of the
 * keys */
static void notes_beside(const char *track, unsigned int k1, unsigned int a1,
                         unsigned int k2, unsigned int a2,
                         struct notes_pages *p)
{
    const char *album_end = track ? strrchr(track, '/') : NULL;
    const char *artist_end;
    const struct notes_page *pg;

    if (!album_end || album_end == track)
        return;
    if (!(p->have & NOTES_ALBUM)
        && (pg = nw_in_dir(track, album_end - track, NOTES_PAGE_ALBUM)))
    {
        strlcpy(p->album, notes_page_path(pg), MAX_PATH);
        p->have |= NOTES_ALBUM;
    }

    for (artist_end = album_end - 1; artist_end > track && *artist_end != '/';
         artist_end--)
        ;
    if (p->have & NOTES_BIO || artist_end <= track)
        return;
    pg = nw_in_dir(track, artist_end - track, NOTES_PAGE_PERSON);
    if (!pg || !(nw_person_is(pg, k1, a1) || nw_person_is(pg, k2, a2)))
        return;
    strlcpy(p->bio, notes_page_path(pg), MAX_PATH);
    notes_page_name(pg, p->person, MAX_PATH);
    p->have |= NOTES_BIO;
}

/* A scraper's pages of the album artist (or artist) and the album, by the
 * files their names give */
static void nw_find_files(const char *albumartist, const char *artist,
                          const char *album, struct notes_pages *p)
{
    static char name[MAX_PATH], rel[MAX_PATH];
    struct notes_page k;
    const char *who[2] = { albumartist, artist };
    int i;

    for (i = 0; i < 2 && !(p->have & NOTES_BIO); i++)
    {
        if (!who[i] || !who[i][0])
            continue;
        notes_file_name(name, sizeof(name), who[i]);
        snprintf(rel, sizeof(rel), "People/%s", name);
        if (nw_file_find(rel, strlen(rel), p->bio, &k))
        {
            strlcpy(p->person, name, MAX_PATH);
            p->have |= NOTES_BIO;
        }
    }
    if (album && album[0] && (who[0] && who[0][0] ? who[0] : who[1]))
    {
        const char *by = who[0] && who[0][0] ? who[0] : who[1];
        size_t n;

        notes_file_name(name, sizeof(name), by);
        n = snprintf(rel, sizeof(rel), "Albums/%s - ", name);
        notes_file_name(rel + MIN(n, sizeof(rel) - 1), sizeof(rel) - n,
                        album);
        if (nw_file_find(rel, strlen(rel), p->album, &k))
            p->have |= NOTES_ALBUM;
    }
}

int notes_find_names(const char *albumartist, const char *artist,
                     const char *album, const char *track,
                     struct notes_pages *p)
{
    const char *by = albumartist && albumartist[0] ? albumartist : artist;
    unsigned int k1 = art_name_hash(albumartist), a1 = art_name_alt_hash(albumartist);
    unsigned int k2 = art_name_hash(artist), a2 = art_name_alt_hash(artist);
    const struct notes_page *pg;

    p->have = 0;
    p->bio[0] = p->person[0] = p->album[0] = '\0';
    if (!global_settings.liner_notes)
        return 0;
    if (notes_scraped())
        nw_find_files(albumartist, artist, album, p);

    /* The album artist's bio, else the track artist's */
    if (p->have & NOTES_BIO)
        ;
    else if ((pg = nw_match(NOTES_PAGE_PERSON, k1, a1))
        || (k2 != k1 && (pg = nw_match(NOTES_PAGE_PERSON, k2, a2))))
    {
        strlcpy(p->bio, notes_page_path(pg), MAX_PATH);
        notes_page_name(pg, p->person, MAX_PATH);
        p->have |= NOTES_BIO;
    }

    /* As /Artwork/Covers is matched: artist and album, else the album */
    if (!(p->have & NOTES_ALBUM) && album && album[0]
        && ((pg = nw_match(NOTES_PAGE_ALBUM, art_name_work_hash(by, album), 0))
            || (pg = nw_match(NOTES_PAGE_ALBUM, art_name_hash(album), 0))))
    {
        strlcpy(p->album, notes_page_path(pg), MAX_PATH);
        p->have |= NOTES_ALBUM;
    }

    if (p->have != (NOTES_BIO | NOTES_ALBUM))
        notes_beside(track, k1, a1, k2, a2, p);
    return p->have;
}

bool notes_album_read(struct tagcache_search *tcs, struct notes_album *a)
{
    long year;

    if (!tagcache_get_next(tcs, a->path, sizeof(a->path)))
        return false;
    if (!tagcache_retrieve_two(tcs, tcs->idx_id,
                               tag_albumartist, a->albumartist,
                               sizeof(a->albumartist),
                               tag_album, a->album, sizeof(a->album)))
        a->albumartist[0] = a->album[0] = '\0';
    if (!tagcache_retrieve(tcs, tcs->idx_id, tag_artist, a->artist,
                           sizeof(a->artist)))
        a->artist[0] = '\0';
    /* <Untagged> names nothing */
    if (!strcmp(a->albumartist, UNTAGGED))
        a->albumartist[0] = '\0';
    if (!strcmp(a->album, UNTAGGED))
        a->album[0] = '\0';
    if (!strcmp(a->artist, UNTAGGED))
        a->artist[0] = '\0';
    year = tagcache_get_numeric(tcs, tag_year);
    if (year > 0)
        snprintf(a->year, sizeof(a->year), "%ld", year);
    else
        a->year[0] = '\0';
    return true;
}

bool notes_album_of(long album_seek, long artist_seek, struct notes_album *a)
{
    struct tagcache_search tcs;
    bool found;

    if (album_seek < 0 || !global_settings.liner_notes
        || !tagcache_is_usable()
        || !tagcache_search(&tcs, tag_filename))
        return false;
    tagcache_search_add_filter(&tcs, tag_album, album_seek);
    if (notes_is_artist(artist_seek))
        tagcache_search_add_filter(&tcs, tag_albumartist, artist_seek);
    found = notes_album_read(&tcs, a);
    tagcache_search_finish(&tcs);
    return found;
}

int notes_find_album(const struct notes_album *a, struct notes_pages *p)
{
    return notes_find_names(a->albumartist, a->artist, a->album, a->path, p);
}

bool notes_has_bio(const char *name)
{
    static struct notes_pages p;
    unsigned int key, alt;
    int i, n = notes_page_count();

    if (!name)
        return false;
    if (notes_scraped())
    {
        p.have = 0;
        nw_find_files(name, NULL, NULL, &p);
        if (p.have & NOTES_BIO)
            return true;
    }
    key = art_name_hash(name);
    alt = art_name_alt_hash(name);
    for (i = 0; i < n; i++)
        if (nw.pages[i].kind == NOTES_PAGE_PERSON
            && nw_person_is(&nw.pages[i], key, alt))
            return true;
    return false;
}

/* ---- a page's front matter --------------------------------------------- */

bool notes_yaml_line(const char *s)
{
    int n;

    if (*s == '\n' || *s == '\r' || !*s || strchr(" \t-#.", *s))
        return true;
    for (n = 0; n < 32; n++, s++)
    {
        unsigned char c = *s;

        if (!((c >= '0' && c <= '9')
              || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
              || c >= 0x80 || (c && strchr("_- .\"'", c))))
            break;
    }
    return *s == ':';
}

size_t notes_front_value(const char *text, const char *key, char *buf,
                         size_t size)
{
    size_t klen = strlen(key);
    const char *s = text;

    buf[0] = '\0';
    if (!strncmp(s, BOM_UTF_8, BOM_UTF_8_SIZE))
        s += BOM_UTF_8_SIZE;
    if (strncmp(s, "---\n", 4) && strncmp(s, "---\r\n", 5))
        return 0;
    while ((s = strchr(s, '\n')) && *++s)
    {
        const char *v = s + klen + 1, *end;
        size_t n;

        if (!strncmp(s, "---", 3) || !strncmp(s, "...", 3)
            || !notes_yaml_line(s))
            break;
        if (strncasecmp(s, key, klen) || s[klen] != ':')
            continue;
        while (*v == ' ' || *v == '\t')
            v++;
        for (end = v; *end && *end != '\n' && *end != '\r'; end++)
            ;
        while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (end - v >= 2 && (*v == '"' || *v == '\'') && end[-1] == *v)
        {
            v++;
            end--;
        }
        n = MIN((size_t)(end - v), size - 1);
        memcpy(buf, v, n);
        buf[n] = '\0';
        return n;
    }
    return 0;
}

