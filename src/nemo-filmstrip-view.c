/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* nemo-filmstrip-view.c - native Filmstrip / Pellicule view for nemo+
 *
 * Layout inspired by Windows XP Explorer filmstrip mode:
 *   - large preview on top
 *   - horizontal thumbnail strip at the bottom
 */

#include <config.h>

#include "nemo-filmstrip-view.h"

#include "nemo-view.h"
#include "nemo-view-factory.h"
#include "nemo-actions.h"

#include <string.h>
#include <math.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#include <libnemo-private/nemo-file-utilities.h>
#include <libnemo-private/nemo-file.h>
#include <libnemo-private/nemo-global-preferences.h>
#include <libnemo-private/nemo-thumbnails.h>
#include <eel/eel-vfs-extensions.h>
#include <gio/gio.h>

#define THUMB_SIZE 96
#define PREVIEW_MARGIN 12
#define PREVIEW_CAP 1600
#define STRIP_HEIGHT (THUMB_SIZE + 48)

struct NemoFilmstripViewDetails {
	GtkWidget *root;
	GtkWidget *preview_row;
	GtkWidget *preview_event;
	GtkWidget *preview_image;
	GtkWidget *nav_prev;
	GtkWidget *nav_next;
	GtkWidget *info_label;
	GtkWidget *strip_scrolled;
	GtkWidget *strip_box;

	GList *files;          /* NemoFile*, owned refs, sorted display order */
	GHashTable *buttons;   /* NemoFile* -> GtkWidget* (not owned files) */
	NemoFile *selected;    /* owned ref or NULL */
	GdkPixbuf *preview_pb; /* owned or NULL */

	guint rebuild_idle_id;
	guint resize_idle_id;
	int zoom_delta;
	guint thumb_generation; /* invalidates pending async thumb loads */
	gdouble scroll_acc;     /* accumulate smooth-wheel deltas */
	GList *thumb_loads;     /* ThumbLoadData*, pending idle loads */
	GCancellable *preview_cancellable;
	guint preview_generation;
	int preview_req_w;
	int preview_req_h;
};

static GList *nemo_filmstrip_view_get_selection (NemoView *view);
static GList *nemo_filmstrip_view_get_selection_for_file_transfer (NemoView *view);
static void   nemo_filmstrip_view_scroll_to_file (NemoView *view, const char *uri);
static void   filmstrip_select_file (NemoFilmstripView *fview, NemoFile *file, gboolean scroll);
static void   filmstrip_update_preview (NemoFilmstripView *fview);
static void   filmstrip_rebuild_strip (NemoFilmstripView *fview);

G_DEFINE_TYPE (NemoFilmstripView, nemo_filmstrip_view, NEMO_TYPE_VIEW)

/* ---------------- helpers ---------------- */

static gint
compare_files_default (NemoFile *a, NemoFile *b)
{
	char *na, *nb;
	gint r;

	na = nemo_file_get_display_name (a);
	nb = nemo_file_get_display_name (b);
	r = g_utf8_collate (na ? na : "", nb ? nb : "");
	g_free (na);
	g_free (nb);
	return r;
}

static gboolean
file_is_image (NemoFile *file)
{
	char *mime;
	gboolean is_image = FALSE;

	if (file == NULL) {
		return FALSE;
	}
	mime = nemo_file_get_mime_type (file);
	if (mime != NULL) {
		is_image = g_str_has_prefix (mime, "image/");
		g_free (mime);
	}
	return is_image;
}

static void
set_button_selected_style (GtkWidget *button, gboolean selected)
{
	GtkStyleContext *ctx = gtk_widget_get_style_context (button);
	if (selected) {
		gtk_style_context_add_class (ctx, "nemo-filmstrip-selected");
	} else {
		gtk_style_context_remove_class (ctx, "nemo-filmstrip-selected");
	}
}

static void
filmstrip_clear_files (NemoFilmstripView *fview)
{
	GList *l;

	if (fview->details->selected) {
		nemo_file_unref (fview->details->selected);
		fview->details->selected = NULL;
	}
	if (fview->details->preview_pb) {
		g_object_unref (fview->details->preview_pb);
		fview->details->preview_pb = NULL;
	}
	for (l = fview->details->files; l != NULL; l = l->next) {
		nemo_file_unref (NEMO_FILE (l->data));
	}
	g_list_free (fview->details->files);
	fview->details->files = NULL;

	if (fview->details->buttons) {
		g_hash_table_remove_all (fview->details->buttons);
	}
}

static void
on_thumb_clicked (GtkButton *button, gpointer user_data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (user_data);
	NemoFile *file;

	file = g_object_get_data (G_OBJECT (button), "nemo-file");
	if (file != NULL) {
		filmstrip_select_file (fview, file, FALSE);
	}
}

static gboolean
on_thumb_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (user_data);
	NemoFile *file;

	file = g_object_get_data (G_OBJECT (widget), "nemo-file");
	if (file == NULL) {
		return FALSE;
	}

	if (event->button == 1) {
		filmstrip_select_file (fview, file, FALSE);
		if (event->type == GDK_2BUTTON_PRESS) {
			GList *files = g_list_prepend (NULL, nemo_file_ref (file));
			nemo_view_activate_files (NEMO_VIEW (fview), files, 0, TRUE);
			nemo_file_list_free (files);
		}
		return TRUE;
	}

	if (event->button == 3) {
		filmstrip_select_file (fview, file, FALSE);
		nemo_view_pop_up_selection_context_menu (NEMO_VIEW (fview), event);
		return TRUE;
	}

	return FALSE;
}

static int
filmstrip_get_scale (NemoFilmstripView *fview)
{
	int scale = 1;

	if (fview != NULL && GTK_IS_WIDGET (fview)) {
		scale = gtk_widget_get_scale_factor (GTK_WIDGET (fview));
	}
	if (scale < 1) {
		scale = 1;
	}
	return scale;
}

static GdkPixbuf *
filmstrip_load_pixbuf (NemoFilmstripView *fview,
		       NemoFile *file,
		       int size,
		       gboolean use_thumbnails)
{
	GdkPixbuf *pb;
	NemoFileIconFlags flags = NEMO_FILE_ICON_FLAGS_NONE;
	int scale;

	if (file == NULL || size < 1) {
		return NULL;
	}
	if (use_thumbnails) {
		flags = NEMO_FILE_ICON_FLAGS_USE_THUMBNAILS;
	}
	scale = filmstrip_get_scale (fview);
	/* scale must be >= 1 or GTK asserts and can abort */
	pb = nemo_file_get_icon_pixbuf (file, size, TRUE, scale, flags);
	return pb;
}

typedef struct {
	NemoFilmstripView *fview; /* weak pointer */
	NemoFile *file;
	GtkWidget *image; /* weak: cleared if widget is destroyed */
	int size;
	guint generation;
	guint idle_id;
} ThumbLoadData;

static void
thumb_image_weak_notify (gpointer data, GObject *where_the_object_was)
{
	ThumbLoadData *td = data;

	if (td->image == (GtkWidget *) where_the_object_was) {
		td->image = NULL;
	}
}

static void
thumb_load_free (ThumbLoadData *data)
{
	if (data->idle_id != 0) {
		g_source_remove (data->idle_id);
		data->idle_id = 0;
	}
	if (data->image != NULL) {
		g_object_weak_unref (G_OBJECT (data->image),
				     thumb_image_weak_notify, data);
		data->image = NULL;
	}
	if (data->fview != NULL) {
		g_object_remove_weak_pointer (G_OBJECT (data->fview),
					      (gpointer *) &data->fview);
		data->fview = NULL;
	}
	if (data->file != NULL) {
		nemo_file_unref (data->file);
		data->file = NULL;
	}
	g_free (data);
}

static void
filmstrip_cancel_thumb_loads (NemoFilmstripView *fview)
{
	GList *loads;
	GList *l;

	loads = fview->details->thumb_loads;
	fview->details->thumb_loads = NULL;
	for (l = loads; l != NULL; l = l->next) {
		thumb_load_free (l->data);
	}
	g_list_free (loads);
}

static gboolean
thumb_load_idle_cb (gpointer user_data)
{
	ThumbLoadData *data = user_data;
	GdkPixbuf *pb = NULL;
	NemoFilmstripView *fview;

	data->idle_id = 0;
	fview = data->fview;

	if (fview != NULL && fview->details != NULL) {
		fview->details->thumb_loads = g_list_remove (fview->details->thumb_loads, data);
	}

	if (fview != NULL &&
	    fview->details != NULL &&
	    data->generation == fview->details->thumb_generation &&
	    data->image != NULL &&
	    GTK_IS_IMAGE (data->image) &&
	    data->file != NULL) {
		pb = filmstrip_load_pixbuf (fview, data->file, data->size, TRUE);
		if (pb != NULL && data->image != NULL && GTK_IS_IMAGE (data->image)) {
			gtk_image_set_from_pixbuf (GTK_IMAGE (data->image), pb);
		}
		if (pb != NULL) {
			g_object_unref (pb);
		}
	}

	thumb_load_free (data);
	return G_SOURCE_REMOVE;
}

static void
schedule_thumb_load (NemoFilmstripView *fview, NemoFile *file, GtkWidget *image, int size)
{
	ThumbLoadData *data;

	data = g_new0 (ThumbLoadData, 1);
	data->fview = fview;
	g_object_add_weak_pointer (G_OBJECT (fview), (gpointer *) &data->fview);
	data->file = nemo_file_ref (file);
	data->image = image;
	data->size = size;
	data->generation = fview->details->thumb_generation;
	g_object_weak_ref (G_OBJECT (image), thumb_image_weak_notify, data);
	fview->details->thumb_loads = g_list_prepend (fview->details->thumb_loads, data);
	data->idle_id = g_idle_add_full (G_PRIORITY_LOW, thumb_load_idle_cb, data, NULL);
}

static GtkWidget *
make_thumb_button (NemoFilmstripView *fview, NemoFile *file)
{
	GtkWidget *button;
	GtkWidget *box;
	GtkWidget *image;
	GtkWidget *label;
	char *name;
	const char *icon_name;
	int size = THUMB_SIZE + fview->details->zoom_delta * 16;

	if (size < 48) size = 48;
	if (size > 128) size = 128; /* cap for performance */

	button = gtk_button_new ();
	gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click (button, FALSE);
	gtk_widget_set_size_request (button, size + 12, size + 28);

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	gtk_container_set_border_width (GTK_CONTAINER (box), 2);

	/* Placeholder icon immediately (no heavy load on main path) */
	if (file_is_image (file)) {
		icon_name = "image-x-generic";
	} else if (nemo_file_is_directory (file)) {
		icon_name = "folder";
	} else {
		icon_name = "text-x-generic";
	}
	image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_DIALOG);
	gtk_box_pack_start (GTK_BOX (box), image, TRUE, TRUE, 0);

	name = nemo_file_get_display_name (file);
	label = gtk_label_new (name != NULL ? name : "");
	g_free (name);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 12);
	gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_halign (label, GTK_ALIGN_CENTER);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	gtk_container_add (GTK_CONTAINER (button), box);

	g_object_set_data (G_OBJECT (button), "nemo-file", file);
	g_object_set_data (G_OBJECT (button), "thumb-image", image);
	g_signal_connect (button, "clicked", G_CALLBACK (on_thumb_clicked), fview);
	g_signal_connect (button, "button-press-event", G_CALLBACK (on_thumb_button_press), fview);

	/* Load real thumbnail later, in idle */
	schedule_thumb_load (fview, file, image, size);

	gtk_widget_show_all (button);
	return button;
}

static void
filmstrip_rebuild_strip (NemoFilmstripView *fview)
{
	GList *l;
	GList *children;

	/* Invalidate all pending async thumbnail loads */
	fview->details->thumb_generation++;
	filmstrip_cancel_thumb_loads (fview);

	/* clear strip */
	children = gtk_container_get_children (GTK_CONTAINER (fview->details->strip_box));
	for (l = children; l != NULL; l = l->next) {
		gtk_widget_destroy (GTK_WIDGET (l->data));
	}
	g_list_free (children);
	g_hash_table_remove_all (fview->details->buttons);

	gtk_widget_set_no_show_all (fview->details->strip_box, TRUE);
	gtk_widget_freeze_child_notify (fview->details->strip_box);

	for (l = fview->details->files; l != NULL; l = l->next) {
		NemoFile *file = NEMO_FILE (l->data);
		GtkWidget *btn = make_thumb_button (fview, file);
		gtk_box_pack_start (GTK_BOX (fview->details->strip_box), btn, FALSE, FALSE, 2);
		g_hash_table_insert (fview->details->buttons, file, btn);
		if (file == fview->details->selected) {
			set_button_selected_style (btn, TRUE);
		}
	}

	gtk_widget_thaw_child_notify (fview->details->strip_box);
	gtk_widget_set_no_show_all (fview->details->strip_box, FALSE);
	gtk_widget_show_all (fview->details->strip_box);
}

static void
filmstrip_scroll_to_selected (NemoFilmstripView *fview)
{
	GtkWidget *btn;
	GtkAdjustment *hadj;
	GtkAllocation alloc;
	gdouble page, target;

	if (fview->details->selected == NULL) {
		return;
	}
	btn = g_hash_table_lookup (fview->details->buttons, fview->details->selected);
	if (btn == NULL) {
		return;
	}

	hadj = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (fview->details->strip_scrolled));
	gtk_widget_get_allocation (btn, &alloc);
	page = gtk_adjustment_get_page_size (hadj);
	target = alloc.x + alloc.width / 2.0 - page / 2.0;
	target = CLAMP (target,
			gtk_adjustment_get_lower (hadj),
			MAX (gtk_adjustment_get_lower (hadj),
			     gtk_adjustment_get_upper (hadj) - page));
	gtk_adjustment_set_value (hadj, target);
}

typedef struct {
	char *path;
	int max_w;
	int max_h;
	guint generation;
} PreviewJob;

static void
preview_job_free (gpointer data)
{
	PreviewJob *job = data;
	g_free (job->path);
	g_free (job);
}

static void
preview_job_thread (GTask *task,
		    gpointer source_object,
		    gpointer task_data,
		    GCancellable *cancellable)
{
	PreviewJob *job = task_data;
	GError *error = NULL;
	GdkPixbuf *pb;

	if (g_cancellable_is_cancelled (cancellable)) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
		return;
	}

	pb = gdk_pixbuf_new_from_file_at_scale (job->path, job->max_w, job->max_h, TRUE, &error);
	if (pb == NULL) {
		g_task_return_error (task, error);
		return;
	}
	g_task_return_pointer (task, pb, g_object_unref);
}

static void
preview_job_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
	NemoFilmstripView *fview;
	PreviewJob *job;
	GdkPixbuf *pb;
	GError *error = NULL;
	char *name, *info;

	fview = NEMO_FILMSTRIP_VIEW (source);
	job = g_task_get_task_data (G_TASK (res));
	pb = g_task_propagate_pointer (G_TASK (res), &error);
	if (error != NULL) {
		g_error_free (error);
		return;
	}
	if (!NEMO_IS_FILMSTRIP_VIEW (fview) ||
	    fview->details == NULL ||
	    job == NULL ||
	    job->generation != fview->details->preview_generation ||
	    fview->details->preview_image == NULL ||
	    !GTK_IS_IMAGE (fview->details->preview_image)) {
		if (pb != NULL) {
			g_object_unref (pb);
		}
		return;
	}
	if (pb == NULL) {
		return;
	}

	if (fview->details->preview_pb != NULL) {
		g_object_unref (fview->details->preview_pb);
	}
	fview->details->preview_pb = pb;
	gtk_image_set_from_pixbuf (GTK_IMAGE (fview->details->preview_image), pb);

	name = fview->details->selected ? nemo_file_get_display_name (fview->details->selected) : NULL;
	info = g_strdup_printf ("%s  —  %d × %d",
				name != NULL ? name : "",
				gdk_pixbuf_get_width (pb),
				gdk_pixbuf_get_height (pb));
	gtk_label_set_text (GTK_LABEL (fview->details->info_label), info);
	g_free (info);
	g_free (name);
}

static void
filmstrip_cancel_preview_load (NemoFilmstripView *fview)
{
	fview->details->preview_generation++;
	if (fview->details->preview_cancellable != NULL) {
		g_cancellable_cancel (fview->details->preview_cancellable);
		g_clear_object (&fview->details->preview_cancellable);
	}
}

static void
filmstrip_set_themed_icon (NemoFilmstripView *fview, const char *icon_name)
{
	GtkIconTheme *theme;
	GdkPixbuf *icon;
	int scale;
	int size = 256;

	theme = gtk_icon_theme_get_default ();
	scale = filmstrip_get_scale (fview);
	icon = gtk_icon_theme_load_icon_for_scale (theme, icon_name, size, scale,
						   GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
	if (icon != NULL) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (fview->details->preview_image), icon);
		g_object_unref (icon);
		return;
	}
	gtk_image_set_from_icon_name (GTK_IMAGE (fview->details->preview_image),
				      icon_name, GTK_ICON_SIZE_DIALOG);
}

static void
filmstrip_update_preview (NemoFilmstripView *fview)
{
	NemoFile *file = fview->details->selected;
	GtkAllocation alloc;
	int max_w, max_h;
	GdkPixbuf *pb;
	char *name, *info;
	goffset size;
	char *size_str;
	GFile *location;
	char *path;
	PreviewJob *job;
	GTask *task;

	filmstrip_cancel_preview_load (fview);

	if (fview->details->preview_pb) {
		g_object_unref (fview->details->preview_pb);
		fview->details->preview_pb = NULL;
	}

	if (file == NULL) {
		gtk_image_clear (GTK_IMAGE (fview->details->preview_image));
		gtk_label_set_text (GTK_LABEL (fview->details->info_label),
				    _("Aucune sélection"));
		return;
	}

	name = nemo_file_get_display_name (file);
	size = nemo_file_get_size (file);
	if (size < 1024) {
		size_str = g_strdup_printf ("%ld o", (long) size);
	} else if (size < 1024 * 1024) {
		size_str = g_strdup_printf ("%.1f Ko", size / 1024.0);
	} else {
		size_str = g_strdup_printf ("%.1f Mo", size / (1024.0 * 1024.0));
	}
	info = g_strdup_printf ("%s  —  %s", name != NULL ? name : "", size_str);
	gtk_label_set_text (GTK_LABEL (fview->details->info_label), info);
	g_free (info);
	g_free (size_str);
	g_free (name);

	if (nemo_file_is_directory (file)) {
		filmstrip_set_themed_icon (fview, "folder");
		return;
	}

	/* Instant: cached 128px thumb / generic icon. Never request a huge
	 * size here — that would force Nemo to decode the original image
	 * on the UI thread. */
	pb = filmstrip_load_pixbuf (fview, file, 128, TRUE);
	if (pb != NULL) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (fview->details->preview_image), pb);
		g_object_unref (pb);
	} else if (file_is_image (file)) {
		filmstrip_set_themed_icon (fview, "image-x-generic");
	} else {
		filmstrip_set_themed_icon (fview, "text-x-generic");
	}

	if (!file_is_image (file)) {
		return;
	}

	location = nemo_file_get_location (file);
	path = location != NULL ? g_file_get_path (location) : NULL;
	if (location != NULL) {
		g_object_unref (location);
	}
	if (path == NULL) {
		return;
	}

	gtk_widget_get_allocation (fview->details->preview_event, &alloc);
	max_w = MAX (alloc.width - PREVIEW_MARGIN * 2, 64);
	max_h = MAX (alloc.height - PREVIEW_MARGIN * 2, 64);
	if (max_w > PREVIEW_CAP) {
		max_w = PREVIEW_CAP;
	}
	if (max_h > PREVIEW_CAP) {
		max_h = PREVIEW_CAP;
	}
	fview->details->preview_req_w = max_w;
	fview->details->preview_req_h = max_h;

	job = g_new0 (PreviewJob, 1);
	job->path = path;
	job->max_w = max_w;
	job->max_h = max_h;
	job->generation = fview->details->preview_generation;

	fview->details->preview_cancellable = g_cancellable_new ();
	task = g_task_new (fview, fview->details->preview_cancellable,
			   preview_job_done, NULL);
	g_task_set_task_data (task, job, preview_job_free);
	g_task_run_in_thread (task, preview_job_thread);
	g_object_unref (task);

	{
		char *uri = nemo_file_get_uri (file);
		if (uri != NULL) {
			nemo_thumbnail_prioritize (uri);
			g_free (uri);
		}
	}
}

static void
filmstrip_select_file (NemoFilmstripView *fview, NemoFile *file, gboolean scroll)
{
	GtkWidget *btn;

	if (fview->details->selected == file) {
		if (scroll) {
			filmstrip_scroll_to_selected (fview);
		}
		return;
	}

	/* unselect previous */
	if (fview->details->selected != NULL) {
		btn = g_hash_table_lookup (fview->details->buttons, fview->details->selected);
		if (btn) {
			set_button_selected_style (btn, FALSE);
		}
		nemo_file_unref (fview->details->selected);
		fview->details->selected = NULL;
	}

	if (file != NULL) {
		fview->details->selected = nemo_file_ref (file);
		btn = g_hash_table_lookup (fview->details->buttons, file);
		if (btn) {
			set_button_selected_style (btn, TRUE);
		}
	}

	filmstrip_update_preview (fview);
	nemo_view_notify_selection_changed (NEMO_VIEW (fview));

	if (scroll) {
		filmstrip_scroll_to_selected (fview);
	}
}

static void
filmstrip_navigate (NemoFilmstripView *fview, int delta)
{
	GList *l;
	int index = -1, n, next;

	if (fview->details->files == NULL) {
		return;
	}
	n = g_list_length (fview->details->files);
	for (l = fview->details->files, index = 0; l != NULL; l = l->next, index++) {
		if (l->data == fview->details->selected) {
			break;
		}
	}
	if (l == NULL) {
		index = 0;
	}
	next = (index + delta) % n;
	if (next < 0) {
		next += n;
	}
	filmstrip_select_file (fview, NEMO_FILE (g_list_nth_data (fview->details->files, next)), TRUE);
}

static gboolean
filmstrip_handle_scroll (NemoFilmstripView *fview, GdkEventScroll *event)
{
	gdouble dx = 0, dy = 0;
	int dir = 0;

	if (fview == NULL || fview->details == NULL) {
		return FALSE;
	}

	/* Ctrl+molette = zoom, comme les autres vues */
	if (nemo_view_handle_scroll_event (NEMO_VIEW (fview), event)) {
		return TRUE;
	}

	switch (event->direction) {
	case GDK_SCROLL_UP:
	case GDK_SCROLL_LEFT:
		dir = -1;
		fview->details->scroll_acc = 0;
		break;
	case GDK_SCROLL_DOWN:
	case GDK_SCROLL_RIGHT:
		dir = 1;
		fview->details->scroll_acc = 0;
		break;
	case GDK_SCROLL_SMOOTH:
		gdk_event_get_scroll_deltas ((const GdkEvent *) event, &dx, &dy);
		/* Molette verticale, ou horizontale sur le pavé / tilt-wheel */
		if (fabs (dx) > fabs (dy)) {
			fview->details->scroll_acc += dx;
		} else {
			fview->details->scroll_acc += dy;
		}
		if (fview->details->scroll_acc <= -0.3) {
			dir = -1;
			fview->details->scroll_acc = 0;
		} else if (fview->details->scroll_acc >= 0.3) {
			dir = 1;
			fview->details->scroll_acc = 0;
		} else {
			/* Manger les micro-deltas pour ne pas les laisser à GtkScrolledWindow */
			return TRUE;
		}
		break;
	default:
		return FALSE;
	}

	if (dir != 0) {
		filmstrip_navigate (fview, dir);
	}
	return TRUE;
}

static gboolean
on_preview_scroll (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
	return filmstrip_handle_scroll (NEMO_FILMSTRIP_VIEW (user_data), event);
}

static gboolean
nemo_filmstrip_view_scroll_event (GtkWidget *widget, GdkEventScroll *event)
{
	return filmstrip_handle_scroll (NEMO_FILMSTRIP_VIEW (widget), event);
}

static void
on_nav_clicked (GtkButton *button, gpointer user_data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (user_data);
	int delta;

	delta = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "nav-delta"));
	filmstrip_navigate (fview, delta);
}

static GtkWidget *
make_nav_button (NemoFilmstripView *fview, const char *symbol, int delta, const char *tooltip)
{
	GtkWidget *button;
	GtkWidget *label;

	button = gtk_button_new ();
	gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
	gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
	gtk_widget_set_can_focus (button, FALSE);
	gtk_widget_set_tooltip_text (button, tooltip);
	gtk_style_context_add_class (gtk_widget_get_style_context (button),
				     "nemo-filmstrip-nav");
	g_object_set_data (G_OBJECT (button), "nav-delta", GINT_TO_POINTER (delta));

	label = gtk_label_new (symbol);
	gtk_style_context_add_class (gtk_widget_get_style_context (label),
				     "nemo-filmstrip-nav-symbol");
	gtk_container_add (GTK_CONTAINER (button), label);

	g_signal_connect (button, "clicked", G_CALLBACK (on_nav_clicked), fview);
	return button;
}

static gboolean
on_preview_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (user_data);
	int w;

	if (event->button == 1 && event->type == GDK_BUTTON_PRESS) {
		w = gtk_widget_get_allocated_width (widget);
		if (event->x < w / 2.0) {
			filmstrip_navigate (fview, -1);
		} else {
			filmstrip_navigate (fview, 1);
		}
		return TRUE;
	}
	if (event->button == 1 && event->type == GDK_2BUTTON_PRESS && fview->details->selected) {
		GList *files = g_list_prepend (NULL, nemo_file_ref (fview->details->selected));
		nemo_view_activate_files (NEMO_VIEW (fview), files, 0, TRUE);
		nemo_file_list_free (files);
		return TRUE;
	}
	if (event->button == 3) {
		if (fview->details->selected) {
			nemo_view_pop_up_selection_context_menu (NEMO_VIEW (fview), event);
		} else {
			nemo_view_pop_up_background_context_menu (NEMO_VIEW (fview), event);
		}
		return TRUE;
	}
	return FALSE;
}

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (user_data);

	switch (event->keyval) {
	case GDK_KEY_Left:
	case GDK_KEY_Up:
	case GDK_KEY_Page_Up:
		filmstrip_navigate (fview, -1);
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_Down:
	case GDK_KEY_Page_Down:
	case GDK_KEY_space:
		filmstrip_navigate (fview, 1);
		return TRUE;
	case GDK_KEY_Home:
		if (fview->details->files) {
			filmstrip_select_file (fview, NEMO_FILE (fview->details->files->data), TRUE);
		}
		return TRUE;
	case GDK_KEY_End:
		if (fview->details->files) {
			filmstrip_select_file (fview, NEMO_FILE (g_list_last (fview->details->files)->data), TRUE);
		}
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		if (fview->details->selected) {
			GList *files = g_list_prepend (NULL, nemo_file_ref (fview->details->selected));
			nemo_view_activate_files (NEMO_VIEW (fview), files, 0, TRUE);
			nemo_file_list_free (files);
		}
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

static gboolean
on_resize_idle (gpointer data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (data);
	GtkAllocation alloc;
	int max_w, max_h;

	fview->details->resize_idle_id = 0;
	gtk_widget_get_allocation (fview->details->preview_event, &alloc);
	max_w = MAX (alloc.width - PREVIEW_MARGIN * 2, 64);
	max_h = MAX (alloc.height - PREVIEW_MARGIN * 2, 64);
	if (max_w > PREVIEW_CAP) {
		max_w = PREVIEW_CAP;
	}
	if (max_h > PREVIEW_CAP) {
		max_h = PREVIEW_CAP;
	}
	/* Un redimensionnement minime ne doit pas annuler l'image déjà grande. */
	if (fview->details->preview_pb != NULL &&
	    ABS (max_w - fview->details->preview_req_w) < 48 &&
	    ABS (max_h - fview->details->preview_req_h) < 48) {
		return G_SOURCE_REMOVE;
	}
	filmstrip_update_preview (fview);
	return G_SOURCE_REMOVE;
}

static void
on_preview_size_allocate (GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (user_data);
	if (fview->details->resize_idle_id != 0) {
		g_source_remove (fview->details->resize_idle_id);
	}
	fview->details->resize_idle_id = g_timeout_add (80, on_resize_idle, fview);
}

static void
apply_css (void)
{
	static gboolean done = FALSE;
	GtkCssProvider *provider;
	const char *css =
		".nemo-filmstrip-preview-bg { background-color: #1a1a1a; }"
		".nemo-filmstrip-strip-bg { background-color: #2b2b2b; }"
		".nemo-filmstrip-selected {"
		"  background-color: alpha(@theme_selected_bg_color, 0.55);"
		"  border-radius: 4px;"
		"  border: 2px solid @theme_selected_bg_color;"
		"}"
		".nemo-filmstrip-nav {"
		"  background-color: rgba(0,0,0,0.28);"
		"  border: none;"
		"  border-radius: 12px;"
		"  padding: 8px 14px;"
		"  min-width: 72px;"
		"  min-height: 140px;"
		"  margin: 12px 8px;"
		"}"
		".nemo-filmstrip-nav:hover {"
		"  background-color: rgba(255,255,255,0.22);"
		"}"
		".nemo-filmstrip-nav:active {"
		"  background-color: rgba(255,255,255,0.35);"
		"}"
		".nemo-filmstrip-nav-symbol {"
		"  color: #f4f4f4;"
		"  font-size: 72px;"
		"  font-weight: bold;"
		"}";

	if (done) {
		return;
	}
	done = TRUE;
	provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider, css, -1, NULL);
	gtk_style_context_add_provider_for_screen (
		gdk_screen_get_default (),
		GTK_STYLE_PROVIDER (provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref (provider);
}

/* ---------------- NemoView vfuncs ---------------- */

static void
nemo_filmstrip_view_add_file (NemoView *view, NemoFile *file, NemoDirectory *directory)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);

	if (g_list_find (fview->details->files, file) != NULL) {
		return;
	}

	fview->details->files = g_list_insert_sorted (fview->details->files,
						      nemo_file_ref (file),
						      (GCompareFunc) compare_files_default);

	/* defer strip rebuild to end_file_changes for batch adds */
}

static void
nemo_filmstrip_view_begin_loading (NemoView *view)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	GList *l, *children;

	filmstrip_cancel_preview_load (fview);
	if (fview->details->rebuild_idle_id != 0) {
		g_source_remove (fview->details->rebuild_idle_id);
		fview->details->rebuild_idle_id = 0;
	}
	filmstrip_cancel_thumb_loads (fview);
	filmstrip_clear_files (fview);

	children = gtk_container_get_children (GTK_CONTAINER (fview->details->strip_box));
	for (l = children; l != NULL; l = l->next) {
		gtk_widget_destroy (GTK_WIDGET (l->data));
	}
	g_list_free (children);

	gtk_image_clear (GTK_IMAGE (fview->details->preview_image));
	gtk_label_set_text (GTK_LABEL (fview->details->info_label), _("Chargement…"));
}

static void
nemo_filmstrip_view_clear (NemoView *view)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	GList *l, *children;

	filmstrip_clear_files (fview);

	children = gtk_container_get_children (GTK_CONTAINER (fview->details->strip_box));
	for (l = children; l != NULL; l = l->next) {
		gtk_widget_destroy (GTK_WIDGET (l->data));
	}
	g_list_free (children);

	gtk_image_clear (GTK_IMAGE (fview->details->preview_image));
	gtk_label_set_text (GTK_LABEL (fview->details->info_label), "");
}

static void
nemo_filmstrip_view_file_changed (NemoView *view, NemoFile *file, NemoDirectory *directory)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	GtkWidget *btn;
	GtkWidget *image;
	GdkPixbuf *pb;
	int size;

	/* Never rebuild the whole strip here: thumbnailer completion would
	 * freeze the UI for several seconds on a folder of photos. */
	btn = g_hash_table_lookup (fview->details->buttons, file);
	if (btn == NULL) {
		return;
	}
	image = g_object_get_data (G_OBJECT (btn), "thumb-image");
	if (image == NULL || !GTK_IS_IMAGE (image)) {
		return;
	}
	size = THUMB_SIZE + fview->details->zoom_delta * 16;
	if (size < 48) {
		size = 48;
	}
	if (size > 128) {
		size = 128;
	}
	pb = filmstrip_load_pixbuf (fview, file, size, TRUE);
	if (pb != NULL) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (image), pb);
		g_object_unref (pb);
	}
}

static GList *
nemo_filmstrip_view_get_selection (NemoView *view)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);

	if (fview->details->selected == NULL) {
		return NULL;
	}
	return g_list_prepend (NULL, nemo_file_ref (fview->details->selected));
}

static GList *
nemo_filmstrip_view_get_selection_for_file_transfer (NemoView *view)
{
	return nemo_filmstrip_view_get_selection (view);
}

static guint
nemo_filmstrip_view_get_item_count (NemoView *view)
{
	return g_list_length (NEMO_FILMSTRIP_VIEW (view)->details->files);
}

static gboolean
nemo_filmstrip_view_is_empty (NemoView *view)
{
	return NEMO_FILMSTRIP_VIEW (view)->details->files == NULL;
}

static gboolean
filmstrip_rebuild_idle_cb (gpointer data)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (data);

	fview->details->rebuild_idle_id = 0;
	filmstrip_rebuild_strip (fview);

	if (fview->details->selected == NULL && fview->details->files != NULL) {
		filmstrip_select_file (fview, NEMO_FILE (fview->details->files->data), TRUE);
	} else if (fview->details->selected != NULL) {
		/* re-apply selection style after rebuild */
		GtkWidget *btn = g_hash_table_lookup (fview->details->buttons, fview->details->selected);
		if (btn != NULL) {
			set_button_selected_style (btn, TRUE);
		}
		/* Ne pas relancer l'aperçu : les mises à jour du dossier
		 * annulaient le décodage et laissaient la vignette de 128 px. */
	}
	return G_SOURCE_REMOVE;
}

static void
filmstrip_schedule_rebuild (NemoFilmstripView *fview)
{
	if (fview->details->rebuild_idle_id != 0) {
		g_source_remove (fview->details->rebuild_idle_id);
		fview->details->rebuild_idle_id = 0;
	}
	/* Coalesce the many end_file_changes Nemo fires while listing a folder */
	fview->details->rebuild_idle_id = g_timeout_add (120, filmstrip_rebuild_idle_cb, fview);
}

static void
nemo_filmstrip_view_end_file_changes (NemoView *view)
{
	/* Rebuild asynchronously so we don't block/crash mid-directory load */
	filmstrip_schedule_rebuild (NEMO_FILMSTRIP_VIEW (view));
}

static void
nemo_filmstrip_view_remove_file (NemoView *view, NemoFile *file, NemoDirectory *directory)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	GList *l;

	l = g_list_find (fview->details->files, file);
	if (l == NULL) {
		return;
	}

	if (fview->details->selected == file) {
		NemoFile *next = NULL;
		if (l->next) {
			next = l->next->data;
		} else if (l->prev) {
			next = l->prev->data;
		}
		filmstrip_select_file (fview, next, FALSE);
	}

	fview->details->files = g_list_delete_link (fview->details->files, l);
	nemo_file_unref (file);
	g_hash_table_remove (fview->details->buttons, file);
}

static void
nemo_filmstrip_view_set_selection (NemoView *view, GList *selection)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);

	if (selection != NULL) {
		filmstrip_select_file (fview, NEMO_FILE (selection->data), TRUE);
	} else {
		filmstrip_select_file (fview, NULL, FALSE);
	}
}

static void
nemo_filmstrip_view_select_all (NemoView *view)
{
	/* single-selection filmstrip: select first */
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	if (fview->details->files) {
		filmstrip_select_file (fview, NEMO_FILE (fview->details->files->data), TRUE);
	}
}

static void
nemo_filmstrip_view_reveal_selection (NemoView *view)
{
	filmstrip_scroll_to_selected (NEMO_FILMSTRIP_VIEW (view));
}

static void
nemo_filmstrip_view_merge_menus (NemoView *view)
{
	NEMO_VIEW_CLASS (nemo_filmstrip_view_parent_class)->merge_menus (view);
}

static void
nemo_filmstrip_view_update_menus (NemoView *view)
{
	NEMO_VIEW_CLASS (nemo_filmstrip_view_parent_class)->update_menus (view);
}

static void
nemo_filmstrip_view_reset_to_defaults (NemoView *view)
{
	NEMO_FILMSTRIP_VIEW (view)->details->zoom_delta = 0;
	filmstrip_rebuild_strip (NEMO_FILMSTRIP_VIEW (view));
	filmstrip_update_preview (NEMO_FILMSTRIP_VIEW (view));
}

static void
nemo_filmstrip_view_bump_zoom_level (NemoView *view, int zoom_increment)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	fview->details->zoom_delta += zoom_increment;
	if (fview->details->zoom_delta < -2) fview->details->zoom_delta = -2;
	if (fview->details->zoom_delta > 3) fview->details->zoom_delta = 3;
	filmstrip_rebuild_strip (fview);
	if (fview->details->selected) {
		GtkWidget *btn = g_hash_table_lookup (fview->details->buttons, fview->details->selected);
		if (btn) set_button_selected_style (btn, TRUE);
	}
	filmstrip_update_preview (fview);
}

static NemoZoomLevel
nemo_filmstrip_view_get_zoom_level (NemoView *view)
{
	return NEMO_ZOOM_LEVEL_STANDARD;
}

static void
nemo_filmstrip_view_zoom_to_level (NemoView *view, NemoZoomLevel zoom_level)
{
}

static void
nemo_filmstrip_view_restore_default_zoom_level (NemoView *view)
{
	NEMO_FILMSTRIP_VIEW (view)->details->zoom_delta = 0;
	filmstrip_rebuild_strip (NEMO_FILMSTRIP_VIEW (view));
}

static gboolean
nemo_filmstrip_view_can_zoom_in (NemoView *view)
{
	return NEMO_FILMSTRIP_VIEW (view)->details->zoom_delta < 3;
}

static gboolean
nemo_filmstrip_view_can_zoom_out (NemoView *view)
{
	return NEMO_FILMSTRIP_VIEW (view)->details->zoom_delta > -2;
}

static void
nemo_filmstrip_view_start_renaming_file (NemoView *view, NemoFile *file, gboolean select_all)
{
	/* not supported in filmstrip yet */
}

static void
nemo_filmstrip_view_click_policy_changed (NemoView *directory_view)
{
}

static int
nemo_filmstrip_view_compare_files (NemoView *view, NemoFile *file1, NemoFile *file2)
{
	return compare_files_default (file1, file2);
}

static gboolean
nemo_filmstrip_view_using_manual_layout (NemoView *view)
{
	return FALSE;
}

static void
nemo_filmstrip_view_end_loading (NemoView *view, gboolean all_files_seen)
{
	filmstrip_schedule_rebuild (NEMO_FILMSTRIP_VIEW (view));
}

static char *
nemo_filmstrip_view_get_first_visible_file (NemoView *view)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	if (fview->details->selected) {
		return nemo_file_get_uri (fview->details->selected);
	}
	if (fview->details->files) {
		return nemo_file_get_uri (NEMO_FILE (fview->details->files->data));
	}
	return NULL;
}

static void
nemo_filmstrip_view_scroll_to_file (NemoView *view, const char *uri)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (view);
	GList *l;

	for (l = fview->details->files; l != NULL; l = l->next) {
		char *file_uri = nemo_file_get_uri (NEMO_FILE (l->data));
		if (g_strcmp0 (file_uri, uri) == 0) {
			filmstrip_select_file (fview, NEMO_FILE (l->data), TRUE);
			g_free (file_uri);
			return;
		}
		g_free (file_uri);
	}
}

static void
nemo_filmstrip_view_sort_directories_first_changed (NemoView *view)
{
}

static const char *
nemo_filmstrip_view_get_id (NemoView *view)
{
	return NEMO_FILMSTRIP_VIEW_ID;
}

static void
nemo_filmstrip_view_dispose (GObject *object)
{
	NemoFilmstripView *fview = NEMO_FILMSTRIP_VIEW (object);

	if (fview->details->rebuild_idle_id != 0) {
		g_source_remove (fview->details->rebuild_idle_id);
		fview->details->rebuild_idle_id = 0;
	}
	if (fview->details->resize_idle_id != 0) {
		g_source_remove (fview->details->resize_idle_id);
		fview->details->resize_idle_id = 0;
	}

	filmstrip_cancel_preview_load (fview);
	filmstrip_cancel_thumb_loads (fview);
	filmstrip_clear_files (fview);
	fview->details->preview_image = NULL;
	fview->details->info_label = NULL;

	if (fview->details->buttons) {
		g_hash_table_destroy (fview->details->buttons);
		fview->details->buttons = NULL;
	}

	G_OBJECT_CLASS (nemo_filmstrip_view_parent_class)->dispose (object);
}

static void
nemo_filmstrip_view_class_init (NemoFilmstripViewClass *class)
{
	NemoViewClass *nemo_view_class;
	GObjectClass *oclass;
	GtkWidgetClass *widget_class;

	g_type_class_add_private (class, sizeof (NemoFilmstripViewDetails));

	oclass = G_OBJECT_CLASS (class);
	oclass->dispose = nemo_filmstrip_view_dispose;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->scroll_event = nemo_filmstrip_view_scroll_event;

	nemo_view_class = NEMO_VIEW_CLASS (class);

	nemo_view_class->add_file = nemo_filmstrip_view_add_file;
	nemo_view_class->begin_loading = nemo_filmstrip_view_begin_loading;
	nemo_view_class->bump_zoom_level = nemo_filmstrip_view_bump_zoom_level;
	nemo_view_class->can_zoom_in = nemo_filmstrip_view_can_zoom_in;
	nemo_view_class->can_zoom_out = nemo_filmstrip_view_can_zoom_out;
	nemo_view_class->click_policy_changed = nemo_filmstrip_view_click_policy_changed;
	nemo_view_class->clear = nemo_filmstrip_view_clear;
	nemo_view_class->file_changed = nemo_filmstrip_view_file_changed;
	nemo_view_class->get_selection = nemo_filmstrip_view_get_selection;
	nemo_view_class->get_selection_for_file_transfer = nemo_filmstrip_view_get_selection_for_file_transfer;
	nemo_view_class->get_item_count = nemo_filmstrip_view_get_item_count;
	nemo_view_class->is_empty = nemo_filmstrip_view_is_empty;
	nemo_view_class->remove_file = nemo_filmstrip_view_remove_file;
	nemo_view_class->merge_menus = nemo_filmstrip_view_merge_menus;
	nemo_view_class->update_menus = nemo_filmstrip_view_update_menus;
	nemo_view_class->reset_to_defaults = nemo_filmstrip_view_reset_to_defaults;
	nemo_view_class->restore_default_zoom_level = nemo_filmstrip_view_restore_default_zoom_level;
	nemo_view_class->reveal_selection = nemo_filmstrip_view_reveal_selection;
	nemo_view_class->select_all = nemo_filmstrip_view_select_all;
	nemo_view_class->set_selection = nemo_filmstrip_view_set_selection;
	nemo_view_class->compare_files = nemo_filmstrip_view_compare_files;
	nemo_view_class->sort_directories_first_changed = nemo_filmstrip_view_sort_directories_first_changed;
	nemo_view_class->start_renaming_file = nemo_filmstrip_view_start_renaming_file;
	nemo_view_class->get_zoom_level = nemo_filmstrip_view_get_zoom_level;
	nemo_view_class->zoom_to_level = nemo_filmstrip_view_zoom_to_level;
	nemo_view_class->end_file_changes = nemo_filmstrip_view_end_file_changes;
	nemo_view_class->using_manual_layout = nemo_filmstrip_view_using_manual_layout;
	nemo_view_class->end_loading = nemo_filmstrip_view_end_loading;
	nemo_view_class->get_view_id = nemo_filmstrip_view_get_id;
	nemo_view_class->get_first_visible_file = nemo_filmstrip_view_get_first_visible_file;
	nemo_view_class->scroll_to_file = nemo_filmstrip_view_scroll_to_file;
}

static void
nemo_filmstrip_view_init (NemoFilmstripView *fview)
{
	GtkWidget *vbox;
	GtkWidget *info_bar;
	GtkWidget *sep;

	fview->details = G_TYPE_INSTANCE_GET_PRIVATE (fview, NEMO_TYPE_FILMSTRIP_VIEW,
						      NemoFilmstripViewDetails);
	fview->details->buttons = g_hash_table_new (g_direct_hash, g_direct_equal);
	fview->details->zoom_delta = 0;
	fview->details->scroll_acc = 0;

	apply_css ();

	/* Outer scrolled window won't scroll — our layout manages it */
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (fview),
					GTK_POLICY_NEVER, GTK_POLICY_NEVER);

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	fview->details->root = vbox;

	/* Info bar */
	info_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (info_bar), 6);
	fview->details->info_label = gtk_label_new ("");
	gtk_label_set_ellipsize (GTK_LABEL (fview->details->info_label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign (GTK_LABEL (fview->details->info_label), 0.0);
	gtk_box_pack_start (GTK_BOX (info_bar), fview->details->info_label, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (vbox), info_bar, FALSE, FALSE, 0);

	sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start (GTK_BOX (vbox), sep, FALSE, FALSE, 0);

	/* Preview :  <  image  >  */
	fview->details->preview_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class (gtk_widget_get_style_context (fview->details->preview_row),
				     "nemo-filmstrip-preview-bg");

	fview->details->nav_prev = make_nav_button (fview, "<", -1,
						    _("Image précédente"));
	fview->details->nav_next = make_nav_button (fview, ">", 1,
						    _("Image suivante"));

	fview->details->preview_event = gtk_event_box_new ();
	gtk_widget_add_events (fview->details->preview_event,
			       GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
			       GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
	gtk_widget_set_can_focus (fview->details->preview_event, TRUE);
	gtk_event_box_set_visible_window (GTK_EVENT_BOX (fview->details->preview_event), TRUE);
	gtk_widget_set_hexpand (fview->details->preview_event, TRUE);
	gtk_widget_set_vexpand (fview->details->preview_event, TRUE);

	fview->details->preview_image = gtk_image_new ();
	gtk_widget_set_halign (fview->details->preview_image, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (fview->details->preview_image, GTK_ALIGN_CENTER);
	gtk_container_add (GTK_CONTAINER (fview->details->preview_event), fview->details->preview_image);

	gtk_box_pack_start (GTK_BOX (fview->details->preview_row),
			    fview->details->nav_prev, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (fview->details->preview_row),
			    fview->details->preview_event, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (fview->details->preview_row),
			    fview->details->nav_next, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (vbox), fview->details->preview_row, TRUE, TRUE, 0);

	g_signal_connect (fview->details->preview_event, "scroll-event",
			  G_CALLBACK (on_preview_scroll), fview);
	g_signal_connect (fview->details->preview_event, "button-press-event",
			  G_CALLBACK (on_preview_button_press), fview);
	g_signal_connect (fview->details->preview_event, "key-press-event",
			  G_CALLBACK (on_key_press), fview);
	g_signal_connect (fview->details->preview_event, "size-allocate",
			  G_CALLBACK (on_preview_size_allocate), fview);

	sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start (GTK_BOX (vbox), sep, FALSE, FALSE, 0);

	/* Filmstrip */
	fview->details->strip_scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (fview->details->strip_scrolled),
					GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (fview->details->strip_scrolled),
						    STRIP_HEIGHT);
	gtk_style_context_add_class (gtk_widget_get_style_context (fview->details->strip_scrolled),
				     "nemo-filmstrip-strip-bg");

	fview->details->strip_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_container_set_border_width (GTK_CONTAINER (fview->details->strip_box), 8);
	gtk_container_add (GTK_CONTAINER (fview->details->strip_scrolled), fview->details->strip_box);
	gtk_box_pack_start (GTK_BOX (vbox), fview->details->strip_scrolled, FALSE, FALSE, 0);

	gtk_widget_add_events (fview->details->strip_scrolled,
			       GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
	g_signal_connect (fview->details->strip_scrolled, "scroll-event",
			  G_CALLBACK (on_preview_scroll), fview);

	gtk_container_add (GTK_CONTAINER (fview), vbox);
	gtk_widget_show_all (vbox);

	gtk_widget_add_events (GTK_WIDGET (fview),
			       GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

	/* also handle keys on the view itself */
	g_signal_connect (fview, "key-press-event", G_CALLBACK (on_key_press), fview);
}

static NemoView *
nemo_filmstrip_view_create (NemoWindowSlot *slot)
{
	NemoFilmstripView *view;

	g_assert (NEMO_IS_WINDOW_SLOT (slot));

	view = g_object_new (NEMO_TYPE_FILMSTRIP_VIEW,
			     "window-slot", slot,
			     NULL);
	return NEMO_VIEW (view);
}

static gboolean
nemo_filmstrip_view_supports_uri (const char *uri,
				  GFileType file_type,
				  const char *mime_type)
{
	if (file_type == G_FILE_TYPE_DIRECTORY) {
		return TRUE;
	}
	if (g_str_has_prefix (uri, "trash:")) {
		return TRUE;
	}
	if (g_str_has_prefix (uri, EEL_SEARCH_URI)) {
		return TRUE;
	}
	return FALSE;
}

#define TRANSLATE_VIEW_INFO(view_info) \
	view_info.view_combo_label = _(view_info.view_combo_label); \
	view_info.view_menu_label_with_mnemonic = _(view_info.view_menu_label_with_mnemonic); \
	view_info.error_label = _(view_info.error_label); \
	view_info.startup_error_label = _(view_info.startup_error_label); \
	view_info.display_location_label = _(view_info.display_location_label);

static NemoViewInfo nemo_filmstrip_view_info = {
	(char *) NEMO_FILMSTRIP_VIEW_ID,
	/* translators: view selection dropdown / preferences */
	(char *) N_("Pellicule"),
	/* translators: View menu */
	(char *) N_("_Pellicule"),
	(char *) N_("La vue pellicule a rencontré une erreur."),
	(char *) N_("La vue pellicule a rencontré une erreur au démarrage."),
	(char *) N_("Afficher cet emplacement en mode pellicule."),
	nemo_filmstrip_view_create,
	nemo_filmstrip_view_supports_uri
};

void
nemo_filmstrip_view_register (void)
{
	TRANSLATE_VIEW_INFO (nemo_filmstrip_view_info)
	nemo_view_factory_register (&nemo_filmstrip_view_info);
}
