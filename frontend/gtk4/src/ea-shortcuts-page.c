/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-shortcuts-page.h"

#include "ea-async.h"
#include "ea-objects.h"
#include "ea-shortcut-dialog.h"
#include "ea-util.h"

#include <stdlib.h>

struct _EaShortcutsPage {
    AdwBin parent_instance;

    EaWindow *window; /* not owned */
    GtkSearchEntry *search;
    GtkToggleButton *only_ours;
    GtkCustomFilter *filter;
    GtkFilterListModel *filtered;
    GtkListBox *list;
    GtkStack *stack;
};

G_DEFINE_FINAL_TYPE(EaShortcutsPage, ea_shortcuts_page, ADW_TYPE_BIN)

/* ---- filtering --------------------------------------------------------- */

static gboolean contains_ci(const char *hay, const char *needle_folded)
{
    if (!hay)
        return FALSE;
    g_autofree char *h = g_utf8_casefold(hay, -1);
    return strstr(h, needle_folded) != NULL;
}

static gboolean filter_func(gpointer item, gpointer ud)
{
    EaShortcutsPage *self = ud;
    EaEntry *e = item;
    if (gtk_toggle_button_get_active(self->only_ours) && !ea_entry_get_managed(e))
        return FALSE;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(self->search));
    if (!q || !*q)
        return TRUE;
    g_autofree char *fq = g_utf8_casefold(q, -1);
    return contains_ci(ea_entry_get_name(e), fq) ||
           contains_ci(ea_entry_get_exec(e), fq) ||
           contains_ci(ea_entry_get_id(e), fq);
}

static void refilter(EaShortcutsPage *self)
{
    gtk_filter_changed(GTK_FILTER(self->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void update_stack(EaShortcutsPage *self)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->filtered));
    gtk_stack_set_visible_child_name(self->stack, n ? "list" : "empty");
}

static void on_filtered_changed(GListModel *m, guint p, guint r, guint a, gpointer ud)
{
    (void)m; (void)p; (void)r; (void)a;
    update_stack(EA_SHORTCUTS_PAGE(ud));
}

/* ---- actions ----------------------------------------------------------- */

static gboolean is_our_icon(const char *icon)
{
    const char *dir = extapp_context_icons_dir(ea_backend());
    return icon && dir && g_str_has_prefix(icon, dir) && icon[strlen(dir)] == '/';
}

static void on_remove_response(AdwAlertDialog *d, const char *response, gpointer ud)
{
    EaShortcutsPage *self = EA_SHORTCUTS_PAGE(ud);
    if (g_strcmp0(response, "remove") != 0)
        return;
    EaEntry *e = g_object_get_data(G_OBJECT(d), "entry");
    GtkCheckButton *icon_check = g_object_get_data(G_OBJECT(d), "icon-check");

    unsigned flags = 0;
    if (!ea_entry_get_managed(e))
        flags |= EXTAPP_REMOVE_ALLOW_UNMANAGED;
    if (icon_check && gtk_check_button_get_active(icon_check))
        flags |= EXTAPP_REMOVE_DELETE_ICON;

    char *err = NULL;
    ExtAppStatus st = extapp_entry_remove(ea_backend(), ea_entry_get_file_path(e),
                                          flags, &err);
    if (st == EXTAPP_OK) {
        g_autofree char *msg = g_strdup_printf("“%s” removed", ea_entry_get_name(e));
        ea_window_toast(self->window, msg);
    } else {
        ea_window_toast(self->window, err ? err : extapp_status_str(st));
    }
    free(err);
    ea_window_reload_entries(self->window);
}

static void on_remove_clicked(GtkButton *btn, gpointer ud)
{
    EaShortcutsPage *self = ud;
    EaEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    g_autofree char *heading = g_strdup_printf("Remove “%s”?", ea_entry_get_name(e));
    const char *body = ea_entry_get_managed(e)
        ? "The shortcut will disappear from the app launcher. The program "
          "itself is not deleted."
        : "This shortcut was not created by ExtApp. It may belong to another "
          "application, or override a system shortcut with the same name. "
          "The program itself is not deleted.";

    AdwDialog *d = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel",
                                   "remove", "Remove", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
    g_object_set_data_full(G_OBJECT(d), "entry", g_object_ref(e), g_object_unref);

    if (is_our_icon(ea_entry_get_icon(e))) {
        GtkWidget *check = gtk_check_button_new_with_label("Also delete the downloaded icon");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
        gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
        adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), check);
        g_object_set_data(G_OBJECT(d), "icon-check", check);
    }
    g_signal_connect_object(d, "response", G_CALLBACK(on_remove_response), self, 0);
    adw_dialog_present(d, GTK_WIDGET(self));
}

static void on_saved(EaShortcutDialog *d, const char *path, gpointer ud)
{
    (void)d; (void)path;
    EaShortcutsPage *self = ud;
    ea_window_toast(self->window, "Shortcut saved");
    ea_window_reload_entries(self->window);
}

static void on_edit_clicked(GtkButton *btn, gpointer ud)
{
    EaShortcutsPage *self = ud;
    EaEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    EaShortcutDialog *d = ea_shortcut_dialog_new_for_entry(e);
    g_signal_connect_object(d, "saved", G_CALLBACK(on_saved), self, 0);
    adw_dialog_present(ADW_DIALOG(d), GTK_WIDGET(self));
}

static void on_open_folder(EaShortcutsPage *self)
{
    const char *dir = extapp_context_applications_dir(ea_backend());
    g_mkdir_with_parents(dir, 0755);
    GFile *f = g_file_new_for_path(dir);
    GtkFileLauncher *l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, GTK_WINDOW(self->window), NULL, NULL, NULL);
    g_object_unref(l);
    g_object_unref(f);
}

/* ---- rows -------------------------------------------------------------- */

static GtkWidget *icon_button(const char *icon, const char *tooltip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b, "flat");
    return b;
}

static GtkWidget *create_row(gpointer item, gpointer ud)
{
    EaShortcutsPage *self = ud;
    EaEntry *e = item;

    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), ea_entry_get_name(e));
    adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 1);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
    const char *target = ea_entry_get_target_path(e);
    g_autofree char *subtitle = NULL;
    if (target && !ea_entry_get_target_exists(e))
        subtitle = g_strdup_printf("Program missing: %s", target);
    else
        subtitle = g_strdup(ea_entry_get_exec(e) ? ea_entry_get_exec(e) : "");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_widget_set_tooltip_text(row, ea_entry_get_file_path(e));

    GtkWidget *img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    ea_image_set_icon(GTK_IMAGE(img), ea_entry_get_icon(e), NULL);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), img);

    if (target && !ea_entry_get_target_exists(e))
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), ea_pill_new("Broken", "error"));
    if (ea_entry_get_no_display(e))
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), ea_pill_new("Hidden", NULL));
    if (ea_entry_get_managed(e)) {
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), ea_pill_new("ExtApp", "accent"));
        if (target) {
            GtkWidget *edit = icon_button("document-edit-symbolic", "Edit Shortcut");
            g_object_set_data_full(G_OBJECT(edit), "entry", g_object_ref(e), g_object_unref);
            g_signal_connect(edit, "clicked", G_CALLBACK(on_edit_clicked), self);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), edit);
        }
    }
    GtkWidget *del = icon_button("user-trash-symbolic", "Remove Shortcut");
    gtk_widget_add_css_class(del, "error");
    g_object_set_data_full(G_OBJECT(del), "entry", g_object_ref(e), g_object_unref);
    g_signal_connect(del, "clicked", G_CALLBACK(on_remove_clicked), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), del);
    return row;
}

/* ---- construction ------------------------------------------------------ */

static void ea_shortcuts_page_dispose(GObject *obj)
{
    EaShortcutsPage *self = EA_SHORTCUTS_PAGE(obj);
    if (self->list)
        gtk_list_box_bind_model(self->list, NULL, NULL, NULL, NULL);
    g_clear_object(&self->filtered);
    self->window = NULL;
    G_OBJECT_CLASS(ea_shortcuts_page_parent_class)->dispose(obj);
}

static void ea_shortcuts_page_class_init(EaShortcutsPageClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = ea_shortcuts_page_dispose;
}

static void ea_shortcuts_page_init(EaShortcutsPage *self)
{
    GtkWidget *tv = adw_toolbar_view_new();

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);
    self->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(self->search, "Search shortcuts");
    gtk_widget_set_hexpand(GTK_WIDGET(self->search), TRUE);
    g_signal_connect_swapped(self->search, "search-changed", G_CALLBACK(refilter), self);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->search));

    self->only_ours = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Only ExtApp"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->only_ours),
                                "Show only shortcuts created by ExtApp");
    g_signal_connect_swapped(self->only_ours, "toggled", G_CALLBACK(refilter), self);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->only_ours));

    GtkWidget *open = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_tooltip_text(open, "Open Applications Folder");
    g_signal_connect_swapped(open, "clicked", G_CALLBACK(on_open_folder), self);
    gtk_box_append(GTK_BOX(bar), open);

    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Refresh");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(refresh), "win.refresh");
    gtk_box_append(GTK_BOX(bar), refresh);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), bar);

    self->stack = GTK_STACK(gtk_stack_new());
    GtkWidget *empty = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "view-grid-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Shortcuts");
    adw_status_page_set_description(ADW_STATUS_PAGE(empty),
        "Shortcuts in your personal applications folder appear here.");
    gtk_stack_add_named(self->stack, empty, "empty");

    self->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->list), GTK_ALIGN_START);
    gtk_widget_set_hexpand(GTK_WIDGET(self->list), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->list));
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), box);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_stack_add_named(self->stack, sw, "list");

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(self->stack));
    adw_bin_set_child(ADW_BIN(self), tv);
}

EaShortcutsPage *ea_shortcuts_page_new(EaWindow *window)
{
    EaShortcutsPage *self = g_object_new(EA_TYPE_SHORTCUTS_PAGE, NULL);
    self->window = window;
    self->filter = gtk_custom_filter_new(filter_func, self, NULL);
    self->filtered = gtk_filter_list_model_new(
        G_LIST_MODEL(g_object_ref(ea_window_get_entries(window))),
        GTK_FILTER(self->filter)); /* takes both refs */
    g_signal_connect(self->filtered, "items-changed",
                     G_CALLBACK(on_filtered_changed), self);
    gtk_list_box_bind_model(self->list, G_LIST_MODEL(self->filtered), create_row,
                            self, NULL);
    update_stack(self);
    return self;
}
