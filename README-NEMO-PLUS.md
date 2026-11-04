# nemo+ (Nemo 6.6.3 du système)

Copie du **Nemo 6.6.3+zena** installé sur cette machine (Linux Mint), avec le **mode Pellicule** repris du fork qui fonctionne.

Le Nemo du bureau n’est pas remplacé. Les sources sont dans `/home/actionpc/Nemo+grid`. L’ancien arbre `/home/actionpc/nemo-plus` (base 6.7.4) n’est pas modifié.

## Isolation

| Élément | Nemo système | nemo+ |
|--------|----------------|--------|
| Programme | `/usr/bin/nemo` | `~/.local/nemo-plus` ou `/opt/nemo-plus` |
| Application | `org.Nemo` | `org.NemoPlus` |
| Fenêtres | `nemo` | `nemo-plus` |
| Config | `~/.config/nemo` | `~/.nemo-plus/config/nemo` |
| Réglages dconf | session du bureau | base privée `nemo-plus` |
| « Ouvrir le dossier » | `FileManager1` | non pris |
| Bureau Cinnamon | `nemo-desktop` | non installé |

## Mode Pellicule

Grande prévisualisation en haut, bandeau de miniatures en bas.

| Accès | Raccourci |
|-------|-----------|
| Menu Affichage → Pellicule | Ctrl+4 |
| Barre d’outils | bouton Pellicule |

Un clic sur une miniature sélectionne le fichier sans recentrer la bande. Les flèches et `<` `>` peuvent faire défiler pour garder l’élément visible.

## Compilation locale

```bash
cd "/home/actionpc/Nemo+grid"
./build-nemo-plus.sh
nemo-plus
```

Préfixe : `~/.local/nemo-plus`. Ce script remplace le nemo+ déjà installé à cet endroit.

## Paquet .deb

```bash
cd "/home/actionpc/Nemo+grid"
./packaging/build-deb.sh
```

Le paquet est `dist/nemo-plus_6.6.3+plus1_*.deb`. Il s’installe sous `/opt/nemo-plus` et ne remplace pas Nemo.

## Ce qui n’a pas été repris

Les contournements de fenêtre de l’ancien fork 6.7.4 (affichage forcé après 600 ms, géométrie, largeur de panneau) ne sont pas dans cette copie. La base est le Nemo du système, dont l’ouverture de fenêtre fonctionne déjà.
