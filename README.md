# ocsh — Mini-shell en C

Projet semestriel *Systèmes d'Exploitation* (UIR/ESIN) — Projet 3.


## Description
`ocsh` est un interpréteur de commandes écrit en C qui utilise directement
les appels système (`fork`, `execvp`, `wait`, `pipe`, `dup2`, `signal`).
Les commandes sont renommées et le pipe s'écrit `->` au lieu de `|`.

## Compilation et exécution
```bash
cd code
make
./ocsh
```
Prérequis : `gcc`, `make`, Linux.

## Commandes
| Bash | ocsh | | Bash | ocsh |
|------|------|-|------|------|
| cd | go | | ls | list |
| exit | quit | | cat | show |
| pwd | path | | cp | copy |
| echo | say | | mv | move |
| help | info | | rm | del |
| history | past | | mkdir | newdir |
| `\|` | `->` | | touch | newfile |

Builtin supplémentaire : `osinfo`.

## Avancement
- [x] M1 — REPL, découpage par `->`, fork/execvp/wait
- [x] M2 — Builtins complets, redirections `>` `>>` `<`
- [ ] M3 — Pipes `->`, `&`, SIGINT/SIGCHLD
- [ ] M4 — Variables, `export`, `$?`, `$!`
- [ ] M5 — `$(...)`, `.ocshrc`, historique, complétion
- [ ] M6 — Mode script (`if`/`for`/`while`), job control

## Exemples
```
ocsh> list
ocsh> say hello > f.txt
ocsh> show f.txt -> wc -l
ocsh> sleep 10 &
```

## Structure
- `code/src/` : sources (main, parser, executor, builtins, aliases)
- `code/include/ocsh.h` : structures partagées
- `code/tests/` : scripts de test
- `report.pdf`, `demo.mp4` : rapport et vidéo de démonstration
