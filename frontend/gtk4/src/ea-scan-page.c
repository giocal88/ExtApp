/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-scan-page.h"

#include "ea-async.h"
#include "ea-objects.h"
#include "ea-shortcut-dialog.h"
#include "ea-util.h"

struct _EaScanPage {
    AdwBin parent_instance;

    EaWindow *window; /* not owned: the window owns us */
    char *folder;

    AdwButtonContent *folder_content;
    GtkButton *scan_btn;
    GtkStack *stack;
    GtkLabel *progress_label;
    GtkColumnView *view;
    GListStore *store;
    GtkMultiSelection *selection;
    GtkLabel *count_label;
    GtkButton *create_btn;
    AdwSpinRow *depth_row;
    AdwSwitchRow *scripts_row;
    AdwSwitchRow *hidden_row;

    GCancellable *scan_cancel;
    gint *progress; /* g_atomic_rc_box */
    guint progress_timer;
    GQueue queue;   /* EaCandidate* waiting for the shortcut dialog */
};

G_DEFINE_FINAL_TYPE(EaScanPage, ea_scan_page, ADW_TYPE_BIN)

static void start_scan(EaScanPage *self);

/* ---- status ------------------------------------------------------------ */

static void clear_box(GtkBox *box)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_box_remove(box, c);
}

static void update_status(EaCandidate *c, GParamSpec *pspec, GtkBox *box)
{
    (void)pspec;
    clear_box(box);
    if (ea_candidate_get_desktop_file(c)) {
        GtkWidget *p = ea_pill_new("Has shortcut", "success");
        gtk_widget_set_tooltip_text(p, ea_candidate_get_desktop_file(c));
        gtk_box_append(box, p);
    }
    if (!ea_candidate_get_is_executable(c))
        gtk_box_append(box, ea_pill_new("Not executable", "warning"));
}

static void relink(EaScanPage *self)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    for (guint i = 0; i < n; i++) {
        EaCandidate *c = g_list_model_get_item(G_LIST_MODEL(self->store), i);
        EaEntry *e = ea_window_find_entry_for_target(self->window,
                                                     ea_candidate_get_real_path(c));
        ea_candidate_set_desktop_file(c, e ? ea_entry_get_file_path(e) : NULL);
        g_object_unref(c);
    }
}

static void on_entries_changed(GListModel *m, guint pos, guint rem, guint add,
                               gpointer ud)
{
    (void)m; (void)pos; (void)rem; (void)add;
    relink(EA_SCAN_PAGE(ud));
}

/* ---- column factories -------------------------------------------------- */

static void setup_icon(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f; (void)ud;
    GtkWidget *img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), 24);
    gtk_list_item_set_child(li, img);
}

static void bind_icon(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f; (void)ud;
    EaCandidate *c = gtk_list_item_get_item(li);
    const char *icon = "application-x-executable-symbolic";
    switch (ea_candidate_get_kind(c)) {
    case EXTAPP_KIND_APPIMAGE: icon = "package-x-generic-symbolic"; break;
    case EXTAPP_KIND_SCRIPT:   icon = "utilities-terminal-symbolic"; break;
    case EXTAPP_KIND_ELF:      break;
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(gtk_list_item_get_child(li)), icon);
}

static void setup_label(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f;
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), GPOINTER_TO_INT(ud) == 2
                                              ? PANGO_ELLIPSIZE_START
                                              : PANGO_ELLIPSIZE_END);
    if (GPOINTER_TO_INT(ud) != 0)
        gtk_widget_add_css_class(l, "dim-label");
    gtk_list_item_set_child(li, l);
}

/* ud: 0 = name, 1 = type, 2 = location */
static void bind_label(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f;
    EaCandidate *c = gtk_list_item_get_item(li);
    GtkLabel *l = GTK_LABEL(gtk_list_item_get_child(li));
    switch (GPOINTER_TO_INT(ud)) {
    case 0:
        gtk_label_set_text(l, ea_candidate_get_name(c));
        gtk_widget_set_tooltip_text(GTK_WIDGET(l), ea_candidate_get_name(c));
        break;
    case 1:
        gtk_label_set_text(l, extapp_kind_str(ea_candidate_get_kind(c)));
        break;
    default: {
        g_autofree char *dir = g_path_get_dirname(ea_candidate_get_path(c));
        g_autofree char *base = g_path_get_basename(ea_candidate_get_path(c));
        g_autofree char *disp = g_strdup_printf("%s/%s", dir, base);
        gtk_label_set_text(l, disp);
        gtk_widget_set_tooltip_text(GTK_WIDGET(l), ea_candidate_get_path(c));
        break;
    }
    }
}

static void setup_status(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f; (void)ud;
    gtk_list_item_set_child(li, gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
}

static void bind_status(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f; (void)ud;
    EaCandidate *c = gtk_list_item_get_item(li);
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(li));
    update_status(c, NULL, box);
    g_signal_connect_object(c, "notify", G_CALLBACK(update_status), box, 0);
}

static void unbind_status(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f; (void)ud;
    EaCandidate *c = gtk_list_item_get_item(li);
    g_signal_handlers_disconnect_by_func(c, update_status, gtk_list_item_get_child(li));
}

static GtkColumnViewColumn *add_column(GtkColumnView *view, const char *title,
                                       GCallback setup, GCallback bind,
                                       GCallback unbind, int kind,
                                       gboolean expand)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", setup, GINT_TO_POINTER(kind));
    g_signal_connect(f, "bind", bind, GINT_TO_POINTER(kind));
    if (unbind)
        g_signal_connect(f, "unbind", unbind, GINT_TO_POINTER(kind));
    GtkColumnViewColumn *col = gtk_column_view_column_new(title, f);
    gtk_column_view_column_set_expand(col, expand);
    gtk_column_view_column_set_resizable(col, TRUE);
    gtk_column_view_append_column(view, col);
    g_object_unref(col);
    return col;
}

/* ---- creating shortcuts ------------------------------------------------ */

static void process_queue(EaScanPage *self);

static void on_shortcut_saved(EaShortcutDialog *d, const char *path, gpointer ud)
{
    EaCandidate *c = g_object_get_data(G_OBJECT(d), "candidate");
    EaScanPage *self = EA_SCAN_PAGE(ud);
    ea_candidate_set_desktop_file(c, path);
    ea_candidate_set_is_executable(
        c, g_file_test(ea_candidate_get_path(c), G_FILE_TEST_IS_EXECUTABLE));
    g_autofree char *msg = g_strdup_printf("Shortcut for “%s” saved",
                                           ea_candidate_get_name(c));
    ea_window_toast(self->window, msg);
    ea_window_reload_entries(self->window);
}

static gboolean process_queue_idle(gpointer ud)
{
    process_queue(EA_SCAN_PAGE(ud));
    g_object_unref(ud);
    return G_SOURCE_REMOVE;
}

static void on_dialog_closed(AdwDialog *d, gpointer ud)
{
    (void)d;
    g_idle_add(process_queue_idle, g_object_ref(ud));
}

static void process_queue(EaScanPage *self)
{
    EaCandidate *c = g_queue_pop_head(&self->queue);
    if (!c)
        return;
    EaEntry *existing = ea_window_find_entry_for_target(
        self->window, ea_candidate_get_real_path(c));
    EaShortcutDialog *d = ea_shortcut_dialog_new_for_candidate(c, existing);
    g_object_set_data_full(G_OBJECT(d), "candidate", c, g_object_unref);
    g_signal_connect_object(d, "saved", G_CALLBACK(on_shortcut_saved), self, 0);
    g_signal_connect_object(d, "closed", G_CALLBACK(on_dialog_closed), self, 0);
    adw_dialog_present(ADW_DIALOG(d), GTK_WIDGET(self));
}

static void on_create_clicked(EaScanPage *self)
{
    if (!g_queue_is_empty(&self->queue))
        return; /* a batch is already running */
    GtkBitset *set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(self->selection));
    GtkBitsetIter it;
    guint pos;
    for (gboolean ok = gtk_bitset_iter_init_first(&it, set, &pos); ok;
         ok = gtk_bitset_iter_next(&it, &pos))
        g_queue_push_tail(&self->queue,
                          g_list_model_get_item(G_LIST_MODEL(self->store), pos));
    gtk_bitset_unref(set);
    process_queue(self);
}

static void on_row_activated(GtkColumnView *view, guint pos, gpointer ud)
{
    (void)view;
    EaScanPage *self = ud;
    if (!g_queue_is_empty(&self->queue))
        return;
    g_queue_push_tail(&self->queue, g_list_model_get_item(G_LIST_MODEL(self->store), pos));
    process_queue(self);
}

static void on_selection_changed(GtkSelectionModel *m, guint pos, guint n, gpointer ud)
{
    (void)pos; (void)n;
    EaScanPage *self = ud;
    GtkBitset *set = gtk_selection_model_get_selection(m);
    guint64 count = gtk_bitset_get_size(set);
    gtk_bitset_unref(set);
    guint total = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    g_autofree char *txt = g_strdup_printf("%u programs · %" G_GUINT64_FORMAT
                                           " selected", total, count);
    gtk_label_set_text(self->count_label, txt);
    gtk_widget_set_sensitive(GTK_WIDGET(self->create_btn), count > 0);
    gtk_button_set_label(self->create_btn,
                         count > 1 ? "Create Shortcuts…" : "Create Shortcut…");
}

static void on_items_changed(GListModel *m, guint pos, guint rem, guint add,
                             gpointer ud)
{
    (void)pos; (void)rem; (void)add;
    on_selection_changed(GTK_SELECTION_MODEL(m), 0, 0, ud);
}

/* ---- scanning ---------------------------------------------------------- */

static void set_scanning(EaScanPage *self, gboolean on)
{
    gtk_button_set_label(self->scan_btn, on ? "Cancel" : "Scan");
    if (on) {
        gtk_widget_remove_css_class(GTK_WIDGET(self->scan_btn), "suggested-action");
    } else {
        gtk_widget_add_css_class(GTK_WIDGET(self->scan_btn), "suggested-action");
        if (self->progress_timer) {
            g_source_remove(self->progress_timer);
            self->progress_timer = 0;
        }
    }
    gtk_widget_set_sensitive(GTK_WIDGET(self->scan_btn), on || self->folder != NULL);
}

static gboolean on_progress_tick(gpointer ud)
{
    EaScanPage *self = ud;
    g_autofree char *txt = g_strdup_printf("Looked at %d items…",
                                           g_atomic_int_get(self->progress));
    gtk_label_set_text(self->progress_label, txt);
    return G_SOURCE_CONTINUE;
}

static void on_scan_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)src;
    GError *err = NULL;
    GListStore *store = ea_scan_finish(res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* page cancelled it; it already updated the UI */
    }
    EaScanPage *self = EA_SCAN_PAGE(ud);
    g_clear_object(&self->scan_cancel);
    set_scanning(self, FALSE);

    if (!store) {
        ea_window_toast(self->window, err ? err->message : "Scan failed");
        g_clear_error(&err);
        gtk_stack_set_visible_child_name(self->stack, "empty");
        return;
    }
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < n; i++)
        g_ptr_array_add(items, g_list_model_get_item(G_LIST_MODEL(store), i));
    g_list_store_splice(self->store, 0,
                        g_list_model_get_n_items(G_LIST_MODEL(self->store)),
                        items->pdata, items->len);
    g_ptr_array_unref(items);
    g_object_unref(store);
    gtk_stack_set_visible_child_name(self->stack, n ? "results" : "none");
}

static void cancel_scan(EaScanPage *self)
{
    if (!self->scan_cancel)
        return;
    g_cancellable_cancel(self->scan_cancel);
    g_clear_object(&self->scan_cancel);
    set_scanning(self, FALSE);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    gtk_stack_set_visible_child_name(self->stack, n ? "results" : "empty");
}

static void start_scan(EaScanPage *self)
{
    if (self->scan_cancel) {
        cancel_scan(self);
        return;
    }
    if (!self->folder)
        return;

    ExtAppScanOptions o;
    extapp_scan_options_init(&o);
    o.max_depth = (int)adw_spin_row_get_value(self->depth_row);
    o.include_scripts = adw_switch_row_get_active(self->scripts_row);
    o.include_hidden = adw_switch_row_get_active(self->hidden_row);
    ea_settings_set_int("scan-depth", o.max_depth);
    ea_settings_set_int("scan-scripts", o.include_scripts);
    ea_settings_set_int("scan-hidden", o.include_hidden);

    g_atomic_int_set(self->progress, 0);
    gtk_label_set_text(self->progress_label, "Starting…");
    gtk_stack_set_visible_child_name(self->stack, "loading");
    self->scan_cancel = g_cancellable_new();
    set_scanning(self, TRUE);
    self->progress_timer = g_timeout_add(150, on_progress_tick, self);
    ea_scan_async(self->folder, &o, self->progress, self->scan_cancel,
                  on_scan_done, self);
}

/* ---- folder ------------------------------------------------------------ */

static void set_folder(EaScanPage *self, const char *path)
{
    g_free(self->folder);
    self->folder = g_strdup(path);
    g_autofree char *base = path ? g_path_get_basename(path) : NULL;
    adw_button_content_set_label(self->folder_content, base ? base : "Choose Folder…");
    gtk_widget_set_tooltip_text(
        gtk_widget_get_parent(GTK_WIDGET(self->folder_content)), path);
    gtk_widget_set_sensitive(GTK_WIDGET(self->scan_btn), path != NULL);
}

static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    EaScanPage *self = ud; /* ref held for us */
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        g_autofree char *path = g_file_get_path(f);
        if (path) {
            cancel_scan(self);
            set_folder(self, path);
            ea_settings_set_string("last-folder", path);
            start_scan(self);
        }
        g_object_unref(f);
    }
    g_object_unref(self);
}

static void on_choose_folder(EaScanPage *self)
{
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Choose a Folder to Scan");
    if (self->folder) {
        GFile *init = g_file_new_for_path(self->folder);
        gtk_file_dialog_set_initial_folder(fd, init);
        g_object_unref(init);
    }
    gtk_file_dialog_select_folder(fd, GTK_WINDOW(self->window), NULL,
                                  on_folder_chosen, g_object_ref(self));
    g_object_unref(fd);
}

/* ---- construction ------------------------------------------------------ */

static void ea_scan_page_dispose(GObject *obj)
{
    EaScanPage *self = EA_SCAN_PAGE(obj);
    if (self->scan_cancel)
        g_cancellable_cancel(self->scan_cancel);
    g_clear_object(&self->scan_cancel);
    if (self->progress_timer) {
        g_source_remove(self->progress_timer);
        self->progress_timer = 0;
    }
    if (self->window)
        g_signal_handlers_disconnect_by_data(ea_window_get_entries(self->window), self);
    self->window = NULL;
    g_queue_clear_full(&self->queue, g_object_unref);
    G_OBJECT_CLASS(ea_scan_page_parent_class)->dispose(obj);
}

static void ea_scan_page_finalize(GObject *obj)
{
    EaScanPage *self = EA_SCAN_PAGE(obj);
    g_free(self->folder);
    g_clear_object(&self->store);
    g_clear_object(&self->selection);
    if (self->progress)
        g_atomic_rc_box_release(self->progress);
    G_OBJECT_CLASS(ea_scan_page_parent_class)->finalize(obj);
}

static void ea_scan_page_class_init(EaScanPageClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = ea_scan_page_dispose;
    G_OBJECT_CLASS(klass)->finalize = ea_scan_page_finalize;
}

static GtkWidget *build_options_popover(EaScanPage *self)
{
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");

    self->depth_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(0, 10, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->depth_row), "Sub-folder Depth");
    adw_spin_row_set_value(self->depth_row, ea_settings_get_int("scan-depth", 3));
    self->scripts_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->scripts_row),
                                  "Include Scripts");
    adw_switch_row_set_active(self->scripts_row, ea_settings_get_int("scan-scripts", 0));
    self->hidden_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->hidden_row),
                                  "Include Hidden Files");
    adw_switch_row_set_active(self->hidden_row, ea_settings_get_int("scan-hidden", 0));

    gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(self->depth_row));
    gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(self->scripts_row));
    gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(self->hidden_row));
    gtk_widget_set_size_request(list, 300, -1);

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), list);
    return pop;
}

static GtkWidget *status_page(const char *icon, const char *title,
                              const char *desc, GtkWidget *child)
{
    GtkWidget *p = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(p), icon);
    adw_status_page_set_title(ADW_STATUS_PAGE(p), title);
    adw_status_page_set_description(ADW_STATUS_PAGE(p), desc);
    if (child)
        adw_status_page_set_child(ADW_STATUS_PAGE(p), child);
    return p;
}

static void ea_scan_page_init(EaScanPage *self)
{
    self->progress = g_atomic_rc_box_new0(gint);
    g_queue_init(&self->queue);
    self->store = g_list_store_new(EA_TYPE_CANDIDATE);
    self->selection = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(self->store)));
    g_signal_connect(self->selection, "selection-changed",
                     G_CALLBACK(on_selection_changed), self);
    g_signal_connect(self->selection, "items-changed",
                     G_CALLBACK(on_items_changed), self);

    GtkWidget *tv = adw_toolbar_view_new();

    /* top bar */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);
    GtkWidget *folder_btn = gtk_button_new();
    self->folder_content = ADW_BUTTON_CONTENT(adw_button_content_new());
    adw_button_content_set_icon_name(self->folder_content, "folder-open-symbolic");
    adw_button_content_set_label(self->folder_content, "Choose Folder…");
    gtk_button_set_child(GTK_BUTTON(folder_btn), GTK_WIDGET(self->folder_content));
    g_signal_connect_swapped(folder_btn, "clicked", G_CALLBACK(on_choose_folder), self);
    gtk_box_append(GTK_BOX(bar), folder_btn);

    GtkWidget *opts = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(opts), "preferences-system-symbolic");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(opts), build_options_popover(self));
    gtk_widget_set_tooltip_text(opts, "Scan Options");
    gtk_box_append(GTK_BOX(bar), opts);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    self->scan_btn = GTK_BUTTON(gtk_button_new_with_label("Scan"));
    gtk_widget_add_css_class(GTK_WIDGET(self->scan_btn), "suggested-action");
    gtk_widget_set_sensitive(GTK_WIDGET(self->scan_btn), FALSE);
    g_signal_connect_swapped(self->scan_btn, "clicked", G_CALLBACK(start_scan), self);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->scan_btn));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), bar);

    /* content */
    self->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(self->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    GtkWidget *choose = gtk_button_new_with_label("Choose Folder…");
    gtk_widget_add_css_class(choose, "pill");
    gtk_widget_add_css_class(choose, "suggested-action");
    gtk_widget_set_halign(choose, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(choose, "clicked", G_CALLBACK(on_choose_folder), self);
    gtk_stack_add_named(self->stack,
        status_page("folder-saved-search-symbolic", "Find Programs",
                    "Choose a folder containing AppImages or other programs "
                    "to create launcher shortcuts for them.", choose),
        "empty");

    GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(lbox, GTK_ALIGN_CENTER);
    GtkWidget *sp = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(sp));
    gtk_widget_set_size_request(sp, 32, 32);
    self->progress_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->progress_label), "dim-label");
    gtk_box_append(GTK_BOX(lbox), sp);
    gtk_box_append(GTK_BOX(lbox), GTK_WIDGET(self->progress_label));
    gtk_stack_add_named(self->stack, lbox, "loading");

    gtk_stack_add_named(self->stack,
        status_page("edit-find-symbolic", "No Programs Found",
                    "Try a different folder, a deeper sub-folder depth, or "
                    "enable scripts in the scan options.", NULL),
        "none");

    self->view = GTK_COLUMN_VIEW(gtk_column_view_new(
        GTK_SELECTION_MODEL(g_object_ref(self->selection))));
    gtk_column_view_set_show_row_separators(self->view, TRUE);
    gtk_column_view_set_enable_rubberband(self->view, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(self->view), "data-table");
    add_column(self->view, "", G_CALLBACK(setup_icon), G_CALLBACK(bind_icon), NULL, 0, FALSE);
    GtkColumnViewColumn *name_col = add_column(self->view, "Name", G_CALLBACK(setup_label),
                                               G_CALLBACK(bind_label), NULL, 0, FALSE);
    gtk_column_view_column_set_fixed_width(name_col, 220);
    add_column(self->view, "Type", G_CALLBACK(setup_label), G_CALLBACK(bind_label), NULL, 1, FALSE);
    add_column(self->view, "Location", G_CALLBACK(setup_label), G_CALLBACK(bind_label), NULL, 2, TRUE);
    add_column(self->view, "Status", G_CALLBACK(setup_status), G_CALLBACK(bind_status),
               G_CALLBACK(unbind_status), 0, FALSE);
    g_signal_connect(self->view, "activate", G_CALLBACK(on_row_activated), self);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), GTK_WIDGET(self->view));
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_stack_add_named(self->stack, sw, "results");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(self->stack));

    /* bottom bar */
    GtkWidget *ab = gtk_action_bar_new();
    self->count_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->count_label), "dim-label");
    gtk_action_bar_pack_start(GTK_ACTION_BAR(ab), GTK_WIDGET(self->count_label));
    self->create_btn = GTK_BUTTON(gtk_button_new_with_label("Create Shortcut…"));
    gtk_widget_add_css_class(GTK_WIDGET(self->create_btn), "suggested-action");
    gtk_widget_set_sensitive(GTK_WIDGET(self->create_btn), FALSE);
    g_signal_connect_swapped(self->create_btn, "clicked", G_CALLBACK(on_create_clicked), self);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(ab), GTK_WIDGET(self->create_btn));
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), ab);

    adw_bin_set_child(ADW_BIN(self), tv);
    on_selection_changed(GTK_SELECTION_MODEL(self->selection), 0, 0, self);
}

EaScanPage *ea_scan_page_new(EaWindow *window)
{
    EaScanPage *self = g_object_new(EA_TYPE_SCAN_PAGE, NULL);
    self->window = window;
    g_signal_connect(ea_window_get_entries(window), "items-changed",
                     G_CALLBACK(on_entries_changed), self);

    g_autofree char *last = ea_settings_get_string("last-folder");
    if (last && *last && g_file_test(last, G_FILE_TEST_IS_DIR)) {
        set_folder(self, last);
        start_scan(self);
    }
    return self;
}
