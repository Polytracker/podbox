/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings menu for the Liner Notes pages (apps-ipod/viewers/artist_bio).
 * The same menu is reached from Settings and, in the screen, by holding
 * Menu. A row shows only while what it governs is on.
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "lang.h"
#include "input/action.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "widgets/color_picker.h"
#include "draw/icon_bitmaps.h"
#include "root_menu.h"          /* MENU_ATTACHED_USB */
#include "viewers/artist_bio/artist_bio.h"

/* Custom needs a colour, so moving to it opens the picker, and the Custom
 * Colour row under it, shown only then, changes the colour afterwards. */
static int pick_custom_colour(void)
{
    int old = global_settings.artist_bio_custom_color;

    if (set_color(str(LANG_CAROUSEL_CUSTOM_COLOR),
                  (unsigned *)&global_settings.artist_bio_custom_color,
                  (unsigned)-1))
        return MENU_ATTACHED_USB;

    if (global_settings.artist_bio_custom_color != old)
    {
        settings_mark_user_tweak(
            find_setting(&global_settings.artist_bio_custom_color));
        settings_save();
    }
    return 0;
}

static int background_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    static int was;

    (void)this_item;
    (void)this_list;

    if (action == ACTION_ENTER_MENUITEM)
        was = global_settings.artist_bio_background;
    else if (action == ACTION_EXIT_MENUITEM
             && global_settings.artist_bio_background != was)
    {
        /* the theme chose it, so a change is remembered for this theme */
        settings_mark_user_tweak(
            find_setting(&global_settings.artist_bio_background));
        if (global_settings.artist_bio_background == ARTIST_BIO_BG_CUSTOM)
            pick_custom_colour();
    }
    return action;
}

static int custom_colour_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;

    if (action == ACTION_REQUEST_MENUITEM
        && global_settings.artist_bio_background != ARTIST_BIO_BG_CUSTOM)
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* Shown while the main switch is on and, for the rows after the pages,
 * while a page they apply to is on */
static int shown_callback(int action, const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list)
{
    const struct user_settings *gs = &global_settings;
    const void *v;
    bool show = gs->liner_notes;

    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    v = this_item->variable;
    if (v == &gs->artist_bio_banner)
        show = show && (gs->liner_notes_bio || gs->liner_notes_album);
    else if (v == &gs->liner_notes_trim)
        show = show && (gs->liner_notes_track || gs->liner_notes_credits);
    else if (v == &gs->liner_notes_icons)
        show = show && gs->liner_notes_credits;
    else if (v == &gs->artist_bio_background)
        show = show && (gs->liner_notes_bio || gs->liner_notes_album
                        || gs->liner_notes_track || gs->liner_notes_credits);
    return show ? action : ACTION_EXIT_MENUITEM;
}

static int background_shown_callback(int action,
                                     const struct menu_item_ex *this_item,
                                     struct gui_synclist *this_list)
{
    if (action == ACTION_REQUEST_MENUITEM)
        return shown_callback(action, this_item, this_list);
    return background_callback(action, this_item, this_list);
}

static int custom_colour_shown_callback(int action,
                                        const struct menu_item_ex *this_item,
                                        struct gui_synclist *this_list)
{
    if (action == ACTION_REQUEST_MENUITEM && !global_settings.liner_notes)
        return ACTION_EXIT_MENUITEM;
    return custom_colour_callback(action, this_item, this_list);
}

MENUITEM_SETTING(liner_notes_item, &global_settings.liner_notes, NULL);
MENUITEM_SETTING(liner_notes_bio, &global_settings.liner_notes_bio,
                 shown_callback);
MENUITEM_SETTING(liner_notes_album, &global_settings.liner_notes_album,
                 shown_callback);
MENUITEM_SETTING(liner_notes_track, &global_settings.liner_notes_track,
                 shown_callback);
MENUITEM_SETTING(liner_notes_credits, &global_settings.liner_notes_credits,
                 shown_callback);
MENUITEM_SETTING(artist_bio_background,
                 &global_settings.artist_bio_background,
                 background_shown_callback);
MENUITEM_FUNCTION(artist_bio_custom_color_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_CAROUSEL_CUSTOM_COLOR), pick_custom_colour,
                  custom_colour_shown_callback, Icon_NOICON);
MENUITEM_SETTING(artist_bio_banner, &global_settings.artist_bio_banner,
                 shown_callback);
MENUITEM_SETTING(liner_notes_names_scroll,
                 &global_settings.liner_notes_names_scroll, shown_callback);
MENUITEM_SETTING(liner_notes_trim, &global_settings.liner_notes_trim,
                 shown_callback);
MENUITEM_SETTING(liner_notes_icons, &global_settings.liner_notes_icons,
                 shown_callback);

MAKE_MENU(artist_bio_menu, ID2P(LANG_BIO_NOTES_MENU), NULL, Icon_NOICON,
          &liner_notes_item, &liner_notes_bio, &liner_notes_album,
          &liner_notes_track, &liner_notes_credits, &artist_bio_background,
          &artist_bio_custom_color_item, &artist_bio_banner,
          &liner_notes_names_scroll,
          &liner_notes_trim, &liner_notes_icons);
