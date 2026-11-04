#!/usr/bin/env bash
# Prépare le runtime isolé de nemo+ (traductions, greffons optionnels).
set -euo pipefail

PREFIX="${NEMO_PLUS_PREFIX:-$HOME/.local/nemo-plus}"

# --- Traductions : lier les .mo système (fr, etc.) ---
if [ -d /usr/share/locale ]; then
  mkdir -p "$PREFIX/share/locale"
  find /usr/share/locale -path '*/LC_MESSAGES/nemo.mo' 2>/dev/null | while read -r mo; do
    rel="${mo#/usr/share/locale/}"
    dest="$PREFIX/share/locale/$rel"
    mkdir -p "$(dirname "$dest")"
    ln -sfn "$mo" "$dest"
  done
  find /usr/share/locale -path '*/LC_MESSAGES/nemo-extensions.mo' 2>/dev/null | while read -r mo; do
    rel="${mo#/usr/share/locale/}"
    dest="$PREFIX/share/locale/$rel"
    mkdir -p "$(dirname "$dest")"
    ln -sfn "$mo" "$dest"
  done
fi

EXT_LOCAL="$PREFIX/lib/x86_64-linux-gnu/nemo/extensions-3.0"
EXT_SYS="/usr/lib/x86_64-linux-gnu/nemo/extensions-3.0"
mkdir -p "$EXT_LOCAL"

# Par défaut : PAS de greffons système (copie isolée).
# Pour les réactiver : NEMO_PLUS_SYSTEM_EXTENSIONS=1
if [ "${NEMO_PLUS_SYSTEM_EXTENSIONS:-0}" = "1" ] && [ -d "$EXT_SYS" ]; then
  for so in "$EXT_SYS"/*.so; do
    [ -f "$so" ] || continue
    base="$(basename "$so")"
    if [ ! -e "$EXT_LOCAL/$base" ] || [ -L "$EXT_LOCAL/$base" ]; then
      ln -sfn "$so" "$EXT_LOCAL/$base"
    fi
  done
else
  # Retirer d'anciens liens vers les .so système pour ne plus les charger
  if [ -d "$EXT_LOCAL" ]; then
    find "$EXT_LOCAL" -maxdepth 1 -type l -name '*.so' -delete 2>/dev/null || true
  fi
fi

echo "Runtime nemo+ préparé :"
echo "  Locales : $PREFIX/share/locale"
echo "  Greffons: $EXT_LOCAL (système=$( [ "${NEMO_PLUS_SYSTEM_EXTENSIONS:-0}" = 1 ] && echo oui || echo non ))"
ls -la "$EXT_LOCAL" 2>/dev/null || true
