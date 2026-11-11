/* ExtApp — GTK4/libadwaita frontend entry point.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <adwaita.h>

#include "ea-async.h"
#include "ea-window.h"

#define APP_ID "org.codeberg.giocal88.ExtApp"

static const char *app_css =
    ".ea-pill { font-size: 0.8em; font-weight: bold; padding: 2px 8px;"
    "  border-radius: 999px; background: alpha(currentColor, 0.1); }"
    ".ea-pill.accent { color: @accent_color; background: alpha(@accent_bg_color, 0.15); }"
    ".ea-pill.success { color: @success_color; background: alpha(@success_bg_color, 0.15); }"
    ".ea-pill.warning { color: @warning_color; background: alpha(@warning_bg_color, 0.2); }"
    ".ea-pill.error { color: @error_color; background: alpha(@error_bg_color, 0.15); }";

static void on_startup(GApplication *app, gpointer ud)
{
    (void)ud;
    ea_backend_init();

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, app_css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Installed builds get the icon from the hicolor theme; an uninstalled
     * build (run from the build tree) falls back to the source tree. */
#ifdef EXTAPP_SOURCE_ICONS_DIR
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (!gtk_icon_theme_has_icon(theme, APP_ID) &&
        g_file_test(EXTAPP_SOURCE_ICONS_DIR, G_FILE_TEST_IS_DIR))
        gtk_icon_theme_add_search_path(theme, EXTAPP_SOURCE_ICONS_DIR);
#endif
    gtk_window_set_default_icon_name(APP_ID);

    static const char *quit_accels[] = {"<Control>q", NULL};
    static const char *refresh_accels[] = {"F5", "<Control>r", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.refresh",
                                          refresh_accels);
}

static void on_quit(GSimpleAction *a, GVariant *p, gpointer app)
{
    (void)a; (void)p;
    g_application_quit(G_APPLICATION(app));
}

static void on_activate(GApplication *app, gpointer ud)
{
    (void)ud;
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (!win)
        win = GTK_WINDOW(ea_window_new(ADW_APPLICATION(app)));
    gtk_window_present(win);
}

int main(int argc, char **argv)
{
    AdwApplication *app = adw_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_set_application_name("ExtApp");

    static const GActionEntry app_actions[] = {{.name = "quit", .activate = on_quit}};
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
                                    G_N_ELEMENTS(app_actions), app);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    ea_backend_shutdown();
    return rc;
}
