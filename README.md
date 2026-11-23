# ocsh — Mini-shell en C

Mini-shell développé en C dans le cadre du **Projet 3** du module Systèmes d'Exploitation (UIR/ESIN).

## Description

ocsh est un interpréteur de commandes simplifié qui reproduit les mécanismes de base de Bash.
Il utilise directement les appels système Linux (`fork`, `execvp`, `waitpid`, `dup2`, `pipe`, `open`)
pour créer des processus, gérer les redirections et les pipes, sans jamais passer par `system()`
ni par `/bin/sh` pour exécuter une commande.

## Compilation

Prérequis : `gcc` et `make` sur Linux.

```bash
make          # produit le binaire ./ocsh
make clean    # supprime le binaire
```

Le code se compile en C99 strict (`-std=c99 -Wall -Wextra`) ; la macro `_POSIX_C_SOURCE`
est définie en tête de `code/ocsh.c` pour exposer les appels POSIX (`sigaction`, `setenv`,
`strdup`, `glob`, `opendir`…).

## Utilisation

### Mode interactif

```bash
./ocsh
```

Le prompt `ocsh>` apparaît. TAB complète les commandes et les noms de fichiers, les flèches
haut/bas parcourent l'historique, Ctrl+D quitte.

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
| `osinfo` | —               | Afficher « Systèmes d'Exploitation -- UIR/ESIN » |
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

- **Exécution externe** : `fork()` + `execvp()` + `waitpid()` ; code retour 127 si la commande est introuvable.
- **Redirections** : `>`, `>>`, `<`, `2>`, `2>&1` — avec ou sans espace (`say hello>f.txt` fonctionne).
- **Pipe** : `->` (au lieu de `|`), nombre d'étages illimité (`a -> b -> c`).
- **Arrière-plan** : `&` en fin de commande, PID rangé dans `$!`.
- **Variables** : `VAR=valeur`, `$VAR`, `${VAR}`, `$?`, `$!`, `export`.
- **Substitution de commandes** : `$(commande)` et `` `commande` ``.
- **Jokers** : `*`, `?`, `[...]` (un motif sans correspondance reste littéral).
- **Fichier de configuration** : `~/.ocshrc`, exécuté au démarrage.
- **Historique** : 100 entrées, navigation par flèches, builtin `past`.
- **Complétion** : TAB sur les commandes intégrées, les alias et les fichiers du répertoire courant.
- **Mode script** : `if/then/else/fi`, `for/in/do/done`, `while/do/done`, avec blocs imbriqués.
- **Signaux** : `SIGINT` n'arrête pas le shell et annule la ligne en cours de frappe ; `SIGCHLD`
  moissonne les fils d'arrière-plan (pas de zombies).

## Limites connues

Ces points ne sont **pas** implémentés ; ils sont volontairement hors du périmètre demandé par le sujet :

- pas de séquencement `;`, ni de `&&` / `||` ;
- `if/then/fi` n'existe qu'en mode script (la forme sur une seule ligne en interactif n'est pas reconnue) ;
- pas de contrôle de tâches (`jobs`, `fg`, `bg`) ;
- pas de heredoc (`<<`), ni de substitution arithmétique `$((...))` ;
- l'expansion des variables a lieu y compris entre apostrophes (`say '$HOME'` développe `$HOME`) ;
- les jokers sont développés même s'ils sont entre guillemets.

## Exemples (démonstration de soutenance)

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
ocsh-mini-shell/
├── code/
│   └── ocsh.c        # code source commenté (C99)
├── Makefile          # compilation
├── README.md         # ce fichier
└── report.md         # rapport (2–4 pages, source du report.pdf)
```

## Auteurs

Projet réalisé dans le cadre du cours de Systèmes d'Exploitation — Pr. Ilias Tougui — UIR/ESIN.
