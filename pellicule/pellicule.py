#!/usr/bin/env python3
# Pellicule — visionneuse photos / vidéos / audio indépendante.
# Lecture via GStreamer (codecs du système : good/bad/ugly/libav).
# Si GStreamer échoue : ouverture dans VLC.

import gi
import hashlib
import mimetypes
import os
import subprocess
import sys

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gtk, Gdk, GdkPixbuf, Gio, GLib, GObject

GST_OK = False
try:
    gi.require_version("Gst", "1.0")
    from gi.repository import Gst

    Gst.init(None)
    GST_OK = Gst.ElementFactory.find("playbin") is not None
except Exception:
    GST_OK = False

APP_ID = "org.nemoplus.Pellicule"
THUMB = 96
IMAGE_EXT = {
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tif", ".tiff",
    ".svg", ".heic", ".heif", ".avif", ".jxl", ".ico",
}
VIDEO_EXT = {
    ".mp4", ".mkv", ".webm", ".avi", ".mov", ".wmv", ".m4v", ".mpeg",
    ".mpg", ".flv", ".ts", ".m2ts", ".3gp", ".ogv", ".vob",
}
AUDIO_EXT = {
    ".mp3", ".flac", ".ogg", ".oga", ".wav", ".m4a", ".aac", ".wma",
    ".opus", ".aiff", ".ape",
}

CSS = """
.pellicule-preview { background-color: #1a1a1a; }
.pellicule-strip { background-color: #2b2b2b; }
.pellicule-selected {
  background-color: alpha(@theme_selected_bg_color, 0.55);
  border-radius: 4px;
  border: 2px solid @theme_selected_bg_color;
}
.pellicule-nav {
  background-color: rgba(0,0,0,0.28);
  border: none;
  border-radius: 12px;
  min-width: 64px;
  min-height: 120px;
  margin: 8px;
  font-size: 48px;
  color: #f4f4f4;
  font-weight: bold;
}
.pellicule-nav:hover { background-color: rgba(255,255,255,0.22); }
.pellicule-info { color: #dddddd; padding: 4px 12px; }
"""


def content_kind(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in IMAGE_EXT:
        return "image"
    if ext in VIDEO_EXT:
        return "video"
    if ext in AUDIO_EXT:
        return "audio"
    ctype, _ = Gio.content_type_guess(path, None)
    if ctype:
        if ctype.startswith("image/"):
            return "image"
        if ctype.startswith("video/"):
            return "video"
        if ctype.startswith("audio/"):
            return "audio"
    mime, _ = mimetypes.guess_type(path)
    if mime:
        if mime.startswith("image/"):
            return "image"
        if mime.startswith("video/"):
            return "video"
        if mime.startswith("audio/"):
            return "audio"
    return None


def list_media(folder):
    items = []
    try:
        names = os.listdir(folder)
    except OSError:
        return items
    for name in names:
        if name.startswith("."):
            continue
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            continue
        kind = content_kind(path)
        if kind:
            items.append((path, kind))
    items.sort(key=lambda x: os.path.basename(x[0]).casefold())
    return items


def thumb_cache_path(path):
    uri = Gio.File.new_for_path(path).get_uri()
    digest = hashlib.md5(uri.encode("utf-8")).hexdigest()
    home = os.path.expanduser("~")
    for sub in ("large", "normal"):
        cand = os.path.join(home, ".cache", "thumbnails", sub, digest + ".png")
        if os.path.isfile(cand):
            return cand
    return None


def load_thumb(path, kind, size=THUMB):
    try:
        if kind == "image":
            return GdkPixbuf.Pixbuf.new_from_file_at_scale(path, size, size, True)
        cached = thumb_cache_path(path)
        if cached:
            return GdkPixbuf.Pixbuf.new_from_file_at_scale(cached, size, size, True)
    except Exception:
        pass
    icon = "video-x-generic" if kind == "video" else (
        "audio-x-generic" if kind == "audio" else "image-x-generic"
    )
    theme = Gtk.IconTheme.get_default()
    try:
        return theme.load_icon(icon, size, 0)
    except Exception:
        return None


def format_size(n):
    if n < 1024:
        return "%d o" % n
    if n < 1024 * 1024:
        return "%.1f Ko" % (n / 1024.0)
    return "%.1f Mo" % (n / (1024.0 * 1024.0))


class PelliculeWindow(Gtk.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Pellicule")
        self.set_default_size(1000, 720)
        self.set_icon_name("applications-multimedia")
        GLib.set_prgname("pellicule")
        self.folder = None
        self.items = []
        self.index = -1
        self.playbin = None
        self.video_widget = None
        self._alloc_id = 0

        provider = Gtk.CssProvider()
        provider.load_from_data(CSS.encode("utf-8"))
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

        header = Gtk.HeaderBar()
        header.set_show_close_button(True)
        header.set_title("Pellicule")
        self.set_titlebar(header)

        open_btn = Gtk.Button.new_from_icon_name("folder-open-symbolic", Gtk.IconSize.BUTTON)
        open_btn.set_tooltip_text("Ouvrir un dossier")
        open_btn.connect("clicked", self.on_open)
        header.pack_start(open_btn)

        self.play_btn = Gtk.Button.new_from_icon_name("media-playback-start-symbolic", Gtk.IconSize.BUTTON)
        self.play_btn.set_tooltip_text("Lecture / pause (Espace)")
        self.play_btn.connect("clicked", self.on_play_pause)
        header.pack_start(self.play_btn)

        vlc_btn = Gtk.Button.new_with_label("VLC")
        vlc_btn.set_tooltip_text("Ouvrir le fichier actuel dans VLC")
        vlc_btn.connect("clicked", self.on_open_vlc)
        header.pack_end(vlc_btn)

        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.add(vbox)

        self.info = Gtk.Label(label="Ouvrir un dossier d’images, de vidéos ou de musique")
        self.info.set_halign(Gtk.Align.START)
        self.info.get_style_context().add_class("pellicule-info")
        vbox.pack_start(self.info, False, False, 0)

        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        row.get_style_context().add_class("pellicule-preview")
        vbox.pack_start(row, True, True, 0)

        self.prev_btn = Gtk.Button(label="<")
        self.prev_btn.get_style_context().add_class("pellicule-nav")
        self.prev_btn.set_valign(Gtk.Align.CENTER)
        self.prev_btn.connect("clicked", lambda *_: self.step(-1))
        row.pack_start(self.prev_btn, False, False, 0)

        self.preview_stack = Gtk.Stack()
        self.preview_stack.set_hexpand(True)
        self.preview_stack.set_vexpand(True)
        row.pack_start(self.preview_stack, True, True, 0)

        self.image = Gtk.Image()
        self.image.set_halign(Gtk.Align.CENTER)
        self.image.set_valign(Gtk.Align.CENTER)
        ev = Gtk.EventBox()
        ev.add(self.image)
        ev.add_events(Gdk.EventMask.SCROLL_MASK | Gdk.EventMask.SMOOTH_SCROLL_MASK |
                      Gdk.EventMask.BUTTON_PRESS_MASK)
        ev.connect("scroll-event", self.on_scroll)
        ev.connect("button-press-event", self.on_preview_click)
        ev.connect("size-allocate", self.on_preview_alloc)
        self.preview_stack.add_named(ev, "image")

        self.video_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.video_box.set_hexpand(True)
        self.video_box.set_vexpand(True)
        self.preview_stack.add_named(self.video_box, "video")

        self.next_btn = Gtk.Button(label=">")
        self.next_btn.get_style_context().add_class("pellicule-nav")
        self.next_btn.set_valign(Gtk.Align.CENTER)
        self.next_btn.connect("clicked", lambda *_: self.step(1))
        row.pack_start(self.next_btn, False, False, 0)

        scrolled = Gtk.ScrolledWindow()
        scrolled.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.NEVER)
        scrolled.set_min_content_height(THUMB + 36)
        scrolled.get_style_context().add_class("pellicule-strip")
        scrolled.add_events(Gdk.EventMask.SCROLL_MASK | Gdk.EventMask.SMOOTH_SCROLL_MASK)
        scrolled.connect("scroll-event", self.on_scroll)
        vbox.pack_start(scrolled, False, False, 0)
        self.strip = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        self.strip.set_margin_top(6)
        self.strip.set_margin_bottom(6)
        self.strip.set_margin_start(8)
        self.strip.set_margin_end(8)
        scrolled.add(self.strip)
        self.strip_scroll = scrolled

        self.connect("key-press-event", self.on_key)
        self.connect("destroy", self.on_destroy)
        self.setup_player()

        self.drag_dest_set(Gtk.DestDefaults.ALL, [], Gdk.DragAction.COPY)
        self.drag_dest_add_uri_targets()
        self.connect("drag-data-received", self.on_drop)

    def setup_player(self):
        if not GST_OK:
            return
        self.playbin = Gst.ElementFactory.make("playbin", "player")
        sink = Gst.ElementFactory.make("gtksink", "sink")
        if self.playbin is None:
            return
        if sink is not None:
            self.playbin.set_property("video-sink", sink)
            self.video_widget = sink.get_property("widget")
            if self.video_widget is not None:
                self.video_widget.set_hexpand(True)
                self.video_widget.set_vexpand(True)
                self.video_box.pack_start(self.video_widget, True, True, 0)
        bus = self.playbin.get_bus()
        bus.add_signal_watch()
        bus.connect("message::error", self.on_gst_error)
        bus.connect("message::eos", self.on_gst_eos)
        bus.connect("message::state-changed", self.on_gst_state)

    def stop_player(self):
        if self.playbin is not None:
            self.playbin.set_state(Gst.State.NULL)
        self.play_btn.set_image(
            Gtk.Image.new_from_icon_name("media-playback-start-symbolic", Gtk.IconSize.BUTTON)
        )

    def on_destroy(self, *_args):
        self.stop_player()

    def on_open(self, *_args):
        dlg = Gtk.FileChooserDialog(
            title="Ouvrir un dossier",
            parent=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER,
        )
        dlg.add_buttons(
            Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
            Gtk.STOCK_OPEN, Gtk.ResponseType.OK,
        )
        if self.folder:
            dlg.set_current_folder(self.folder)
        resp = dlg.run()
        path = dlg.get_filename() if resp == Gtk.ResponseType.OK else None
        dlg.destroy()
        if path:
            self.load_folder(path)

    def on_drop(self, _w, _ctx, _x, _y, data, _info, _time):
        uris = data.get_uris()
        if not uris:
            return
        f = Gio.File.new_for_uri(uris[0])
        path = f.get_path()
        if path and os.path.isdir(path):
            self.load_folder(path)
        elif path and os.path.isfile(path):
            self.load_folder(os.path.dirname(path), select=path)

    def load_folder(self, folder, select=None):
        self.stop_player()
        self.folder = folder
        self.items = list_media(folder)
        self.index = -1
        self.rebuild_strip()
        if not self.items:
            self.info.set_text("Aucun média dans %s" % folder)
            self.image.clear()
            self.preview_stack.set_visible_child_name("image")
            return
        start = 0
        if select:
            for i, (p, _k) in enumerate(self.items):
                if os.path.samefile(p, select) if os.path.exists(p) else p == select:
                    start = i
                    break
        self.select_index(start)

    def rebuild_strip(self):
        for child in list(self.strip.get_children()):
            self.strip.remove(child)
        for i, (path, kind) in enumerate(self.items):
            btn = Gtk.Button()
            btn.set_relief(Gtk.ReliefStyle.NONE)
            img = Gtk.Image()
            pb = load_thumb(path, kind)
            if pb is not None:
                img.set_from_pixbuf(pb)
            btn.add(img)
            btn.set_tooltip_text(os.path.basename(path))
            btn.connect("clicked", self._on_thumb, i)
            self.strip.pack_start(btn, False, False, 0)
        self.strip.show_all()

    def _on_thumb(self, _btn, index):
        self.select_index(index)

    def mark_selected(self):
        children = self.strip.get_children()
        for i, btn in enumerate(children):
            ctx = btn.get_style_context()
            if i == self.index:
                ctx.add_class("pellicule-selected")
                adj = self.strip_scroll.get_hadjustment()
                alloc = btn.get_allocation()
                target = alloc.x - (adj.get_page_size() - alloc.width) / 2
                adj.set_value(max(0, min(target, adj.get_upper() - adj.get_page_size())))
            else:
                ctx.remove_class("pellicule-selected")

    def select_index(self, index):
        if not self.items:
            return
        n = len(self.items)
        self.index = index % n
        path, kind = self.items[self.index]
        self.stop_player()
        self.mark_selected()
        name = os.path.basename(path)
        try:
            size = os.path.getsize(path)
        except OSError:
            size = 0
        kind_fr = {"image": "Image", "video": "Vidéo", "audio": "Audio"}[kind]
        extra = ""
        if kind in ("video", "audio"):
            extra = "  —  GStreamer (codecs système)" if GST_OK else "  —  ouvrir avec VLC"
        self.info.set_text("%s  ·  %s  ·  %s%s" % (name, kind_fr, format_size(size), extra))
        self.set_title("%s — Pellicule" % name)

        if kind == "image":
            self.preview_stack.set_visible_child_name("image")
            self.show_image(path)
        elif kind in ("video", "audio"):
            if GST_OK and self.playbin is not None:
                if kind == "video" and self.video_widget is not None:
                    self.preview_stack.set_visible_child_name("video")
                else:
                    self.preview_stack.set_visible_child_name("image")
                    self.show_placeholder(kind)
                uri = Gio.File.new_for_path(path).get_uri()
                self.playbin.set_property("uri", uri)
                self.playbin.set_state(Gst.State.PLAYING)
            else:
                self.preview_stack.set_visible_child_name("image")
                self.show_placeholder(kind)

    def show_placeholder(self, kind):
        pb = load_thumb(self.items[self.index][0], kind, 256)
        if pb is not None:
            self.image.set_from_pixbuf(pb)
        else:
            icon = "video-x-generic" if kind == "video" else "audio-x-generic"
            self.image.set_from_icon_name(icon, Gtk.IconSize.DIALOG)

    def show_image(self, path):
        alloc = self.preview_stack.get_allocation()
        max_w = max(64, alloc.width - 16)
        max_h = max(64, alloc.height - 16)
        try:
            pb = GdkPixbuf.Pixbuf.new_from_file_at_scale(path, max_w, max_h, True)
            self.image.set_from_pixbuf(pb)
        except Exception:
            self.image.set_from_icon_name("image-x-generic", Gtk.IconSize.DIALOG)

    def on_preview_alloc(self, _w, _alloc):
        if self._alloc_id:
            GLib.source_remove(self._alloc_id)
        self._alloc_id = GLib.timeout_add(120, self._refresh_image)

    def _refresh_image(self):
        self._alloc_id = 0
        if 0 <= self.index < len(self.items) and self.items[self.index][1] == "image":
            self.show_image(self.items[self.index][0])
        return False

    def step(self, delta):
        if not self.items:
            return
        self.select_index(self.index + delta)

    def on_scroll(self, _w, event):
        direction = 0
        if event.direction == Gdk.ScrollDirection.UP or event.direction == Gdk.ScrollDirection.LEFT:
            direction = -1
        elif event.direction == Gdk.ScrollDirection.DOWN or event.direction == Gdk.ScrollDirection.RIGHT:
            direction = 1
        elif event.direction == Gdk.ScrollDirection.SMOOTH:
            _ok, dx, dy = event.get_scroll_deltas()
            if abs(dx) > abs(dy):
                direction = 1 if dx > 0.2 else (-1 if dx < -0.2 else 0)
            else:
                direction = 1 if dy > 0.2 else (-1 if dy < -0.2 else 0)
        if direction:
            self.step(direction)
            return True
        return False

    def on_preview_click(self, _w, event):
        if event.button != 1:
            return False
        alloc = _w.get_allocation()
        if event.x < alloc.width / 2:
            self.step(-1)
        else:
            self.step(1)
        return True

    def on_key(self, _w, event):
        key = event.keyval
        if key in (Gdk.KEY_Left, Gdk.KEY_Up, Gdk.KEY_Page_Up):
            self.step(-1)
            return True
        if key in (Gdk.KEY_Right, Gdk.KEY_Down, Gdk.KEY_Page_Down):
            self.step(1)
            return True
        if key in (Gdk.KEY_space, Gdk.KEY_p):
            self.on_play_pause()
            return True
        if key == Gdk.KEY_o and event.state & Gdk.ModifierType.CONTROL_MASK:
            self.on_open()
            return True
        if key == Gdk.KEY_Escape:
            self.stop_player()
            return True
        return False

    def on_play_pause(self, *_args):
        if not self.items or self.playbin is None:
            if self.items:
                self.on_open_vlc()
            return
        kind = self.items[self.index][1]
        if kind == "image":
            self.step(1)
            return
        _ok, state, _pending = self.playbin.get_state(0)
        if state == Gst.State.PLAYING:
            self.playbin.set_state(Gst.State.PAUSED)
        else:
            self.playbin.set_state(Gst.State.PLAYING)

    def on_gst_state(self, _bus, message):
        if message.src != self.playbin:
            return
        _old, new, _pending = message.parse_state_changed()
        if new == Gst.State.PLAYING:
            self.play_btn.set_image(
                Gtk.Image.new_from_icon_name("media-playback-pause-symbolic", Gtk.IconSize.BUTTON)
            )
        elif new in (Gst.State.PAUSED, Gst.State.NULL, Gst.State.READY):
            self.play_btn.set_image(
                Gtk.Image.new_from_icon_name("media-playback-start-symbolic", Gtk.IconSize.BUTTON)
            )

    def on_gst_error(self, _bus, message):
        err, debug = message.parse_error()
        self.stop_player()
        dlg = Gtk.MessageDialog(
            transient_for=self,
            flags=0,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.NONE,
            text="GStreamer n’a pas pu lire ce fichier",
        )
        dlg.format_secondary_text("%s\n\nOuvrir dans VLC (ses propres codecs) ?" % err)
        dlg.add_button("Annuler", Gtk.ResponseType.CANCEL)
        dlg.add_button("Ouvrir dans VLC", Gtk.ResponseType.OK)
        resp = dlg.run()
        dlg.destroy()
        if resp == Gtk.ResponseType.OK:
            self.on_open_vlc()

    def on_gst_eos(self, _bus, _message):
        self.stop_player()
        self.step(1)

    def on_open_vlc(self, *_args):
        if not self.items or self.index < 0:
            return
        path = self.items[self.index][0]
        vlc = GLib.find_program_in_path("vlc")
        if not vlc:
            dlg = Gtk.MessageDialog(
                transient_for=self, flags=0,
                message_type=Gtk.MessageType.INFO,
                buttons=Gtk.ButtonsType.OK,
                text="VLC n’est pas installé",
            )
            dlg.run()
            dlg.destroy()
            return
        self.stop_player()
        try:
            subprocess.Popen([vlc, path], start_new_session=True)
        except OSError:
            pass


class PelliculeApp(Gtk.Application):
    def __init__(self):
        super().__init__(
            application_id=APP_ID,
            flags=Gio.ApplicationFlags.HANDLES_OPEN | Gio.ApplicationFlags.NON_UNIQUE,
        )
        self.win = None

    def do_activate(self):
        if self.win is None:
            self.win = PelliculeWindow(self)
            self.win.show_all()
        self.win.present()

    def do_open(self, files, _n, _hint):
        self.activate()
        if not files:
            return
        path = files[0].get_path()
        if not path:
            return
        if os.path.isdir(path):
            self.win.load_folder(path)
        elif os.path.isfile(path):
            self.win.load_folder(os.path.dirname(path), select=path)


def main():
    app = PelliculeApp()
    return app.run(sys.argv)


if __name__ == "__main__":
    sys.exit(main())
