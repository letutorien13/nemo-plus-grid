#!/usr/bin/env bash
# Lanceur isolé de nemo+ : ne remplace pas Nemo, n'en vole pas le rôle.
#
# Isolation :
#   - binaire / libs dans PREFIX (/opt/nemo-plus ou ~/.local/nemo-plus)
#   - config, dconf, cache dans ~/.nemo-plus/
#   - pas de bus org.freedesktop.FileManager1
#   - pas d'autostart / nemo-desktop
#   - greffons Nemo système non chargés (sauf NEMO_PLUS_SYSTEM_EXTENSIONS=1)
set -u

if [ -n "${NEMO_PLUS_PREFIX:-}" ]; then
  PREFIX="$NEMO_PLUS_PREFIX"
elif [ -x /opt/nemo-plus/bin/nemo-plus ]; then
  PREFIX=/opt/nemo-plus
else
  PREFIX="${HOME}/.local/nemo-plus"
fi

BIN="$PREFIX/bin/nemo-plus"
if [ ! -x "$BIN" ]; then
  echo "nemo+ n'est pas installé ($BIN introuvable)." >&2
  echo "Installe le paquet nemo-plus, ou compile : ./build-nemo-plus.sh" >&2
  exit 1
fi

# Instance fantôme (processus sans fenêtre) : relancer vraiment
if command -v pgrep >/dev/null 2>&1 && pgrep -u "${USER:-$(id -un)}" -x nemo-plus >/dev/null 2>&1; then
  has_win=0
  if command -v wmctrl >/dev/null 2>&1; then
    if wmctrl -lx 2>/dev/null | grep -qi 'nemo-plus'; then
      has_win=1
    fi
  fi
  if [ "$has_win" = 0 ]; then
    pkill -u "${USER:-$(id -un)}" -x nemo-plus >/dev/null 2>&1 || true
    sleep 0.2
    pkill -9 -u "${USER:-$(id -un)}" -x nemo-plus >/dev/null 2>&1 || true
  fi
fi

USER_HOME="${HOME}"
NEMO_PLUS_HOME="${NEMO_PLUS_HOME:-$USER_HOME/.nemo-plus}"

mkdir -p \
  "$NEMO_PLUS_HOME/config" \
  "$NEMO_PLUS_HOME/cache" \
  "$NEMO_PLUS_HOME/data" \
  "$NEMO_PLUS_HOME/state"

seed_file() {
  local src="$1" dest="$2"
  if [ -f "$src" ] && [ ! -e "$dest" ]; then
    mkdir -p "$(dirname "$dest")"
    cp -n "$src" "$dest" 2>/dev/null || true
  fi
}

seed_link() {
  local src="$1" dest="$2"
  if [ -e "$src" ] && [ ! -e "$dest" ]; then
    mkdir -p "$(dirname "$dest")"
    ln -sfn "$src" "$dest"
  fi
}

# Copie initiale inutilisée tant que XDG_CONFIG_HOME reste celui de la session.
seed_file "$USER_HOME/.config/user-dirs.dirs" "$NEMO_PLUS_HOME/config/user-dirs.dirs"
seed_file "$USER_HOME/.config/gtk-3.0/settings.ini" "$NEMO_PLUS_HOME/config/gtk-3.0/settings.ini"
seed_file "$USER_HOME/.config/gtk-3.0/bookmarks" "$NEMO_PLUS_HOME/config/gtk-3.0/bookmarks"
seed_link "$USER_HOME/.cache/thumbnails" "$NEMO_PLUS_HOME/cache/thumbnails"
seed_link "$USER_HOME/.local/share/icons" "$NEMO_PLUS_HOME/data/icons"
seed_link "$USER_HOME/.local/share/themes" "$NEMO_PLUS_HOME/data/themes"

# Locales / greffons (idempotent)
SETUP="$PREFIX/share/nemo-plus/setup-runtime.sh"
if [ ! -x "$SETUP" ]; then
  SETUP="$(cd "$(dirname "$0")" && pwd)/setup-runtime.sh"
fi
if [ -x "$SETUP" ]; then
  NEMO_PLUS_PREFIX="$PREFIX" bash "$SETUP" >/dev/null 2>&1 || true
fi

# Cache et données privés. XDG_CONFIG_HOME reste celui de la session :
# le service dconf ignore un XDG_CONFIG_HOME différent et la lecture locale
# retombe alors sur les valeurs par défaut (barre latérale à 170 px).
# Le profil nommé « nemoplus » isole quand même les clés de celles de Nemo.
export XDG_CACHE_HOME="$NEMO_PLUS_HOME/cache"
export XDG_DATA_HOME="$NEMO_PLUS_HOME/data"
export XDG_STATE_HOME="$NEMO_PLUS_HOME/state"

# dconf isolé. Le client cherche un profil NOMMÉ dans $XDG_DATA_DIRS/dconf/profile/,
# pas dans XDG_DATA_HOME. Un chemin absolu dans DCONF_PROFILE fait planter à la fermeture.
mkdir -p "$PREFIX/share/dconf/profile" "$USER_HOME/.config/dconf"
# Pas de tiret dans le nom : dconf en fait un chemin D-Bus, et « - » est invalide.
printf '%s\n' 'user-db:nemoplus' > "$PREFIX/share/dconf/profile/nemo-plus"
export DCONF_PROFILE=nemo-plus

# Données de l'appli + icônes/mime système. Pas $HOME/.local/share (greffons Python de Nemo).
export XDG_DATA_DIRS="$PREFIX/share:/usr/local/share:/usr/share"
export GSETTINGS_SCHEMA_DIR="$PREFIX/share/glib-2.0/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}"
export LD_LIBRARY_PATH="$PREFIX/lib/x86_64-linux-gnu:$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$PREFIX/bin:/usr/bin:/bin:${PATH:-}"

export NEMO_PLUS_CLAIM_FILEMANAGER="${NEMO_PLUS_CLAIM_FILEMANAGER:-0}"

if [ -z "${LANGUAGE:-}" ] && [ -n "${LANG:-}" ]; then
  case "$LANG" in
    fr*|FR*) export LANGUAGE="${LANG%%.*}:fr" ;;
  esac
fi

if [ -d "$PREFIX/share/glib-2.0/schemas" ]; then
  glib-compile-schemas "$PREFIX/share/glib-2.0/schemas" 2>/dev/null || true
fi

exec "$BIN" "$@"
