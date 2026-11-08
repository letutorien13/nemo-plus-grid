#!/usr/bin/env bash
# Paquet .deb de Nemo+grid+ (pellicule, boutons agrandis).
# S'installe dans /opt/nemo-plus-grid et ne remplace pas le Nemo du système.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VER="${NEMO_PLUS_VERSION:-6.6.3+grid2}"
ARCH="$(dpkg --print-architecture)"
BUILD="$ROOT/build-deb"
STAGE="$ROOT/packaging/stage"
PKGDIR="$STAGE/nemo-plus-grid_${VER}_${ARCH}"
OUTDIR="${NEMO_PLUS_DEB_OUT:-$ROOT/dist}"
PREFIX=/opt/nemo-plus-grid

echo "=== Nemo+grid+ — paquet Debian isolé ==="
echo "Version : $VER  Arch : $ARCH"
echo

rm -rf "$STAGE"
mkdir -p "$PKGDIR" "$OUTDIR"

if [ ! -f "$BUILD/build.ninja" ]; then
  rm -rf "$BUILD"
  meson setup "$BUILD" \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Dgtk_doc=false \
    -Dselinux=false \
    -Dtracker=false \
    -Dsystem_integration=false
else
  meson configure "$BUILD" --prefix="$PREFIX" -Dsystem_integration=false --buildtype=release
fi

ninja -C "$BUILD"
DESTDIR="$PKGDIR" ninja -C "$BUILD" install

rm -f \
  "$PKGDIR$PREFIX/share/applications/nemo.desktop" \
  "$PKGDIR$PREFIX/share/applications/nemo-autostart.desktop" \
  "$PKGDIR$PREFIX/share/applications/nemo-autorun-software.desktop" \
  "$PKGDIR$PREFIX/share/dbus-1/services/nemo.service" \
  "$PKGDIR$PREFIX/share/dbus-1/services/nemo.FileManager1.service" \
  "$PKGDIR$PREFIX/share/polkit-1/actions/org.nemo.root.policy" \
  "$PKGDIR$PREFIX/bin/nemo-desktop" \
  "$PKGDIR$PREFIX/bin/nemo-autorun-software" \
  "$PKGDIR$PREFIX/bin/nemo-plus" \
  "$PKGDIR$PREFIX/share/applications/nemo-plus.desktop" \
  "$PKGDIR$PREFIX/share/applications/pellicule.desktop"

if [ ! -x "$PKGDIR$PREFIX/bin/nemo-plus-test" ]; then
  echo "Binaire introuvable : $PKGDIR$PREFIX/bin/nemo-plus-test" >&2
  exit 1
fi

# Greffons du Nemo Mint (archives, images, partage, Python).
# Fichiers réels, pas des liens : ils doivent partir avec le paquet.
EXT_SRC="/usr/lib/x86_64-linux-gnu/nemo/extensions-3.0"
EXT_DST="$PKGDIR$PREFIX/lib/x86_64-linux-gnu/nemo/extensions-3.0"
mkdir -p "$EXT_DST"
for name in libnemo-fileroller.so libnemo-image-converter.so libnemo-python.so libnemo-share.so; do
  if [ ! -f "$EXT_SRC/$name" ]; then
    echo "Greffon manquant : $EXT_SRC/$name" >&2
    exit 1
  fi
  install -m 0644 "$EXT_SRC/$name" "$EXT_DST/$name"
done

mkdir -p \
  "$PKGDIR/usr/bin" \
  "$PKGDIR/usr/share/applications" \
  "$PKGDIR/usr/share/doc/nemo-plus-grid" \
  "$PKGDIR$PREFIX/share/nemo-plus-grid" \
  "$PKGDIR$PREFIX/share/dconf/profile" \
  "$PKGDIR/DEBIAN"

install -m 0755 "$ROOT/scripts/setup-runtime.sh" "$PKGDIR$PREFIX/share/nemo-plus-grid/setup-runtime.sh"
printf '%s\n' 'user-db:nemoplusgrid' > "$PKGDIR$PREFIX/share/dconf/profile/nemo-plus-grid"

cat > "$PKGDIR$PREFIX/share/nemo-plus-grid/launcher.sh" << 'EOF'
#!/usr/bin/env bash
# Lance Nemo+grid+ sans remplacer le Nemo du système.
set -u

PREFIX="${NEMO_PLUS_PREFIX:-/opt/nemo-plus-grid}"
BIN="$PREFIX/bin/nemo-plus-test"

if [ ! -x "$BIN" ]; then
  echo "Nemo+grid+ n'est pas installé ($BIN)." >&2
  exit 1
fi

# Une instance sans fenêtre bloque les suivantes. On ne tue que CE paquet.
if command -v pgrep >/dev/null 2>&1; then
  for pid in $(pgrep -u "${USER:-$(id -un)}" -x nemo-plus-test || true); do
    exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
    if [ "$exe" = "$BIN" ]; then
      has_win=0
      if command -v wmctrl >/dev/null 2>&1 && wmctrl -lx 2>/dev/null | grep -q 'nemo-plus-test\.Nemo-plus-test'; then
        has_win=1
      fi
      if [ "$has_win" = 0 ]; then
        kill "$pid" >/dev/null 2>&1 || true
        sleep 0.2
      fi
    fi
  done
fi

NEMO_PLUS_HOME="${NEMO_PLUS_HOME:-$HOME/.nemo-plus-grid}"
mkdir -p \
  "$NEMO_PLUS_HOME/cache" \
  "$NEMO_PLUS_HOME/data" \
  "$NEMO_PLUS_HOME/state" \
  "$HOME/.config/dconf"

export XDG_CACHE_HOME="$NEMO_PLUS_HOME/cache"
export XDG_DATA_HOME="$NEMO_PLUS_HOME/data"
export XDG_STATE_HOME="$NEMO_PLUS_HOME/state"
export DCONF_PROFILE=nemo-plus-grid
export XDG_DATA_DIRS="$PREFIX/share:/usr/local/share:/usr/share"
export GSETTINGS_SCHEMA_DIR="$PREFIX/share/glib-2.0/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}"
export LD_LIBRARY_PATH="$PREFIX/lib/x86_64-linux-gnu:$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$PREFIX/bin:/usr/bin:/bin:${PATH:-}"
export NEMO_PLUS_CLAIM_FILEMANAGER=0

if [ -z "${LANGUAGE:-}" ] && [ -n "${LANG:-}" ]; then
  case "$LANG" in
    fr*|FR*) export LANGUAGE="${LANG%%.*}:fr" ;;
  esac
fi

if [ -d "$PREFIX/share/glib-2.0/schemas" ]; then
  glib-compile-schemas "$PREFIX/share/glib-2.0/schemas" 2>/dev/null || true
fi

if [ ! -f "$NEMO_PLUS_HOME/.window-seeded" ]; then
  gsettings set org.nemo.window-state maximized true || true
  gsettings set org.nemo.window-state sidebar-width 400 || true
  touch "$NEMO_PLUS_HOME/.window-seeded"
fi

exec "$BIN" "$@"
EOF
chmod 0755 "$PKGDIR$PREFIX/share/nemo-plus-grid/launcher.sh"

cat > "$PKGDIR/usr/bin/nemo-plus-grid" << 'EOF'
#!/bin/sh
export NEMO_PLUS_PREFIX=/opt/nemo-plus-grid
exec /opt/nemo-plus-grid/share/nemo-plus-grid/launcher.sh "$@"
EOF
chmod 0755 "$PKGDIR/usr/bin/nemo-plus-grid"

cat > "$PKGDIR/usr/share/applications/nemo-plus-grid.desktop" << 'EOF'
[Desktop Entry]
Name=Nemo+grid+
Comment=mode pellicule, boutons agrandis
Exec=/usr/bin/nemo-plus-grid %U
TryExec=/usr/bin/nemo-plus-grid
Icon=system-file-manager
Terminal=false
Type=Application
StartupNotify=true
StartupWMClass=nemo-plus-test
DBusActivatable=false
Categories=GTK;Utility;Filesystem;
MimeType=inode/directory;
EOF

if [ -f "$ROOT/pellicule/pellicule.py" ]; then
  install -m 0755 "$ROOT/pellicule/pellicule.py" "$PKGDIR$PREFIX/bin/pellicule"
  cat > "$PKGDIR/usr/bin/pellicule" << 'EOF'
#!/bin/sh
exec /opt/nemo-plus-grid/bin/pellicule "$@"
EOF
  chmod 0755 "$PKGDIR/usr/bin/pellicule"
  if [ -f "$ROOT/pellicule/pellicule.desktop" ]; then
    sed -e 's|^Exec=pellicule|Exec=/usr/bin/pellicule|' \
        -e 's|^TryExec=pellicule|TryExec=/usr/bin/pellicule|' \
        -e 's|inode/directory;||g' \
      "$ROOT/pellicule/pellicule.desktop" \
      > "$PKGDIR/usr/share/applications/pellicule.desktop"
  fi
fi

cat > "$PKGDIR/usr/share/doc/nemo-plus-grid/copyright" << 'EOF'
Nemo+grid+ is based on Nemo from Linux Mint.
Nemo is GPL-2.0-or-later. The source is in this package's Git repository.
The bundled Nemo extensions (fileroller, image-converter, python, share)
are the GPL binaries shipped by Linux Mint for Nemo 6.6.
EOF
cp "$ROOT/COPYING" "$PKGDIR/usr/share/doc/nemo-plus-grid/COPYING"

DEPS="libc6, libgtk-3-0t64 | libgtk-3-0, libglib2.0-0t64 | libglib2.0-0, libcinnamon-desktop4t64 | libcinnamon-desktop4, libxapp1, libexif12, libexempi8, libgail-3-0t64 | libgail-3-0, libjson-glib-1.0-0, libpango-1.0-0, gvfs, gsettings-desktop-schemas, cinnamon-desktop-data, shared-mime-info"
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
  SHLIB=$(dpkg-shlibdeps \
    -l"$PKGDIR$PREFIX/lib/x86_64-linux-gnu" \
    -O "$PKGDIR$PREFIX/bin/nemo-plus-test" 2>/dev/null | sed 's/^shlibs:Depends=//' || true)
  if [ -n "${SHLIB:-}" ]; then
    DEPS="$SHLIB, gvfs, gsettings-desktop-schemas, cinnamon-desktop-data, shared-mime-info, file-roller, nemo-image-converter, nemo-python, nemo-share"
  fi
fi
case "$DEPS" in
  *file-roller*) ;;
  *) DEPS="$DEPS, file-roller, nemo-image-converter, nemo-python, nemo-share" ;;
esac

SIZE_KB=$(du -sk "$PKGDIR" | awk '{print $1}')

cat > "$PKGDIR/DEBIAN/control" << EOF
Package: nemo-plus-grid
Version: $VER
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: letutorien13 <letutorien13@users.noreply.github.com>
Installed-Size: $SIZE_KB
Depends: $DEPS
Recommends: cinnamon-l10n
Homepage: https://github.com/letutorien13/nemo-plus-grid
Description: Nemo+grid+ — mode pellicule, boutons agrandis
 Copie isolée du gestionnaire de fichiers Nemo, avec le mode pellicule
 (Ctrl+4), des boutons de barre d'outils agrandis, et les greffons
 archives, images, partage et Python.
 Installée dans /opt/nemo-plus-grid. Elle ne remplace pas /usr/bin/nemo,
 ne lance pas le bureau, et ne devient pas le gestionnaire par défaut.
EOF

cat > "$PKGDIR/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e
if [ -d /opt/nemo-plus-grid/share/glib-2.0/schemas ]; then
  glib-compile-schemas /opt/nemo-plus-grid/share/glib-2.0/schemas || true
fi
if [ -x /opt/nemo-plus-grid/share/nemo-plus-grid/setup-runtime.sh ]; then
  NEMO_PLUS_PREFIX=/opt/nemo-plus-grid /opt/nemo-plus-grid/share/nemo-plus-grid/setup-runtime.sh >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null; then
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 0755 "$PKGDIR/DEBIAN/postinst"

cat > "$PKGDIR/DEBIAN/prerm" << 'EOF'
#!/bin/sh
set -e
if command -v pgrep >/dev/null; then
  for pid in $(pgrep -x nemo-plus-test 2>/dev/null || true); do
    exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
    if [ "$exe" = "/opt/nemo-plus-grid/bin/nemo-plus-test" ]; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
fi
exit 0
EOF
chmod 0755 "$PKGDIR/DEBIAN/prerm"

DEB="$OUTDIR/nemo-plus-grid_${VER}_${ARCH}.deb"
fakeroot dpkg-deb --build "$PKGDIR" "$DEB"

echo
echo "=== Paquet créé ==="
ls -lh "$DEB"
dpkg-deb -I "$DEB" | sed -n '1,45p'
echo
echo "Sur un autre ordinateur Linux Mint :"
echo "  sudo apt install ./$(basename "$DEB")"
echo "Puis : nemo-plus-grid"
echo
echo "Le Nemo du système n'est pas remplacé."
