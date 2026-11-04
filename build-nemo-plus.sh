#!/usr/bin/env bash
# Compile et installe nemo+ comme copie isolée dans ~/.local/nemo-plus
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${NEMO_PLUS_PREFIX:-$ROOT/prefix}"
BUILD="$ROOT/build"

echo "=== nemo+ — installation isolée (ne remplace pas Nemo) ==="
echo "Préfixe : $PREFIX"
echo

PKGS=(
  build-essential meson ninja-build pkg-config
  libgtk-3-dev libglib2.0-dev libgirepository1.0-dev
  libxml2-dev libxapp-dev libcinnamon-desktop-dev
  libexif-dev libexempi-dev libnotify-dev libgail-3-dev
  libjson-glib-dev libpango1.0-dev libx11-dev libxext-dev
  libxrender-dev libwayland-dev libgtk-layer-shell-dev
  libgsf-1-dev intltool itstool
)

if ! pkg-config --exists gtk+-3.0; then
  if command -v sudo >/dev/null; then
    sudo apt-get update
    sudo apt-get install -y "${PKGS[@]}"
  else
    echo "Installez manuellement : ${PKGS[*]}"
    exit 1
  fi
else
  echo "Dépendances GTK déjà présentes."
fi

echo
echo "=== Configuration meson (prefix=$PREFIX, sans intégration système) ==="
# Recréer le build si l'option d'isolation n'était pas là
NEED_RECONFIGURE=0
if [ ! -f "$BUILD/build.ninja" ]; then
  NEED_RECONFIGURE=1
elif ! grep -q "system_integration" "$BUILD/meson-info/intro-buildoptions.json" 2>/dev/null; then
  NEED_RECONFIGURE=1
fi

if [ "$NEED_RECONFIGURE" = 1 ]; then
  rm -rf "$BUILD"
  meson setup "$BUILD" \
    --prefix="$PREFIX" \
    -Dgtk_doc=false \
    -Dselinux=false \
    -Dtracker=false \
    -Dsystem_integration=false \
    2>&1 || {
      echo
      echo "Meson a échoué. Installez les paquets manquants listés ci-dessus."
      exit 1
    }
else
  meson configure "$BUILD" -Dsystem_integration=false --prefix="$PREFIX"
fi

echo
echo "=== Compilation ==="
ninja -C "$BUILD"

echo
echo "=== Installation ==="
ninja -C "$BUILD" install

# Retirer d'anciens fichiers d'intégration qui pourraient encore traîner
rm -f \
  "$PREFIX/share/applications/nemo.desktop" \
  "$PREFIX/share/applications/nemo-autostart.desktop" \
  "$PREFIX/share/applications/nemo-autorun-software.desktop" \
  "$PREFIX/share/dbus-1/services/nemo.service" \
  "$PREFIX/share/dbus-1/services/nemo.FileManager1.service" \
  "$PREFIX/share/polkit-1/actions/org.nemo.root.policy" \
  "$PREFIX/bin/nemo-desktop" \
  "$PREFIX/bin/nemo-autorun-software"

# Cette copie de test ne s'installe pas dans ~/.local.
# Le nemo+ déjà installé (lanceur, menu, binaire) reste intact.
chmod +x "$ROOT/nemo-plus" 2>/dev/null || true

if [ -d "$PREFIX/share/glib-2.0/schemas" ]; then
  glib-compile-schemas "$PREFIX/share/glib-2.0/schemas" || true
fi

# Traductions françaises (et les autres langues) depuis le Nemo du système.
NEMO_PLUS_PREFIX="$PREFIX" bash "$ROOT/scripts/setup-runtime.sh" || true

echo
echo "=== copie de test compilée (boutons de barre doublés) ==="
echo "  Binaire   : $PREFIX/bin/nemo-plus-test"
echo "  Lanceur   : $ROOT/nemo-plus"
echo "  Données   : $HOME/.nemo-plus-test/  (séparées de nemo+ et du Nemo système)"
echo
echo "Lancer depuis ce dossier :"
echo "  ./nemo-plus"
echo
echo "Le Nemo de Linux Mint et le nemo+ installé ne sont pas remplacés."
