/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings menu for the artist bio screen (apps-ipod/viewers/artist_bio).
 * The same menu is reached from Settings and, in the screen, by holding
 * Menu.
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

MENUITEM_SETTING(artist_bio_background,
                 &global_settings.artist_bio_background, background_callback);
MENUITEM_FUNCTION(artist_bio_custom_color_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_CAROUSEL_CUSTOM_COLOR), pick_custom_colour,
                  custom_colour_callback, Icon_NOICON);
MENUITEM_SETTING(artist_bio_banner, &global_settings.artist_bio_banner, NULL);

MAKE_MENU(artist_bio_menu, ID2P(LANG_BIO_NOTES_MENU), NULL, Icon_NOICON,
          &artist_bio_background, &artist_bio_custom_color_item,
          &artist_bio_banner);
