# Rapport — Projet 3 : ocsh, Mini-shell en C

**Module** : Systèmes d'Exploitation  
**Professeur** : Pr. Ilias Tougui  
**Établissement** : UIR/ESIN  

---

## 1. Introduction

Le projet ocsh consiste à développer un mini-shell en C qui reproduit les mécanismes fondamentaux de Bash. L'objectif est de comprendre concrètement comment fonctionne un interpréteur de commandes en utilisant directement les appels système du noyau Linux.

## 2. Approche de développement

Nous avons suivi une approche progressive, en développant les fonctionnalités dans l'ordre suivant :

1. **Boucle interactive** : lecture d'une ligne, découpage en arguments, affichage du prompt `ocsh>`.
2. **Commandes intégrées et alias** : implémentation de `go`, `quit`, `path`, `say`, `info`, `past`, `osinfo`, et des alias (`list`, `show`, `copy`, `move`, `del`, `newdir`, `newfile`).
3. **Exécution de commandes externes** : utilisation de `fork()` pour créer un processus fils, `execvp()` pour remplacer l'image du processus, et `waitpid()` pour attendre sa terminaison.
4. **Redirections** : implémentation de `>`, `>>` et `<` avec `open()`, `dup2()` et `close()`.
5. **Pipes** : l'opérateur `->` connecte la sortie d'une commande à l'entrée de la suivante via `pipe()` et `dup2()`.
6. **Arrière-plan et signaux** : exécution en arrière-plan avec `&`, gestion de `SIGINT` pour ne pas quitter le shell, et `SIGCHLD` pour éviter les processus zombies.
7. **Variables et expansions** : support de `$VAR`, `${VAR}`, `$?`, `$!`, `export`, et substitution de commandes avec `$(...)`.
8. **Mode script** : exécution de fichiers `.ocsh` avec support de `if/then/else/fi`, `for/in/do/done` et `while/do/done`.

## 3. Liens avec les TP

Le projet mobilise directement les notions vues en TP :

- **TP03** (programmation C sous Linux) : toute la structure du programme, la compilation avec GCC, le Makefile.
- **TP04** (processus et signaux) : `fork()`, `execvp()`, `wait()`, `waitpid()`, `signal()` et `sigaction()`. C'est le cœur du projet.
- **TP02** (redirections et pipes) : nous avons implémenté en C ce que nous utilisions en ligne de commande avec `>`, `>>`, `<` et `|`.
- **TP01 et TP07** (commandes Linux, fichiers) : les alias (`list`, `show`, etc.) correspondent aux commandes de base.
- **TP05 et TP06** (scripts Bash) : le mode script de ocsh reproduit les structures `if`, `for` et `while` vues dans les scripts Bash.

## 4. Concepts clés

- **Programme vs processus** : un programme est un fichier exécutable sur le disque, un processus est une instance en cours d'exécution en mémoire.
- **fork/exec/wait** : `fork()` crée une copie du processus, `exec()` remplace l'image du processus par un nouveau programme, `wait()` attend la fin du fils.
- **Pourquoi `go` (cd) est un builtin** : `cd` doit modifier le répertoire courant du processus shell lui-même. Si on le faisait dans un fils (avec fork), seul le fils changerait de répertoire, pas le parent.
- **Redirections** : les descripteurs de fichier 0 (stdin), 1 (stdout) et 2 (stderr) sont dupliqués avec `dup2()` vers des fichiers ouverts avec `open()`.
- **Pipes** : `pipe()` crée deux descripteurs connectés. La sortie d'une commande est redirigée vers l'entrée de la suivante.
- **Signaux** : `SIGINT` (Ctrl+C) est intercepté pour ne pas tuer le shell. `SIGCHLD` est géré pour nettoyer les processus fils terminés en arrière-plan.

## 5. Difficultés rencontrées

- **Expansion des variables dans les substitutions** : les variables `$VAR` à l'intérieur de `$(...)` devaient être résolues avant d'exécuter la commande. Nous avons résolu cela en effectuant une expansion récursive.
- **Gestion du terminal pour la complétion** : pour capturer la touche TAB et les flèches, il faut passer le terminal en mode "raw" avec `termios`, ce qui n'était pas couvert en TP. Nous avons utilisé `tcgetattr` et `tcsetattr` de manière simple.
- **Parsing du pipe `->` ** : comme `->` contient deux caractères, il fallait le distinguer d'un simple `-` suivi d'un `>` (redirection). Nous parcourons la ligne caractère par caractère.
- **Course entre `SIGCHLD` et `waitpid()`** : le handler `SIGCHLD` moissonnait tous les fils avec `waitpid(-1, WNOHANG)`, y compris le fils au premier plan que le shell s'apprêtait à attendre. Selon l'ordre d'exécution, ce dernier récupérait `-1` et lisait `$?` dans un statut non initialisé. Nous avons corrigé le problème en bloquant `SIGCHLD` avec `sigprocmask()` autour de `fork()`/`waitpid()` : le handler ne s'exécute plus dans la section critique et ne traite que les fils d'arrière-plan.
- **Redirection collée au mot** : `say hello>f.txt` n'était pas reconnu, car le découpage se faisait uniquement sur les espaces. Une passe de normalisation insère des espaces autour des opérateurs (`>`, `>>`, `<`, `2>`, `2>&1`) avant le découpage, en respectant les guillemets.

## 6. Résultats

| Fonctionnalité | État | Remarque |
|---|---|---|
| Boucle interactive | ✓ | |
| Builtins (go, quit, path, say, info, past, osinfo) | ✓ | |
| Alias (list, show, copy, move, del, newdir, newfile) | ✓ | |
| Redirections (>, >>, <) | ✓ | avec ou sans espace ; `2>` et `2>&1` également |
| Pipe (->) | ✓ | pipelines à plusieurs étages |
| Arrière-plan (&) | ✓ | PID dans `$!`, pas de zombie |
| Signaux (SIGINT, SIGCHLD) | ✓ | Ctrl+C annule la ligne, ne quitte pas le shell |
| Variables ($VAR, ${VAR}, $?, $!) | ✓ | |
| Export | ✓ | |
| Substitution de commandes ($(...), backticks) | ✓ | |
| Fichier .ocshrc | ✓ | |
| Historique et complétion | ✓ | flèches + TAB (commandes et fichiers) |
| Mode script (if, for, while) | ✓ | blocs imbriqués ; forme sur une seule ligne non supportée en interactif |
| Jokers (*, ?, [...]) | ✓ | motif sans correspondance laissé littéral |

Limites assumées (hors périmètre du sujet) : pas de `;`, `&&`, `||`, ni de contrôle de tâches
(`jobs`, `fg`, `bg`), pas de heredoc, et les variables sont développées même entre apostrophes.

Les commandes de la soutenance fonctionnent correctement :

```
ocsh> list
ocsh> go .. puis path
ocsh> say hello > f.txt puis show f.txt
ocsh> show f.txt -> wc -l
ocsh> sleep 10 &
ocsh> ps
ocsh> osinfo
```

## 7. Conclusion

Ce projet nous a permis de comprendre en profondeur le fonctionnement d'un shell. En implémentant nous-mêmes `fork`, `exec`, `wait`, `dup2` et `pipe`, nous avons vu concrètement comment le noyau Linux gère les processus et les communications inter-processus. La difficulté principale était de coordonner correctement les descripteurs de fichiers dans les pipelines et de gérer les cas particuliers (builtins avec redirections, variables dans les substitutions).
