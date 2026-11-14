# Safe database rebuild

An internals note for whoever reviews or maintains `apps-ipod/database/tagcache.c`.

## What it does

A Rebuild keeps the old database on the disk until the new one is whole. If
the rebuild is cut short, the old database is put back, so the Music menus
keep working. If it finishes, the old files are deleted.

This covers every way a rebuild starts:

- **Settings > Library > Maintenance > Rebuild Database**.
- The automatic rebuild the Music menus start when the database isn't usable.
- `database_pb.exe --rebuild` on a computer.

## Why it's needed

A Rebuild used to delete the database before building the new one, in
`remove_files()`. An Update was already safe, because its merge writes `.new`
files and only swaps them in once complete (`database_swap.tcd`). A Rebuild had
nothing to fall back on.

So a rebuild that didn't finish left the player with no database at all. Any of
these would do it:

- connecting USB (the commit stands down for the host),
- a restart (a new firmware or database from the computer, or a battery cut-off),
- the player turning off.

The Music menus then show **Please reboot to enable**. Rebooting doesn't help,
because there is no main file left and nothing waiting to be committed. The
automatic rebuild from the Music menus can then be cut off the same way.

On an iPod Classic 6G running the 13 November 2026 release, the database
folder was left like this:

```
database_0.tcd ... database_5.tcd     the rebuild's first tag files
database_6/7/8/12.tcd.new             a commit's merge, cut short
database_tmp.tcd                      12 bytes: an empty header
(no database_idx.tcd)                 the master, gone
```

All of these had the player's own clock in their file times, so the player
wrote them, not a desktop tool. Only rebuilding from the computer recovered
the library.

## How it works

`database_rebuild.tcd` marks a rebuild in progress:

1. **Starting** (`remove_files()`): the marker goes down first. Then the master
   and each string tag's file are renamed with `.old` added, instead of deleted.
   If the marker can't be written, the files are deleted as before.
2. **Building:** unchanged. The scan writes `database_tmp.tcd` and the commit
   builds a new database from nothing, as it always did after a rebuild.
3. **Settling** (`rebuild_settle()`): this runs after the rebuild's build, at
   boot after any leftover commit, and in the desktop tool.
   - If a temp file is still waiting, its commit has yet to run, so nothing is
     decided.
   - If a master with entries exists, the rebuild finished: the marker is
     removed, then the `.old` files.
   - Otherwise the rebuild was cut short: any half-made database is discarded,
     the `.old` files are renamed back, and the marker is removed.
   - Without a marker, any `.old` files left over are deleted.

The marker is removed before the `.old` files are deleted. A cut between the
two leaves only stray `.old` files, which the next settle deletes.

The Q_REBUILD case reads the headers again when the old database comes back,
so the restored database is usable straight away rather than after a restart.

## What changes for the player's owner

- An interrupted rebuild leaves the library exactly as it was before the
  rebuild started.
- While a rebuild runs, the old database takes its usual space a second time,
  as `.old` files.
- A rebuild that finds no music at all (an empty or wrong music folder) keeps
  the old database rather than replacing it with an empty one. The next Update
  removes entries whose files are really gone.

## Tested

The tool was built natively against a copy of a simulator disk, with 2 tracks:

| Case | Result |
|---|---|
| `--rebuild` | New database, no marker, no `.old` files |
| Marker down, every file renamed to `.old`, a stray `.new` file and one new tag file, no master | An Update put the old database back (2 tracks, none new) and deleted the stray files |
| Stray `.old` files, no marker | Deleted |

The firmware builds for the iPod Classic 6G without warnings in
`tagcache.c`.

## Files

- `apps-ipod/database/tagcache.c`: `remove_files()` sets the old files aside;
  added `rebuild_settle()` and the helpers it uses; settle is called at boot,
  after a Q_REBUILD, and twice in `tagcache_tool_run()`.
