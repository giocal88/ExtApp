/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-icon-search-dialog.h"

#include "ea-async.h"
#include "ea-objects.h"
#include "ea-util.h"

#define MAX_RESULTS     40
#define THUMB_MAX_BYTES (1024 * 1024)
#define IMAGE_MAX_BYTES (5 * 1024 * 1024)
#define ICON_SIZE       256

struct _EaIconSearchDialog {
    AdwDialog parent_instance;

    GtkEntry *entry;
    GtkButton *search_btn;
    GtkStack *stack;
    AdwStatusPage *error_page;
    GtkFlowBox *flow;
    GtkLabel *loading_label;
    AdwToastOverlay *toasts;

    GCancellable *cancel;   /* search + thumbnails */
    GCancellable *download; /* full image download */
};

G_DEFINE_FINAL_TYPE(EaIconSearchDialog, ea_icon_search_dialog, ADW_TYPE_DIALOG)

enum { SIGNAL_ICON_CHOSEN, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- thumbnails -------------------------------------------------------- */

static void on_thumb_fetched(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)src;
    GtkWidget *child = ud; /* ref held for us */
    GError *err = NULL;
    GBytes *bytes = ea_fetch_finish(res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        g_object_unref(child);
        return;
    }

    GtkStack *stack = g_object_get_data(G_OBJECT(child), "thumb-stack");
    GdkTexture *tex = bytes ? ea_image_texture(bytes, 128, NULL) : NULL;
    if (tex) {
        GtkPicture *pic = g_object_get_data(G_OBJECT(child), "thumb-picture");
        gtk_picture_set_paintable(pic, GDK_PAINTABLE(tex));
        gtk_stack_set_visible_child_name(stack, "picture");
        g_object_set_data_full(G_OBJECT(child), "thumb-bytes",
                               g_bytes_ref(bytes), (GDestroyNotify)g_bytes_unref);
        g_object_unref(tex);
    } else {
        /* broken thumbnail: hide the result */
        gtk_widget_set_visible(child, FALSE);
    }
    if (bytes)
        g_bytes_unref(bytes);
    g_clear_error(&err);
    g_object_unref(child);
}

static GtkWidget *make_result_child(EaIconSearchDialog *self, EaIconResult *r)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_size_request(stack, 96, 96);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(stack), spinner, "loading");
    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
    gtk_stack_add_named(GTK_STACK(stack), pic, "picture");
    gtk_box_append(GTK_BOX(box), stack);

    int w = ea_icon_result_get_width(r), h = ea_icon_result_get_height(r);
    const char *provider = ea_icon_result_get_provider(r);
    char *size = ea_icon_result_get_scalable(r) ? g_strdup("SVG")
                 : (w > 0 && h > 0)            ? g_strdup_printf("%d × %d", w, h)
                                               : g_strdup("");
    char *caption = g_strdup_printf("%s\n%s", provider ? provider : "", size);
    GtkWidget *label = gtk_label_new(caption);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    g_free(caption);
    g_free(size);
    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget *child = gtk_flow_box_child_new();
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), box);
    if (ea_icon_result_get_title(r))
        gtk_widget_set_tooltip_text(child, ea_icon_result_get_title(r));
    g_object_set_data_full(G_OBJECT(child), "result", g_object_ref(r),
                           g_object_unref);
    g_object_set_data(G_OBJECT(child), "thumb-stack", stack);
    g_object_set_data(G_OBJECT(child), "thumb-picture", pic);

    const char *thumb = ea_icon_result_get_thumbnail_url(r);
    if (!thumb)
        thumb = ea_icon_result_get_image_url(r);
    ea_fetch_async(thumb, THUMB_MAX_BYTES, self->cancel, on_thumb_fetched,
                   g_object_ref(child));
    return child;
}

/* ---- search ------------------------------------------------------------ */

static void show_error(EaIconSearchDialog *self, const char *title,
                       const char *msg)
{
    adw_status_page_set_title(self->error_page, title);
    adw_status_page_set_description(self->error_page, msg);
    gtk_stack_set_visible_child_name(self->stack, "error");
}

static void on_search_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)src;
    GError *err = NULL;
    GListStore *store = ea_icon_search_finish(res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return;
    }
    EaIconSearchDialog *self = EA_ICON_SEARCH_DIALOG(ud);
    gtk_widget_set_sensitive(GTK_WIDGET(self->search_btn), TRUE);
    if (!store) {
        show_error(self, "Search Failed", err ? err->message : "Unknown error");
        g_clear_error(&err);
        return;
    }
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    if (n == 0) {
        show_error(self, "No Results", "No images found. Try a different search.");
    } else {
        for (guint i = 0; i < n; i++) {
            EaIconResult *r = g_list_model_get_item(G_LIST_MODEL(store), i);
            gtk_flow_box_append(self->flow, make_result_child(self, r));
            g_object_unref(r);
        }
        gtk_stack_set_visible_child_name(self->stack, "results");
    }
    g_object_unref(store);
}

static void start_search(EaIconSearchDialog *self)
{
    g_autofree char *q = g_strstrip(
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry))));
    if (!*q)
        return;

    if (self->cancel) {
        g_cancellable_cancel(self->cancel);
        g_object_unref(self->cancel);
    }
    self->cancel = g_cancellable_new();
    gtk_flow_box_remove_all(self->flow);

    gtk_label_set_text(self->loading_label,
                       "Searching AppImageHub, Flathub and Wikimedia Commons…");
    gtk_stack_set_visible_child_name(self->stack, "loading");
    gtk_widget_set_sensitive(GTK_WIDGET(self->search_btn), FALSE);
    /* No ref on self: closing/disposing cancels, and the callback bails out
     * on cancellation before touching the dialog. */
    ea_icon_search_async(q, MAX_RESULTS, self->cancel, on_search_done, self);
}

/* ---- choosing ---------------------------------------------------------- */

static void finish_with(EaIconSearchDialog *self, GBytes *raw)
{
    GError *err = NULL;
    GBytes *norm = ea_image_normalize(raw, ICON_SIZE, &err);
    if (!norm) {
        adw_toast_overlay_add_toast(
            self->toasts, adw_toast_new("This image format is not supported"));
        g_clear_error(&err);
        return;
    }
    g_signal_emit(self, signals[SIGNAL_ICON_CHOSEN], 0, norm);
    g_bytes_unref(norm);
    adw_dialog_close(ADW_DIALOG(self));
}

static void on_full_image(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)src;
    GtkWidget *child = ud;
    GError *err = NULL;
    GBytes *bytes = ea_fetch_finish(res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        g_object_unref(child);
        return;
    }
    EaIconSearchDialog *self = EA_ICON_SEARCH_DIALOG(
        gtk_widget_get_ancestor(child, EA_TYPE_ICON_SEARCH_DIALOG));
    if (!self) {
        if (bytes)
            g_bytes_unref(bytes);
        g_clear_error(&err);
        g_object_unref(child);
        return;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(self->flow), TRUE);

    /* Prefer the full image; fall back to the thumbnail if it cannot be
     * downloaded or decoded (e.g. WebP without a pixbuf loader). */
    GBytes *use = NULL;
    if (bytes) {
        GdkTexture *probe = ea_image_is_svg(bytes) ? NULL
                                                   : ea_image_texture(bytes, 16, NULL);
        if (probe || ea_image_is_svg(bytes))
            use = g_bytes_ref(bytes);
        g_clear_object(&probe);
    }
    if (!use) {
        GBytes *thumb = g_object_get_data(G_OBJECT(child), "thumb-bytes");
        if (thumb)
            use = g_bytes_ref(thumb);
    }
    if (use) {
        finish_with(self, use);
        g_bytes_unref(use);
    } else {
        adw_toast_overlay_add_toast(
            self->toasts, adw_toast_new(err ? err->message : "Download failed"));
    }
    if (bytes)
        g_bytes_unref(bytes);
    g_clear_error(&err);
    g_object_unref(child);
}

static void on_child_activated(GtkFlowBox *box, GtkFlowBoxChild *child,
                               gpointer ud)
{
    (void)box;
    EaIconSearchDialog *self = ud;
    EaIconResult *r = g_object_get_data(G_OBJECT(child), "result");
    const char *url = r ? ea_icon_result_get_image_url(r) : NULL;
    if (!url) {
        GBytes *thumb = g_object_get_data(G_OBJECT(child), "thumb-bytes");
        if (thumb)
            finish_with(self, thumb);
        return;
    }
    if (self->download) {
        g_cancellable_cancel(self->download);
        g_object_unref(self->download);
    }
    self->download = g_cancellable_new();
    gtk_widget_set_sensitive(GTK_WIDGET(self->flow), FALSE);
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new("Downloading image…"));
    ea_fetch_async(url, IMAGE_MAX_BYTES, self->download, on_full_image,
                   g_object_ref(child));
}

/* ---- lifecycle --------------------------------------------------------- */

static void cancel_all(EaIconSearchDialog *self)
{
    if (self->cancel)
        g_cancellable_cancel(self->cancel);
    if (self->download)
        g_cancellable_cancel(self->download);
}

static void ea_icon_search_dialog_dispose(GObject *obj)
{
    EaIconSearchDialog *self = EA_ICON_SEARCH_DIALOG(obj);
    cancel_all(self);
    g_clear_object(&self->cancel);
    g_clear_object(&self->download);
    G_OBJECT_CLASS(ea_icon_search_dialog_parent_class)->dispose(obj);
}

static void ea_icon_search_dialog_closed(AdwDialog *dialog)
{
    cancel_all(EA_ICON_SEARCH_DIALOG(dialog));
    if (ADW_DIALOG_CLASS(ea_icon_search_dialog_parent_class)->closed)
        ADW_DIALOG_CLASS(ea_icon_search_dialog_parent_class)->closed(dialog);
}

static void ea_icon_search_dialog_class_init(EaIconSearchDialogClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = ea_icon_search_dialog_dispose;
    ADW_DIALOG_CLASS(klass)->closed = ea_icon_search_dialog_closed;
    signals[SIGNAL_ICON_CHOSEN] = g_signal_new(
        "icon-chosen", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BYTES);
}

static GtkWidget *status_page(const char *icon, const char *title,
                              const char *desc)
{
    GtkWidget *p = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(p), icon);
    adw_status_page_set_title(ADW_STATUS_PAGE(p), title);
    if (desc)
        adw_status_page_set_description(ADW_STATUS_PAGE(p), desc);
    gtk_widget_add_css_class(p, "compact");
    gtk_widget_set_hexpand(p, TRUE);
    return p;
}

static void ea_icon_search_dialog_init(EaIconSearchDialog *self)
{
    adw_dialog_set_title(ADW_DIALOG(self), "Search Icon Online");
    adw_dialog_set_content_width(ADW_DIALOG(self), 620);
    adw_dialog_set_content_height(ADW_DIALOG(self), 520);

    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), adw_header_bar_new());

    /* query row */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_widget_set_margin_bottom(row, 6);
    self->entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(self->entry), TRUE);
    gtk_entry_set_placeholder_text(self->entry, "Application name, e.g. Kdenlive");
    g_signal_connect_swapped(self->entry, "activate", G_CALLBACK(start_search), self);
    self->search_btn = GTK_BUTTON(gtk_button_new_with_label("Search"));
    gtk_widget_add_css_class(GTK_WIDGET(self->search_btn), "suggested-action");
    g_signal_connect_swapped(self->search_btn, "clicked", G_CALLBACK(start_search), self);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(self->entry));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(self->search_btn));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), row);

    /* content */
    self->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(self->stack,
                        status_page("system-search-symbolic", "Search for an Icon",
                                    "Icons come from AppImageHub, Flathub and "
                                    "Wikimedia Commons. Logos may be trademarks: "
                                    "check the license before sharing them."),
                        "idle");

    GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(lbox, GTK_ALIGN_CENTER);
    GtkWidget *sp = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(sp));
    gtk_widget_set_size_request(sp, 32, 32);
    self->loading_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->loading_label), "dim-label");
    gtk_box_append(GTK_BOX(lbox), sp);
    gtk_box_append(GTK_BOX(lbox), GTK_WIDGET(self->loading_label));
    gtk_stack_add_named(self->stack, lbox, "loading");

    self->error_page = ADW_STATUS_PAGE(
        status_page("dialog-warning-symbolic", "No Results", NULL));
    gtk_stack_add_named(self->stack, GTK_WIDGET(self->error_page), "error");

    self->flow = GTK_FLOW_BOX(gtk_flow_box_new());
    gtk_flow_box_set_selection_mode(self->flow, GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(self->flow, TRUE);
    gtk_flow_box_set_min_children_per_line(self->flow, 3);
    gtk_flow_box_set_max_children_per_line(self->flow, 8);
    gtk_flow_box_set_activate_on_single_click(self->flow, TRUE);
    gtk_widget_set_valign(GTK_WIDGET(self->flow), GTK_ALIGN_START);
    gtk_widget_set_margin_start(GTK_WIDGET(self->flow), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->flow), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->flow), 12);
    g_signal_connect(self->flow, "child-activated", G_CALLBACK(on_child_activated), self);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), GTK_WIDGET(self->flow));
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_stack_add_named(self->stack, sw, "results");

    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->stack));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(self->toasts));
    adw_dialog_set_child(ADW_DIALOG(self), tv);
    adw_dialog_set_focus(ADW_DIALOG(self), GTK_WIDGET(self->entry));
}

EaIconSearchDialog *ea_icon_search_dialog_new(const char *query)
{
    EaIconSearchDialog *self = g_object_new(EA_TYPE_ICON_SEARCH_DIALOG, NULL);
    if (query && *query) {
        gtk_editable_set_text(GTK_EDITABLE(self->entry), query);
        start_search(self);
    }
    return self;
}
