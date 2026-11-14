# Artwork guide

The README gives the simplest layout: a `folder.jpg` in each album's, book's and show's
folder, and one in each artist's or author's folder. This guide covers everything else
PodBox finds art from, for a library laid out another way or art you would rather keep
apart from your music. The same rules apply to music, audiobooks, books and podcasts.

After adding or moving art, run `Settings > Library > Maintenance > Update Art Cache`.

## One folder for all your art

An `Artwork` folder at the root of the player holds covers and pictures by name, and what
is in it wins over anything beside your files:

```text
Artwork
├── Covers
│   ├── Radiohead - OK Computer.jpg
│   ├── Dune.jpg
│   └── Show Name.jpg
└── People
    ├── Radiohead.jpg
    └── Frank Herbert.jpg
```

- In `Covers`, name a cover `Artist - Title` or just `Title`. For a book the artist is its
  author, and for a podcast its host. Use the artist when two works share a title.
- In `People`, name a picture after the artist or author.

`Files > Pictures` does not list the `Artwork` folder.

## How names are matched

Case, accents, punctuation, spaces and a leading "The" are ignored, `&` matches "and", and
"Herbert, Frank" matches "Frank Herbert". So `NERD.jpg` is the picture for N\*E\*R\*D and
N.E.R.D alike, `ACDC.jpg` for AC/DC, and a folder called `Beatles` is The Beatles'. This
applies to the `Artwork` files and to folder names alike.

An album's artist is its album artist tag, or its artist when there is none. Someone who is
both an artist and an author has one picture.

## Where a cover comes from

The first of:

1. the `Artwork/Covers` file named for it
2. for a book in one file, an image beside it with the book's name: `Dune Messiah.jpg` for
   `Dune Messiah.m4b`
3. `cover.jpg` or `folder.jpg` in its folder, and the cover embedded in its files

At step 3 the image file is used first and the embedded cover when there is none.
`Settings > Library > Art Cache > Album Art Source` changes that order, or limits it to one
kind; steps 1 and 2 are used whatever it says. An album in disc folders (`CD1`, `CD2`) can
keep its image in the folder above them. The README says which kinds of image work.

A book's folder gives it a cover only when the book has the folder to itself. So with several
books loose in one folder, name each one's image after the book, as in step 2. A folder named
after the book's author is the author's, and its image is their picture, not the book's cover.

## Where a picture of a person comes from

Pictures only ever come from image files, never from tags. The first of:

1. the `Artwork/People` file named for them
2. `cover.jpg` or `folder.jpg` in a folder named after them: the tracks' own folder or up to
   three above it, so `Radiohead/folder.jpg` is found with the albums in `Radiohead` or in
   `Radiohead/Albums`
3. `cover.jpg` or `folder.jpg` in the folder above the album's, or the one above that. For an
   ebook, the book's own folder and the one above.

Step 3 is for an artist whose folder is not named the way the tags are, such as albums by
Prince and by Prince & The Revolution, all in `Prince`. It skips a folder that holds the
folders of more than one artist, such as `Music` or a genre folder, so an image there is
nobody's picture.

## When a cover is missing

`Settings > Library > Art Cache` has `Missing Album Artwork` and `Missing Artist Portraits`,
which list what has none. `Settings > System > Debug Logs > Album Art Log`
writes the reason for each to `art.log`.

## Now Playing

With `Settings > Appearance > Now Playing Screen > Album Art` set to `Prefer Image File`, Now
Playing reads the image itself rather than the cached copy: the `Artwork/Covers` file, an
image named after the track, or `cover.jpg` or `folder.jpg` in the track's folder. An
audiobook in a folder named after its author skips that folder's image, as above. Several
books loose in one folder all show the folder's image here, so name each one's image after
the book.

## Artist bios

`Artist Bio` in the now-playing menu (hold Select) shows a page about the playing artist:
a banner photo, the name, their dates and a biography. In Music, the menu of an artist, or
of one of their albums, has it too. Turn the wheel to scroll, and press
Menu to go back; hold Menu for its settings. The page comes from two files in
`Artwork/Bios`, named after the artist the same way as `People`:

```text
Artwork
└── Bios
    ├── Radiohead.txt
    └── Radiohead.jpg
```

The `.txt` is UTF-8 text: the name on the first line, a dates line on the second (it can be
left empty), a blank line, then the biography, a paragraph to a line. The `.jpg` is the
banner, 320 pixels wide and 160 tall; its lower part fades into the background under the
name. Without it the page starts with the name. The album artist's bio is shown, or the
artist's when the album artist has none. The `ipod-artist-photos` tool writes both files.

## Liner notes

`Liner Notes`, next to `Artist Bio` in the now-playing menu and in the menu of an album in
Music, is the same kind of page for the album: its cover as the banner, the title, a line
of facts and its notes. Opened from the now-playing menu, either page swaps to the other
with Right: the artist's bio and the playing album's notes. Right does nothing when the
track has no album. The files are in `Artwork/Notes`, named `<Album Artist> - <Album>`:

```text
Artwork
└── Notes
    ├── Radiohead - OK Computer.txt
    └── Radiohead - OK Computer.jpg
```

The `.txt` is laid out as for a bio. A paragraph starting with `# ` is drawn small and grey,
for the critics' scores or the source; one followed by an ordinary paragraph is
that paragraph's heading, drawn in bold. Both pages share their settings, under
Settings > Appearance > Viewers > Artist Bio & Liner Notes.
