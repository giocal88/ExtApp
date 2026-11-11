/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-shortcut-dialog.h"

#include "ea-async.h"
#include "ea-icon-search-dialog.h"
#include "ea-util.h"

#include <glib/gstdio.h>
#include <stdlib.h>

#define ICON_SIZE 256

static const struct {
    const char *label;
    const char *value;
} categories[] = {
    {"Utility", "Utility"},          {"Development", "Development"},
    {"Graphics", "Graphics"},        {"Games", "Game"},
    {"Internet", "Network"},         {"Office", "Office"},
    {"Audio & Video", "AudioVideo"}, {"Education", "Education"},
    {"Science", "Science"},          {"System", "System"},
    {"Settings", "Settings"},
};

struct _EaShortcutDialog {
    AdwDialog parent_instance;

    char *target_path;
    char *edit_path;      /* managed shortcut being edited (nullable)  */
    char *old_icon;       /* Icon= of the edited shortcut (nullable)   */
    char *icon_value;     /* icon kept as-is (name or path, nullable)  */
    GBytes *icon_bytes;   /* new icon waiting to be stored (nullable)  */

    GtkImage *icon_image;
    GtkButton *icon_clear_btn;
    AdwEntryRow *name_row;
    AdwEntryRow *comment_row;
    AdwEntryRow *args_row;
    AdwComboRow *category_row;
    AdwSwitchRow *terminal_row;
    AdwSwitchRow *chmod_row;
    AdwActionRow *target_row;
    AdwBanner *banner;
    GtkButton *save_btn;
    AdwToastOverlay *toasts;
};

G_DEFINE_FINAL_TYPE(EaShortcutDialog, ea_shortcut_dialog, ADW_TYPE_DIALOG)

enum { SIGNAL_SAVED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void toast(EaShortcutDialog *self, const char *msg)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(msg));
}

/* ---- icon handling ----------------------------------------------------- */

static void refresh_icon(EaShortcutDialog *self)
{
    gboolean has = FALSE;
    if (self->icon_bytes) {
        GdkTexture *t = ea_image_texture(self->icon_bytes, 128, NULL);
        if (t) {
            gtk_image_set_from_paintable(self->icon_image, GDK_PAINTABLE(t));
            g_object_unref(t);
            has = TRUE;
        }
    } else if (self->icon_value) {
        ea_image_set_icon(self->icon_image, self->icon_value, NULL);
        has = TRUE;
    }
    if (!has)
        gtk_image_set_from_icon_name(self->icon_image, "application-x-executable");
    gtk_widget_set_visible(GTK_WIDGET(self->icon_clear_btn), has);
}

static void set_icon_bytes(EaShortcutDialog *self, GBytes *bytes)
{
    g_clear_pointer(&self->icon_bytes, g_bytes_unref);
    g_clear_pointer(&self->icon_value, g_free);
    self->icon_bytes = bytes ? g_bytes_ref(bytes) : NULL;
    refresh_icon(self);
}

static void on_icon_chosen(EaIconSearchDialog *d, GBytes *bytes, gpointer ud)
{
    (void)d;
    set_icon_bytes(EA_SHORTCUT_DIALOG(ud), bytes);
}

static void on_search_icon(EaShortcutDialog *self)
{
    const char *name = gtk_editable_get_text(GTK_EDITABLE(self->name_row));
    g_autofree char *q = g_strdup_printf("%s icon", *name ? name : "app");
    EaIconSearchDialog *d = ea_icon_search_dialog_new(q);
    g_signal_connect_object(d, "icon-chosen", G_CALLBACK(on_icon_chosen), self, 0);
    adw_dialog_present(ADW_DIALOG(d), GTK_WIDGET(self));
}

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
    EaShortcutDialog *self = ud; /* ref held for us */
    if (file) {
        GBytes *raw = g_file_load_bytes(file, NULL, NULL, &err);
        GBytes *norm = raw ? ea_image_normalize(raw, ICON_SIZE, &err) : NULL;
        if (norm)
            set_icon_bytes(self, norm);
        else
            toast(self, "Could not load this image");
        if (raw)
            g_bytes_unref(raw);
        if (norm)
            g_bytes_unref(norm);
        g_object_unref(file);
    }
    g_clear_error(&err);
    g_object_unref(self);
}

static void on_choose_file(EaShortcutDialog *self)
{
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Choose an Icon");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Images");
    gtk_file_filter_add_pixbuf_formats(f);
    gtk_file_filter_add_mime_type(f, "image/svg+xml");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, f);
    gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filters));
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    gtk_file_dialog_open(fd, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
                         on_file_chosen, g_object_ref(self));
    g_object_unref(filters);
    g_object_unref(f);
    g_object_unref(fd);
}

static void on_clear_icon(EaShortcutDialog *self)
{
    set_icon_bytes(self, NULL);
}

/* ---- saving ------------------------------------------------------------ */

static gboolean is_our_icon(const char *icon)
{
    const char *dir = extapp_context_icons_dir(ea_backend());
    return icon && dir && g_str_has_prefix(icon, dir) && icon[strlen(dir)] == '/';
}

static void update_save_sensitivity(EaShortcutDialog *self)
{
    g_autofree char *name = g_strstrip(
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->name_row))));
    gtk_widget_set_sensitive(GTK_WIDGET(self->save_btn), *name != '\0');
}

static char *opt_text(AdwEntryRow *row)
{
    char *s = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
    if (!*s) {
        g_free(s);
        return NULL;
    }
    return s;
}

static void on_save(EaShortcutDialog *self)
{
    ExtAppContext *ctx = ea_backend();
    g_autofree char *name = opt_text(self->name_row);
    g_autofree char *comment = opt_text(self->comment_row);
    g_autofree char *args = opt_text(self->args_row);
    if (!name)
        return;

    char *err = NULL;
    char *saved_icon = NULL;
    if (self->icon_bytes) {
        gsize n = 0;
        const unsigned char *d = g_bytes_get_data(self->icon_bytes, &n);
        ExtAppStatus st = extapp_icon_save(ctx, name, d, n, &saved_icon, &err);
        if (st != EXTAPP_OK) {
            toast(self, err ? err : extapp_status_str(st));
            free(err);
            return;
        }
    }

    guint cat = adw_combo_row_get_selected(self->category_row);
    ExtAppEntrySpec spec = {
        .name = name,
        .target_path = self->target_path,
        .args = args,
        .icon = saved_icon ? saved_icon : self->icon_value,
        .comment = comment,
        .categories = cat < G_N_ELEMENTS(categories) ? categories[cat].value : NULL,
        .terminal = adw_switch_row_get_active(self->terminal_row),
    };
    unsigned flags = 0;
    if (gtk_widget_get_visible(GTK_WIDGET(self->chmod_row)) &&
        adw_switch_row_get_active(self->chmod_row))
        flags |= EXTAPP_CREATE_MAKE_EXECUTABLE;
    if (self->edit_path)
        flags |= EXTAPP_CREATE_OVERWRITE;

    char *path = NULL;
    ExtAppStatus st = extapp_entry_create(ctx, &spec, flags, &path, &err);
    if (st != EXTAPP_OK) {
        if (saved_icon)
            g_unlink(saved_icon);
        toast(self, err ? err : extapp_status_str(st));
        free(err);
        free(saved_icon);
        free(path);
        return;
    }

    /* replaced an icon we had downloaded earlier: drop the old file */
    if (self->old_icon && is_our_icon(self->old_icon) &&
        g_strcmp0(self->old_icon, spec.icon) != 0)
        g_unlink(self->old_icon);

    g_signal_emit(self, signals[SIGNAL_SAVED], 0, path);
    free(saved_icon);
    free(path);
    adw_dialog_close(ADW_DIALOG(self));
}

/* ---- construction ------------------------------------------------------ */

static void ea_shortcut_dialog_finalize(GObject *obj)
{
    EaShortcutDialog *self = EA_SHORTCUT_DIALOG(obj);
    g_free(self->target_path);
    g_free(self->edit_path);
    g_free(self->old_icon);
    g_free(self->icon_value);
    g_clear_pointer(&self->icon_bytes, g_bytes_unref);
    G_OBJECT_CLASS(ea_shortcut_dialog_parent_class)->finalize(obj);
}

static void ea_shortcut_dialog_class_init(EaShortcutDialogClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ea_shortcut_dialog_finalize;
    signals[SIGNAL_SAVED] = g_signal_new("saved", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 1, G_TYPE_STRING);
}

static GtkWidget *flat_button(const char *icon, const char *label)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *c = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(c), icon);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(c), label);
    gtk_button_set_child(GTK_BUTTON(b), c);
    gtk_widget_add_css_class(b, "pill");
    return b;
}

static void ea_shortcut_dialog_init(EaShortcutDialog *self)
{
    adw_dialog_set_content_width(ADW_DIALOG(self), 520);
    adw_dialog_set_content_height(ADW_DIALOG(self), 680);

    GtkWidget *tv = adw_toolbar_view_new();
    GtkWidget *hb = adw_header_bar_new();
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(hb), FALSE);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(hb), FALSE);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), cancel);
    self->save_btn = GTK_BUTTON(gtk_button_new_with_label("Create"));
    gtk_widget_add_css_class(GTK_WIDGET(self->save_btn), "suggested-action");
    g_signal_connect_swapped(self->save_btn, "clicked", G_CALLBACK(on_save), self);
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), GTK_WIDGET(self->save_btn));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

    self->banner = ADW_BANNER(adw_banner_new(""));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(self->banner));

    GtkWidget *page = adw_preferences_page_new();

    /* icon */
    GtkWidget *icon_group = adw_preferences_group_new();
    GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    self->icon_image = GTK_IMAGE(gtk_image_new());
    gtk_image_set_pixel_size(self->icon_image, 96);
    gtk_widget_add_css_class(GTK_WIDGET(self->icon_image), "icon-dropshadow");
    gtk_box_append(GTK_BOX(icon_box), GTK_WIDGET(self->icon_image));
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    GtkWidget *search = flat_button("system-search-symbolic", "Search Online…");
    gtk_widget_add_css_class(search, "suggested-action");
    g_signal_connect_swapped(search, "clicked", G_CALLBACK(on_search_icon), self);
    GtkWidget *file = flat_button("document-open-symbolic", "From File…");
    g_signal_connect_swapped(file, "clicked", G_CALLBACK(on_choose_file), self);
    self->icon_clear_btn = GTK_BUTTON(gtk_button_new_from_icon_name("edit-clear-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->icon_clear_btn), "Remove icon");
    gtk_widget_add_css_class(GTK_WIDGET(self->icon_clear_btn), "circular");
    gtk_widget_set_valign(GTK_WIDGET(self->icon_clear_btn), GTK_ALIGN_CENTER);
    g_signal_connect_swapped(self->icon_clear_btn, "clicked", G_CALLBACK(on_clear_icon), self);
    gtk_box_append(GTK_BOX(btns), search);
    gtk_box_append(GTK_BOX(btns), file);
    gtk_box_append(GTK_BOX(btns), GTK_WIDGET(self->icon_clear_btn));
    gtk_box_append(GTK_BOX(icon_box), btns);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(icon_group), icon_box);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(icon_group));

    /* fields */
    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Shortcut");
    self->name_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->name_row), "Name");
    g_signal_connect_swapped(self->name_row, "changed",
                             G_CALLBACK(update_save_sensitivity), self);
    g_signal_connect_swapped(self->name_row, "entry-activated",
                             G_CALLBACK(on_save), self);
    self->comment_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->comment_row),
                                  "Description (optional)");
    self->args_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->args_row),
                                  "Extra arguments (optional)");

    GtkStringList *labels = gtk_string_list_new(NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(categories); i++)
        gtk_string_list_append(labels, categories[i].label);
    self->category_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->category_row), "Category");
    adw_combo_row_set_model(self->category_row, G_LIST_MODEL(labels));
    g_object_unref(labels);

    self->terminal_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->terminal_row),
                                  "Run in Terminal");
    self->chmod_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->chmod_row),
                                  "Make Program Executable");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self->chmod_row),
                                "The file is not marked as executable yet");
    adw_switch_row_set_active(self->chmod_row, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->chmod_row), FALSE);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->name_row));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->comment_row));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->category_row));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->args_row));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->terminal_row));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->chmod_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));

    /* program */
    GtkWidget *pgroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(pgroup), "Program");
    self->target_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->target_row), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(self->target_row), "property");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(pgroup), GTK_WIDGET(self->target_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(pgroup));

    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, page);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(self->toasts));
    adw_dialog_set_child(ADW_DIALOG(self), tv);
    adw_dialog_set_focus(ADW_DIALOG(self), GTK_WIDGET(self->name_row));
}

static guint category_index(const char *cats)
{
    if (!cats)
        return 0;
    g_auto(GStrv) parts = g_strsplit(cats, ";", -1);
    for (char **p = parts; *p; p++)
        for (gsize i = 0; i < G_N_ELEMENTS(categories); i++)
            if (g_strcmp0(*p, categories[i].value) == 0)
                return (guint)i;
    return 0;
}

static void fill_from_entry(EaShortcutDialog *self, EaEntry *e)
{
    self->edit_path = g_strdup(ea_entry_get_file_path(e));
    self->old_icon = g_strdup(ea_entry_get_icon(e));
    self->icon_value = g_strdup(ea_entry_get_icon(e));
    gtk_editable_set_text(GTK_EDITABLE(self->name_row), ea_entry_get_name(e));
    if (ea_entry_get_comment(e))
        gtk_editable_set_text(GTK_EDITABLE(self->comment_row), ea_entry_get_comment(e));
    if (ea_entry_get_args(e))
        gtk_editable_set_text(GTK_EDITABLE(self->args_row), ea_entry_get_args(e));
    adw_combo_row_set_selected(self->category_row,
                               category_index(ea_entry_get_categories(e)));
    adw_switch_row_set_active(self->terminal_row, ea_entry_get_terminal(e));
    adw_dialog_set_title(ADW_DIALOG(self), "Edit Shortcut");
    gtk_button_set_label(self->save_btn, "Save");
}

static void set_target(EaShortcutDialog *self, const char *path, const char *kind)
{
    self->target_path = g_strdup(path);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->target_row), kind);
    adw_action_row_set_subtitle(self->target_row, path);
}

EaShortcutDialog *ea_shortcut_dialog_new_for_candidate(EaCandidate *c,
                                                       EaEntry *existing)
{
    EaShortcutDialog *self = g_object_new(EA_TYPE_SHORTCUT_DIALOG, NULL);
    adw_dialog_set_title(ADW_DIALOG(self), "New Shortcut");
    set_target(self, ea_candidate_get_path(c),
               extapp_kind_str(ea_candidate_get_kind(c)));
    gtk_widget_set_visible(GTK_WIDGET(self->chmod_row),
                           !ea_candidate_get_is_executable(c));

    if (existing && ea_entry_get_managed(existing)) {
        fill_from_entry(self, existing);
    } else {
        gtk_editable_set_text(GTK_EDITABLE(self->name_row), ea_candidate_get_name(c));
        if (existing) {
            g_autofree char *msg = g_strdup_printf(
                "“%s” already launches this program. A second shortcut will be created.",
                ea_entry_get_name(existing));
            adw_banner_set_title(self->banner, msg);
            adw_banner_set_revealed(self->banner, TRUE);
        }
    }
    refresh_icon(self);
    update_save_sensitivity(self);
    return self;
}

EaShortcutDialog *ea_shortcut_dialog_new_for_entry(EaEntry *entry)
{
    EaShortcutDialog *self = g_object_new(EA_TYPE_SHORTCUT_DIALOG, NULL);
    const char *target = ea_entry_get_target_path(entry);
    set_target(self, target ? target : "", "Program");
    gtk_widget_set_visible(GTK_WIDGET(self->chmod_row),
                           target && !g_file_test(target, G_FILE_TEST_IS_EXECUTABLE));
    fill_from_entry(self, entry);
    refresh_icon(self);
    update_save_sensitivity(self);
    return self;
}
