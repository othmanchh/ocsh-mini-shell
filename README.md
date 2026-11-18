# ocsh — Mini-shell en C

Mini-shell développé en C dans le cadre du Projet 3 du module Systèmes d'Exploitation (UIR/ESIN).

## Description

ocsh est un interpréteur de commandes simplifié qui reproduit les fonctionnalités de base de Bash. Il utilise directement les appels système Linux (`fork`, `execvp`, `wait`, `dup2`, `pipe`) pour créer des processus, gérer les redirections et les pipes.

## Compilation

```bash
make
```

Pour nettoyer :

```bash
make clean
```

## Utilisation

### Mode interactif

```bash
./ocsh
```

Le prompt `ocsh>` apparaît. Tapez vos commandes, utilisez TAB pour la complétion et les flèches haut/bas pour naviguer dans l'historique.

### Mode script

```bash
./ocsh script.ocsh
```

## Commandes intégrées

| Commande | Équivalent Bash | Description |
|----------|-----------------|-------------|
| `go`     | `cd`            | Changer de répertoire |
| `quit`   | `exit`          | Quitter le shell |
| `path`   | `pwd`           | Afficher le répertoire courant |
| `say`    | `echo`          | Afficher du texte |
| `info`   | `help`          | Afficher l'aide |
| `past`   | `history`       | Afficher l'historique |
| `osinfo` | —               | Afficher "Systèmes d'Exploitation -- UIR/ESIN" |
| `export` | `export`        | Exporter une variable d'environnement |

## Alias

| Alias     | Commande Linux |
|-----------|----------------|
| `list`    | `ls`           |
| `show`    | `cat`          |
| `copy`    | `cp`           |
| `move`    | `mv`           |
| `del`     | `rm`           |
| `newdir`  | `mkdir`        |
| `newfile` | `touch`        |

## Fonctionnalités

- **Redirections** : `>` (écriture), `>>` (ajout), `<` (lecture)
- **Pipe** : `->` (au lieu de `|`)
- **Arrière-plan** : `&` en fin de commande
- **Variables** : `VAR=valeur`, `$VAR`, `${VAR}`, `$?`, `$!`
- **Substitution de commandes** : `$(commande)` et `` `commande` ``
- **Fichier de configuration** : `~/.ocshrc` (exécuté au démarrage)
- **Historique** : navigation avec flèches haut/bas
- **Complétion** : TAB pour compléter les commandes et noms de fichiers
- **Mode script** : support de `if/then/else/fi`, `for/in/do/done`, `while/do/done`
- **Signaux** : SIGINT (Ctrl+C) ne quitte pas le shell, SIGCHLD nettoie les zombies

## Exemples (démonstration soutenance)

```bash
ocsh> list
ocsh> go ..
ocsh> path
ocsh> say hello > f.txt
ocsh> show f.txt
ocsh> show f.txt -> wc -l
ocsh> sleep 10 &
ocsh> ps
ocsh> osinfo
```

## Structure du projet

```
ocsh/
├── code/
│   └── ocsh.c        # Code source principal
├── Makefile           # Compilation
├── README.md          # Ce fichier
├── report.pdf         # Rapport (2-4 pages)
└── demo.mp4           # Vidéo de démonstration
```

## Auteurs

Projet réalisé dans le cadre du cours de Systèmes d'Exploitation — Pr. Ilias Tougui — UIR/ESIN.
