/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The artist bio and liner notes screen: a banner, a name, a line of facts
 * and the text, for the playing artist from /Artwork/Bios or the playing
 * album from /Artwork/Notes, scrolled with the wheel.
 ****************************************************************************/

#ifndef _ARTIST_BIO_H
#define _ARTIST_BIO_H

/* global_settings.artist_bio_background */
#define ARTIST_BIO_BG_THEME  0   /* the theme's colours, album art and all */
#define ARTIST_BIO_BG_CUSTOM 1   /* artist_bio_custom_color, text to suit */
#define ARTIST_BIO_BG_BLURRED 2  /* the banner, blurred under white text */
#define ARTIST_BIO_BG_BLURRED_DARK 3 /* the same, taken darker */

/* Where the bios are: <name>.txt, and <name>.jpg beside it for the banner */
#define ARTIST_BIO_DIR "/Artwork/Bios"

/* Where the liner notes are: "<album artist> - <album>.txt", and .jpg */
#define LINER_NOTES_DIR "/Artwork/Notes"

/* Show the bio of the playing track's artist, returning a GO_TO_* code when
 * the user leaves. Opens whatever is playing, and says so when there is no
 * bio to show. */
int artist_bio(void);

/* The same for the artist called `name`, whatever is playing */
int artist_bio_of(const char *name);

/* The playing album's liner notes, and the same for any album */
int liner_notes(void);
int liner_notes_of(const char *artist, const char *album);

#endif /* _ARTIST_BIO_H */
