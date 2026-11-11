/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-window.h"

#include "ea-async.h"
#include "ea-scan-page.h"
#include "ea-shortcuts-page.h"

struct _EaWindow {
    AdwApplicationWindow parent_instance;

    AdwToastOverlay *toasts;
    AdwViewStack *stack;
    GListStore *entries;
    GCancellable *entries_cancel;
};

G_DEFINE_FINAL_TYPE(EaWindow, ea_window, ADW_TYPE_APPLICATION_WINDOW)

static EaWindow *main_window; /* single window app */

EaWindow *ea_window_from_widget(GtkWidget *w)
{
    GtkRoot *root = w ? gtk_widget_get_root(w) : NULL;
    if (EA_IS_WINDOW(root))
        return EA_WINDOW(root);
    return main_window;
}

void ea_window_toast(EaWindow *self, const char *msg)
{
    AdwToast *t = adw_toast_new(msg);
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(self->toasts, t);
}

GListModel *ea_window_get_entries(EaWindow *self)
{
    return G_LIST_MODEL(self->entries);
}

EaEntry *ea_window_find_entry_for_target(EaWindow *self, const char *real_path)
{
    if (!real_path)
        return NULL;
    EaEntry *found = NULL;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->entries));
    for (guint i = 0; i < n; i++) {
        EaEntry *e = g_list_model_get_item(G_LIST_MODEL(self->entries), i);
        if (g_strcmp0(ea_entry_get_target_path(e), real_path) == 0 &&
            (!found || (ea_entry_get_managed(e) && !ea_entry_get_managed(found)))) {
            found = e;
        }
        g_object_unref(e); /* store keeps it alive */
    }
    return found;
}

static void on_entries_loaded(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)src;
    GError *err = NULL;
    GListStore *store = ea_list_entries_finish(res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return;
    }
    EaWindow *self = EA_WINDOW(ud);
    if (!store) {
        ea_window_toast(self, err ? err->message : "Cannot read shortcuts");
        g_clear_error(&err);
        return;
    }
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < n; i++)
        g_ptr_array_add(items, g_list_model_get_item(G_LIST_MODEL(store), i));
    g_list_store_splice(self->entries, 0,
                        g_list_model_get_n_items(G_LIST_MODEL(self->entries)),
                        items->pdata, items->len);
    g_ptr_array_unref(items);
    g_object_unref(store);
}

void ea_window_reload_entries(EaWindow *self)
{
    if (self->entries_cancel) {
        g_cancellable_cancel(self->entries_cancel);
        g_object_unref(self->entries_cancel);
    }
    self->entries_cancel = g_cancellable_new();
    ea_list_entries_async(self->entries_cancel, on_entries_loaded, self);
}

/* ---- actions ----------------------------------------------------------- */

static void on_refresh(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    ea_window_reload_entries(EA_WINDOW(ud));
}

static void on_about(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    AdwDialog *about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "ExtApp");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about),
                                          "org.codeberg.giocal88.ExtApp");
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), EXTAPP_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Giorgio Caldini");
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "Manage launcher shortcuts for AppImages and other loose executables.");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_dialog_present(about, GTK_WIDGET(ud));
}

static const GActionEntry win_actions[] = {
    {.name = "refresh", .activate = on_refresh},
    {.name = "about", .activate = on_about},
};

/* ---- lifecycle --------------------------------------------------------- */

static void ea_window_dispose(GObject *obj)
{
    EaWindow *self = EA_WINDOW(obj);
    if (self->entries_cancel)
        g_cancellable_cancel(self->entries_cancel);
    g_clear_object(&self->entries_cancel);
    if (main_window == self)
        main_window = NULL;
    G_OBJECT_CLASS(ea_window_parent_class)->dispose(obj);
}

static void ea_window_finalize(GObject *obj)
{
    g_clear_object(&EA_WINDOW(obj)->entries);
    G_OBJECT_CLASS(ea_window_parent_class)->finalize(obj);
}

static void ea_window_class_init(EaWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = ea_window_dispose;
    G_OBJECT_CLASS(klass)->finalize = ea_window_finalize;
}

static void ea_window_init(EaWindow *self)
{
    main_window = self;
    self->entries = g_list_store_new(EA_TYPE_ENTRY);

    gtk_window_set_title(GTK_WINDOW(self), "ExtApp");
    gtk_window_set_default_size(GTK_WINDOW(self), 980, 680);
    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);

    self->stack = ADW_VIEW_STACK(adw_view_stack_new());
    AdwViewStackPage *p;
    p = adw_view_stack_add_titled(self->stack, GTK_WIDGET(ea_scan_page_new(self)),
                                  "scan", "Find Programs");
    adw_view_stack_page_set_icon_name(p, "system-search-symbolic");
    p = adw_view_stack_add_titled(self->stack, GTK_WIDGET(ea_shortcuts_page_new(self)),
                                  "shortcuts", "Shortcuts");
    adw_view_stack_page_set_icon_name(p, "view-grid-symbolic");

    GtkWidget *hb = adw_header_bar_new();
    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), self->stack);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher),
                                 ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(hb), switcher);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Refresh Shortcuts", "win.refresh");
    g_menu_append(menu, "About ExtApp", "win.about");
    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_btn), TRUE);
    gtk_widget_set_tooltip_text(menu_btn, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), menu_btn);
    g_object_unref(menu);

    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->stack));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), tv);

    ea_window_reload_entries(self);
}

EaWindow *ea_window_new(AdwApplication *app)
{
    return g_object_new(EA_TYPE_WINDOW, "application", app, NULL);
}
