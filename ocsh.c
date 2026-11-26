/*
 * ocsh — Mini-shell en C
 * Projet 3 — Systèmes d'Exploitation — UIR/ESIN
 */

/* Sans cette macro, la compilation en -std=c99 strict n'expose pas
   struct sigaction, SA_RESTART, setenv, strdup, popen... (POSIX 2008). */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <glob.h>
#include <dirent.h>

#define MAX_LINE 1024
#define MAX_ARGS 128
#define MAX_HIST 100
#define MAX_VARS 100
#define MAX_PATH 1024
#define MAX_SCRIPT_LINES 500

/* ===================== Variables globales ===================== */

char *historique[MAX_HIST];
int nb_hist = 0;

/* Variables d'environnement internes */
typedef struct {
    char nom[256];
    char valeur[256];
} Variable;

Variable variables[MAX_VARS];
int nb_vars = 0;

int dernier_code = 0;    /* $? */
int dernier_pid_bg = 0;  /* $! */

/* Déclaration anticipée : utilisée par la substitution $(...) */
void executer_ligne(char *ligne);

/* ===================== Gestion des signaux ===================== */

/*
 * Pourquoi SIGCHLD est bloqué autour de fork()/waitpid() :
 * le handler moissonne TOUS les fils terminés avec waitpid(-1). Sans
 * masquage, il peut voler le statut d'un fils au premier plan juste
 * avant que le shell ne l'attende ; waitpid() renvoie alors -1 et le
 * code retour ($?) serait lu dans une variable non initialisée.
 * On bloque donc SIGCHLD pendant la section critique : le handler ne
 * s'exécute qu'après, quand il ne reste que les fils d'arrière-plan.
 */

void bloquer_sigchld(sigset_t *ancien) {
    sigset_t masque;
    sigemptyset(&masque);
    sigaddset(&masque, SIGCHLD);
    sigprocmask(SIG_BLOCK, &masque, ancien);
}

void restaurer_masque(sigset_t *ancien) {
    sigprocmask(SIG_SETMASK, ancien, NULL);
}

/* Attend un fils précis en tolérant les interruptions (SIGINT).
   Renvoie 0 si le statut a été récupéré, -1 si le fils est inconnu. */
int attendre_fils(pid_t pid, int *statut) {
    int st;

    for (;;) {
        if (waitpid(pid, &st, 0) == pid) {
            if (statut) *statut = st;
            return 0;
        }
        if (errno != EINTR)
            return -1;  /* ECHILD : déjà moissonné, statut inconnu */
    }
}

/* Eviter les processus zombies (fils d'arrière-plan) */
void handle_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* Ctrl+C : le shell survit, mais la ligne en cours de frappe est jetée.
   Pas de SA_RESTART : read()/fgets() renvoient EINTR et la boucle
   d'affichage repart sur un prompt propre. */
void handle_sigint(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
}

void init_signaux(void) {
    struct sigaction sa;

    /* SIGCHLD : nettoyer les zombies */
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* SIGINT : ne pas quitter le shell */
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

/* ===================== Historique ===================== */

void ajouter_historique(char *ligne) {
    if (strlen(ligne) == 0)
        return;
    if (nb_hist < MAX_HIST) {
        historique[nb_hist] = strdup(ligne);
        nb_hist++;
    } else {
        free(historique[0]);
        for (int i = 0; i < MAX_HIST - 1; i++)
            historique[i] = historique[i + 1];
        historique[MAX_HIST - 1] = strdup(ligne);
    }
}

void afficher_historique(void) {
    for (int i = 0; i < nb_hist; i++)
        printf("  %d  %s\n", i + 1, historique[i]);
}

/* ===================== Variables internes ===================== */

void set_variable(char *nom, char *valeur) {
    for (int i = 0; i < nb_vars; i++) {
        if (strcmp(variables[i].nom, nom) == 0) {
            strncpy(variables[i].valeur, valeur, 255);
            variables[i].valeur[255] = '\0';
            return;
        }
    }
    if (nb_vars < MAX_VARS) {
        strncpy(variables[nb_vars].nom, nom, 255);
        variables[nb_vars].nom[255] = '\0';
        strncpy(variables[nb_vars].valeur, valeur, 255);
        variables[nb_vars].valeur[255] = '\0';
        nb_vars++;
    }
}

char *get_variable(char *nom) {
    /* Variables spéciales */
    static char buf[64];
    if (strcmp(nom, "?") == 0) {
        sprintf(buf, "%d", dernier_code);
        return buf;
    }
    if (strcmp(nom, "!") == 0) {
        sprintf(buf, "%d", dernier_pid_bg);
        return buf;
    }

    /* Variables internes */
    for (int i = 0; i < nb_vars; i++) {
        if (strcmp(variables[i].nom, nom) == 0)
            return variables[i].valeur;
    }

    /* Variables d'environnement */
    char *env = getenv(nom);
    if (env)
        return env;

    return NULL;
}

/* ===================== Expansion des variables ===================== */

/*
 * Substitution de commande ($(...) et `...`).
 * La commande est exécutée par ocsh LUI-MÊME dans un processus fils
 * (fork + executer_ligne) : on n'appelle ni system() ni /bin/sh.
 * La sortie du fils est récupérée par un pipe (IPC), puis insérée
 * dans `dest` à la position *j, sans le(s) '\n' final(aux).
 */
static void substituer_commande(const char *cmd, char *dest, int *j, int dest_size) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }

    fflush(stdout);   /* évite de dupliquer le tampon dans le fils */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* Fils : sa sortie standard part dans le pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        signal(SIGINT, SIG_DFL);

        char copie[MAX_LINE];
        strncpy(copie, cmd, MAX_LINE - 1);
        copie[MAX_LINE - 1] = '\0';
        executer_ligne(copie);
        fflush(stdout);
        exit(dernier_code);
    }

    /* Père : lire tout ce que le fils écrit, jusqu'à la fin (EOF) */
    close(pipefd[1]);
    char buf_sub[MAX_LINE];
    int n = 0, r;
    while (n < MAX_LINE - 1 &&
           (r = (int)read(pipefd[0], buf_sub + n, MAX_LINE - 1 - n)) > 0)
        n += r;
    close(pipefd[0]);
    waitpid(pid, NULL, 0);   /* (peut renvoyer -1 si SIGCHLD l'a déjà fait) */

    while (n > 0 && buf_sub[n - 1] == '\n')
        n--;
    for (int m = 0; m < n && *j < dest_size - 1; m++)
        dest[(*j)++] = buf_sub[m];
}

void expand_variables(char *src, char *dest, int dest_size) {
    int i = 0, j = 0;

    while (src[i] != '\0' && j < dest_size - 1) {
        if (src[i] == '$') {
            i++;
            char nom_var[256];
            int k = 0;

            if (src[i] == '{') {
                /* ${VAR} */
                i++;
                while (src[i] != '}' && src[i] != '\0' && k < 255) {
                    nom_var[k++] = src[i++];
                }
                if (src[i] == '}') i++;
            } else if (src[i] == '(') {
                /* $(...) — substitution de commande */
                i++;
                char cmd[MAX_LINE];
                int depth = 1;
                k = 0;
                while (depth > 0 && src[i] != '\0' && k < MAX_LINE - 1) {
                    if (src[i] == '(') depth++;
                    if (src[i] == ')') depth--;
                    if (depth > 0) cmd[k++] = src[i];
                    i++;
                }
                cmd[k] = '\0';
                substituer_commande(cmd, dest, &j, dest_size);
                continue;
            } else if (src[i] == '?' || src[i] == '!') {
                nom_var[k++] = src[i++];
            } else {
                while ((isalnum((unsigned char)src[i]) || src[i] == '_') && k < 255) {
                    nom_var[k++] = src[i++];
                }
                if (k == 0) {
                    /* '$' seul (ex: "prix: $") : on le garde tel quel */
                    dest[j++] = '$';
                    continue;
                }
            }
            nom_var[k] = '\0';

            if (k > 0) {
                char *val = get_variable(nom_var);
                if (val) {
                    while (*val && j < dest_size - 1)
                        dest[j++] = *val++;
                }
            }
        } else if (src[i] == '`') {
            /* `commande` — substitution avec backticks */
            i++;
            char cmd[MAX_LINE];
            int k = 0;
            while (src[i] != '`' && src[i] != '\0' && k < MAX_LINE - 1) {
                cmd[k++] = src[i++];
            }
            if (src[i] == '`') i++;
            cmd[k] = '\0';
            substituer_commande(cmd, dest, &j, dest_size);
        } else {
            dest[j++] = src[i++];
        }
    }
    dest[j] = '\0';
}

/* ===================== Découpage en arguments ===================== */

int decouper(char *ligne, char **args) {
    int nb = 0;
    char *p = ligne;

    while (*p != '\0' && nb < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (*p == '"') {
            p++;
            args[nb++] = p;
            while (*p != '"' && *p != '\0') p++;
            if (*p == '"') *p++ = '\0';
        } else if (*p == '\'') {
            p++;
            args[nb++] = p;
            while (*p != '\'' && *p != '\0') p++;
            if (*p == '\'') *p++ = '\0';
        } else {
            args[nb++] = p;
            while (*p != ' ' && *p != '\t' && *p != '\0') p++;
            if (*p != '\0') *p++ = '\0';
        }
    }
    args[nb] = NULL;
    return nb;
}

/* ===================== Traduction des alias ===================== */

void traduire_alias(char **args) {
    if (args[0] == NULL) return;

    if (strcmp(args[0], "list") == 0)    args[0] = "ls";
    else if (strcmp(args[0], "show") == 0)    args[0] = "cat";
    else if (strcmp(args[0], "copy") == 0)    args[0] = "cp";
    else if (strcmp(args[0], "move") == 0)    args[0] = "mv";
    else if (strcmp(args[0], "del") == 0)     args[0] = "rm";
    else if (strcmp(args[0], "newdir") == 0)  args[0] = "mkdir";
    else if (strcmp(args[0], "newfile") == 0) args[0] = "touch";
}

/* ===================== Commandes intégrées (builtins) ===================== */

int est_builtin(char *cmd) {
    char *builtins[] = {"go", "quit", "path", "say", "info", "past",
                        "osinfo", "export", NULL};
    for (int i = 0; builtins[i] != NULL; i++) {
        if (strcmp(cmd, builtins[i]) == 0) return 1;
    }
    return 0;
}

/* Exécute un builtin et renvoie son code de retour (0 = succès).
   Ce code alimente $? comme pour une commande externe. */
int executer_builtin(char **args, int argc) {
    if (strcmp(args[0], "go") == 0) {
        char *dir = (argc > 1) ? args[1] : getenv("HOME");
        if (dir == NULL) {
            fprintf(stderr, "go: HOME non défini\n");
            return 1;
        }
        if (chdir(dir) != 0) {
            perror("go");
            return 1;
        }
        return 0;
    }

    if (strcmp(args[0], "quit") == 0) {
        printf("Au revoir !\n");
        exit(0);
    }

    if (strcmp(args[0], "path") == 0) {
        char cwd[MAX_PATH];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            return 0;
        }
        perror("path");
        return 1;
    }

    if (strcmp(args[0], "say") == 0) {
        for (int i = 1; i < argc; i++) {
            printf("%s", args[i]);
            if (i < argc - 1) printf(" ");
        }
        printf("\n");
        return 0;
    }

    if (strcmp(args[0], "info") == 0) {
        printf("ocsh — Mini-shell en C\n");
        printf("Commandes intégrées : go, quit, path, say, info, past, osinfo, export\n");
        printf("Alias : list, show, copy, move, del, newdir, newfile\n");
        printf("Redirections : > >> < 2> 2>&1\n");
        printf("Pipe : ->\n");
        printf("Arrière-plan : &\n");
        return 0;
    }

    if (strcmp(args[0], "past") == 0) {
        afficher_historique();
        return 0;
    }

    if (strcmp(args[0], "osinfo") == 0) {
        printf("Systèmes d'Exploitation -- UIR/ESIN\n");
        return 0;
    }

    if (strcmp(args[0], "export") == 0) {
        if (argc < 2) {
            printf("Usage: export VAR=valeur\n");
            return 1;
        }
        /* export VAR=valeur */
        char *egal = strchr(args[1], '=');
        if (egal) {
            *egal = '\0';
            char *nom = args[1];
            char *val = egal + 1;
            setenv(nom, val, 1);
            set_variable(nom, val);
        } else {
            /* export VAR (exporter une variable existante) */
            char *val = get_variable(args[1]);
            if (val)
                setenv(args[1], val, 1);
        }
        return 0;
    }

    return 127;   /* inconnu (ne devrait pas arriver : est_builtin() filtre) */
}

/* ===================== Gestion des redirections ===================== */

/*
 * Insère des espaces autour des opérateurs de redirection pour que
 * `say hello>f.txt` soit analysé comme `say hello > f.txt`.
 * Les opérateurs entre guillemets sont laissés intacts.
 * Opérateurs reconnus : > >> < 2> 2>&1
 */
void normaliser_redirections(const char *src, char *dest, int taille) {
    int j = 0;
    char guillemet = 0;

    for (int i = 0; src[i] != '\0' && j < taille - 5; i++) {
        char c = src[i];

        if (guillemet) {
            dest[j++] = c;
            if (c == guillemet) guillemet = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            guillemet = c;
            dest[j++] = c;
            continue;
        }

        /* 2> et 2>&1 : redirection de stderr */
        if (c == '2' && src[i + 1] == '>' &&
            (i == 0 || src[i - 1] == ' ' || src[i - 1] == '\t')) {
            if (j > 0 && dest[j - 1] != ' ') dest[j++] = ' ';
            dest[j++] = '2';
            dest[j++] = '>';
            i++;
            if (src[i + 1] == '&' && src[i + 2] == '1') {
                dest[j++] = '&';
                dest[j++] = '1';
                i += 2;
            }
            dest[j++] = ' ';
            continue;
        }

        if (c == '>' || c == '<') {
            if (j > 0 && dest[j - 1] != ' ') dest[j++] = ' ';
            dest[j++] = c;
            if (c == '>' && src[i + 1] == '>') {
                dest[j++] = '>';
                i++;
            }
            dest[j++] = ' ';
            continue;
        }

        dest[j++] = c;
    }
    dest[j] = '\0';
}

/*
 * Expansion des jokers (*, ?, [...]) comme le fait bash.
 * Un motif sans correspondance reste littéral (GLOB_NOCHECK).
 */
void expandre_jokers(char **args, int *argc) {
    static char stockage[MAX_ARGS][MAX_PATH];
    char *nouveaux[MAX_ARGS];
    int nb = 0;

    for (int i = 0; i < *argc && nb < MAX_ARGS - 1; i++) {
        if (strpbrk(args[i], "*?[") == NULL) {
            nouveaux[nb++] = args[i];
            continue;
        }

        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(args[i], GLOB_NOCHECK, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc && nb < MAX_ARGS - 1; k++) {
                strncpy(stockage[nb], g.gl_pathv[k], MAX_PATH - 1);
                stockage[nb][MAX_PATH - 1] = '\0';
                nouveaux[nb] = stockage[nb];
                nb++;
            }
            globfree(&g);
        } else {
            nouveaux[nb++] = args[i];
        }
    }

    nouveaux[nb] = NULL;
    for (int i = 0; i <= nb; i++)
        args[i] = nouveaux[i];
    *argc = nb;
}

/* Cherche et applique les redirections >, >> et < dans les arguments.
   Modifie args[] en supprimant les tokens de redirection. */
void appliquer_redirections(char **args, int *argc) {
    int i = 0;
    while (i < *argc) {
        if (strcmp(args[i], ">") == 0 && i + 1 < *argc) {
            int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror("open"); exit(1); }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            /* Supprimer > et le fichier des args */
            for (int j = i; j < *argc - 2; j++)
                args[j] = args[j + 2];
            *argc -= 2;
            args[*argc] = NULL;
        } else if (strcmp(args[i], ">>") == 0 && i + 1 < *argc) {
            int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) { perror("open"); exit(1); }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            for (int j = i; j < *argc - 2; j++)
                args[j] = args[j + 2];
            *argc -= 2;
            args[*argc] = NULL;
        } else if (strcmp(args[i], "<") == 0 && i + 1 < *argc) {
            int fd = open(args[i + 1], O_RDONLY);
            if (fd < 0) { perror("open"); exit(1); }
            dup2(fd, STDIN_FILENO);
            close(fd);
            for (int j = i; j < *argc - 2; j++)
                args[j] = args[j + 2];
            *argc -= 2;
            args[*argc] = NULL;
        } else if (strcmp(args[i], "2>&1") == 0) {
            /* 2>&1 : stderr pointe sur la sortie standard courante */
            if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0)
                perror("dup2");
            for (int j = i; j < *argc - 1; j++)
                args[j] = args[j + 1];
            *argc -= 1;
            args[*argc] = NULL;
        } else if (strcmp(args[i], "2>") == 0 && i + 1 < *argc) {
            int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror("open"); exit(1); }
            dup2(fd, STDERR_FILENO);
            close(fd);
            for (int j = i; j < *argc - 2; j++)
                args[j] = args[j + 2];
            *argc -= 2;
            args[*argc] = NULL;
        } else {
            i++;
        }
    }
}

/* ===================== Exécution d'une commande simple ===================== */

void executer_commande_simple(char **args, int argc, int background) {
    if (argc == 0) return;

    /* Vérifier si c'est un builtin */
    if (est_builtin(args[0])) {
        /* Les builtins avec redirection : on fork quand même */
        int a_redirection = 0;
        for (int i = 0; i < argc; i++) {
            if (strcmp(args[i], ">") == 0 || strcmp(args[i], ">>") == 0 ||
                strcmp(args[i], "<") == 0 || strcmp(args[i], "2>") == 0 ||
                strcmp(args[i], "2>&1") == 0) {
                a_redirection = 1;
                break;
            }
        }

        if (a_redirection) {
            sigset_t ancien;

            fflush(stdout);
            bloquer_sigchld(&ancien);
            pid_t pid = fork();
            if (pid == 0) {
                restaurer_masque(&ancien);
                signal(SIGINT, SIG_DFL);
                appliquer_redirections(args, &argc);
                int rc = executer_builtin(args, argc);
                fflush(stdout);
                exit(rc);
            } else if (pid > 0) {
                if (!background) {
                    int status;
                    if (attendre_fils(pid, &status) == 0)
                        dernier_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                    restaurer_masque(&ancien);
                } else {
                    restaurer_masque(&ancien);
                    dernier_pid_bg = pid;
                    printf("[bg] %d\n", pid);
                }
            } else {
                restaurer_masque(&ancien);
                perror("fork");
            }
        } else {
            /* Builtin sans redirection : exécuté dans le PÈRE
               (indispensable pour go/export qui modifient le shell) */
            dernier_code = executer_builtin(args, argc);
        }
        return;
    }

    /* Traduire les alias avant d'exécuter */
    traduire_alias(args);

    /* fork + exec pour les commandes externes */
    sigset_t ancien;

    fflush(stdout);
    bloquer_sigchld(&ancien);
    pid_t pid = fork();
    if (pid == 0) {
        /* Processus fils : rétablir SIGINT par défaut */
        restaurer_masque(&ancien);
        signal(SIGINT, SIG_DFL);
        appliquer_redirections(args, &argc);
        execvp(args[0], args);
        /* Si execvp échoue */
        fprintf(stderr, "ocsh: commande introuvable: %s\n", args[0]);
        exit(127);
    } else if (pid > 0) {
        if (!background) {
            int status;
            if (attendre_fils(pid, &status) == 0)
                dernier_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            restaurer_masque(&ancien);
        } else {
            restaurer_masque(&ancien);
            dernier_pid_bg = pid;
            printf("[bg] %d\n", pid);
        }
    } else {
        restaurer_masque(&ancien);
        perror("fork");
    }
}

/* ===================== Exécution avec pipes (->) ===================== */

/* Vrai si la chaîne ne contient que des espaces/tabulations */
static int est_vide(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return *s == '\0';
}

/* Découpe la ligne par -> et exécute un pipeline */
void executer_pipeline(char *ligne) {
    /* Séparer la ligne par -> */
    char *commandes[MAX_ARGS];
    int nb_cmd = 0;
    char *p = ligne;

    commandes[nb_cmd++] = p;
    while (*p != '\0') {
        if (*p == '-' && *(p + 1) == '>') {
            *p = '\0';
            p += 2;
            while (*p == ' ') p++;
            commandes[nb_cmd++] = p;
        } else {
            p++;
        }
    }

    if (nb_cmd == 1) {
        /* Pas de pipe, commande simple */
        char *args[MAX_ARGS];
        char normalisee[MAX_LINE];
        int background = 0;

        normaliser_redirections(commandes[0], normalisee, MAX_LINE);
        int argc = decouper(normalisee, args);
        if (argc == 0) return;

        expandre_jokers(args, &argc);
        if (argc == 0) return;

        /* Vérifier & en fin de ligne */
        if (argc > 0 && strcmp(args[argc - 1], "&") == 0) {
            background = 1;
            args[argc - 1] = NULL;
            argc--;
        }

        executer_commande_simple(args, argc, background);
        return;
    }

    /* Un étage vide ("a ->" ou "-> b" ou "a -> -> b") est une erreur */
    for (int c = 0; c < nb_cmd; c++) {
        if (est_vide(commandes[c])) {
            fprintf(stderr, "ocsh: erreur de syntaxe près de '->'\n");
            dernier_code = 2;
            return;
        }
    }

    /* Pipeline avec plusieurs commandes */
    int i;
    int prev_fd = -1;  /* fd de lecture du pipe précédent */
    pid_t pids[MAX_ARGS];
    sigset_t ancien;

    /* SIGCHLD bloqué pendant tout le câblage et l'attente : le handler
       ne peut pas voler le statut d'un maillon du pipeline. */
    fflush(stdout);
    bloquer_sigchld(&ancien);

    for (i = 0; i < nb_cmd; i++) {
        char *args[MAX_ARGS];
        char normalisee[MAX_LINE];

        pids[i] = 0;   /* initialisé même si l'étage est ignoré */

        normaliser_redirections(commandes[i], normalisee, MAX_LINE);
        int argc = decouper(normalisee, args);
        if (argc == 0) continue;

        expandre_jokers(args, &argc);
        traduire_alias(args);

        int pipefd[2];
        if (i < nb_cmd - 1) {
            /* Créer un pipe sauf pour la dernière commande */
            if (pipe(pipefd) < 0) {
                perror("pipe");
                if (prev_fd != -1)
                    close(prev_fd);
                restaurer_masque(&ancien);   /* ne pas laisser SIGCHLD bloqué */
                return;
            }
        }

        pid_t pid = fork();
        pids[i] = pid;
        if (pid == 0) {
            restaurer_masque(&ancien);
            signal(SIGINT, SIG_DFL);

            /* Connecter l'entrée au pipe précédent */
            if (prev_fd != -1) {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }

            /* Connecter la sortie au pipe suivant */
            if (i < nb_cmd - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }

            appliquer_redirections(args, &argc);

            /* Vérifier si c'est un builtin */
            if (est_builtin(args[0])) {
                int rc = executer_builtin(args, argc);
                fflush(stdout);
                exit(rc);
            }

            execvp(args[0], args);
            fprintf(stderr, "ocsh: commande introuvable: %s\n", args[0]);
            exit(127);
        } else if (pid < 0) {
            perror("fork");
            pids[i] = 0;
            continue;
        }

        /* Parent : fermer les descripteurs */
        if (prev_fd != -1)
            close(prev_fd);
        if (i < nb_cmd - 1) {
            close(pipefd[1]);
            prev_fd = pipefd[0];
        }
    }

    /* Attendre tous les processus du pipeline (dans l'ordre de création) */
    for (i = 0; i < nb_cmd; i++) {
        int status;

        if (pids[i] <= 0)
            continue;
        if (attendre_fils(pids[i], &status) == 0) {
            if (i == nb_cmd - 1)
                dernier_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
    restaurer_masque(&ancien);
}

/* ===================== Gestion VAR=valeur ===================== */

int est_affectation(char *ligne) {
    char *p = ligne;

    /* Ignorer l'indentation : une affectation peut être indentée
       dans un bloc while/for/if. */
    while (*p == ' ' || *p == '\t') p++;

    char *egal = strchr(p, '=');
    if (!egal || egal == p) return 0;
    /* Vérifier que la partie avant = est un nom valide */
    for (char *q = p; q < egal; q++) {
        if (!isalnum((unsigned char)*q) && *q != '_') return 0;
    }
    return 1;
}

void traiter_affectation(char *ligne) {
    char *p = ligne;

    while (*p == ' ' || *p == '\t') p++;

    char *egal = strchr(p, '=');
    *egal = '\0';
    set_variable(p, egal + 1);
}

/* ===================== Fichier .ocshrc ===================== */

void charger_ocshrc(void) {
    char chemin[MAX_PATH];
    char *home = getenv("HOME");
    if (!home) return;

    snprintf(chemin, sizeof(chemin), "%s/.ocshrc", home);
    FILE *f = fopen(chemin, "r");
    if (!f) return;

    char ligne[MAX_LINE];
    while (fgets(ligne, sizeof(ligne), f)) {
        /* Supprimer le retour à la ligne */
        ligne[strcspn(ligne, "\n")] = '\0';

        /* Ignorer les lignes vides et les commentaires */
        char *p = ligne;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        executer_ligne(p);
    }
    fclose(f);
}

/* ===================== Mode script ===================== */

/* Évalue une condition simple pour if/while (code retour de la commande) */
int evaluer_condition(char *condition) {
    /* Expansion des variables dans la condition */
    char expanded[MAX_LINE];
    expand_variables(condition, expanded, MAX_LINE);

    char copie[MAX_LINE];
    char normalisee[MAX_LINE];
    strncpy(copie, expanded, MAX_LINE - 1);
    copie[MAX_LINE - 1] = '\0';

    normaliser_redirections(copie, normalisee, MAX_LINE);

    char *args[MAX_ARGS];
    int argc = decouper(normalisee, args);
    if (argc == 0) return 1;

    expandre_jokers(args, &argc);

    traduire_alias(args);

    sigset_t ancien;

    fflush(stdout);
    bloquer_sigchld(&ancien);
    pid_t pid = fork();
    if (pid == 0) {
        restaurer_masque(&ancien);
        appliquer_redirections(args, &argc);
        if (est_builtin(args[0])) {
            int rc = executer_builtin(args, argc);
            fflush(stdout);
            exit(rc);
        }
        execvp(args[0], args);
        exit(127);
    } else if (pid > 0) {
        int status;
        int ok = attendre_fils(pid, &status);

        restaurer_masque(&ancien);
        if (ok == 0 && WIFEXITED(status))
            return WEXITSTATUS(status);
        return 1;
    } else {
        restaurer_masque(&ancien);
        return 1;
    }
}

void executer_script_lignes(char **lignes, int debut, int fin);

/* Trouver le "done" ou "fi" correspondant en gérant l'imbrication */
int trouver_fin_bloc(char **lignes, int debut, int fin, char *mot_debut, char *mot_fin) {
    int depth = 1;
    for (int i = debut; i <= fin; i++) {
        char copie[MAX_LINE];
        strncpy(copie, lignes[i], MAX_LINE - 1);
        copie[MAX_LINE - 1] = '\0';
        char *args[MAX_ARGS];
        int argc = decouper(copie, args);
        if (argc == 0) continue;

        if (strcmp(args[0], mot_debut) == 0) depth++;
        if (strcmp(args[0], mot_fin) == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

/* Trouver "else" au même niveau */
int trouver_else(char **lignes, int debut, int fin) {
    int depth = 0;
    for (int i = debut; i <= fin; i++) {
        char copie[MAX_LINE];
        strncpy(copie, lignes[i], MAX_LINE - 1);
        copie[MAX_LINE - 1] = '\0';
        char *args[MAX_ARGS];
        int argc = decouper(copie, args);
        if (argc == 0) continue;

        if (strcmp(args[0], "if") == 0) depth++;
        if (strcmp(args[0], "fi") == 0) depth--;
        if (strcmp(args[0], "else") == 0 && depth == 0)
            return i;
    }
    return -1;
}

void executer_script_lignes(char **lignes, int debut, int fin) {
    int i = debut;
    while (i <= fin) {
        char copie[MAX_LINE];
        strncpy(copie, lignes[i], MAX_LINE - 1);
        copie[MAX_LINE - 1] = '\0';

        /* Ignorer lignes vides et commentaires */
        char *p = copie;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') { i++; continue; }

        char *args[MAX_ARGS];
        int argc = decouper(copie, args);

        /* if ... then ... [else ...] fi */
        if (argc > 0 && strcmp(args[0], "if") == 0) {
            /* Récupérer la condition (tout après "if") */
            char *cond = lignes[i] + 2;
            while (*cond == ' ') cond++;

            /* Chercher "then" sur la même ligne ou la suivante */
            char *then_pos = strstr(cond, "; then");
            char condition[MAX_LINE];
            if (then_pos) {
                strncpy(condition, cond, then_pos - cond);
                condition[then_pos - cond] = '\0';
            } else {
                strncpy(condition, cond, MAX_LINE - 1);
                condition[MAX_LINE - 1] = '\0';
                /* Le "then" est sur la ligne suivante */
                i++;
            }

            int pos_fi = trouver_fin_bloc(lignes, i + 1, fin, "if", "fi");
            if (pos_fi < 0) {
                fprintf(stderr, "ocsh: 'fi' manquant\n");
                return;
            }

            int pos_else = trouver_else(lignes, i + 1, pos_fi - 1);
            int result = evaluer_condition(condition);

            if (result == 0) {
                /* Condition vraie */
                int bloc_fin = (pos_else >= 0) ? pos_else - 1 : pos_fi - 1;
                executer_script_lignes(lignes, i + 1, bloc_fin);
            } else if (pos_else >= 0) {
                /* Condition fausse, exécuter le else */
                executer_script_lignes(lignes, pos_else + 1, pos_fi - 1);
            }

            i = pos_fi + 1;
            continue;
        }

        /* while ... do ... done */
        if (argc > 0 && strcmp(args[0], "while") == 0) {
            char *cond = lignes[i] + 5;
            while (*cond == ' ') cond++;

            char *do_pos = strstr(cond, "; do");
            char condition[MAX_LINE];
            if (do_pos) {
                strncpy(condition, cond, do_pos - cond);
                condition[do_pos - cond] = '\0';
            } else {
                strncpy(condition, cond, MAX_LINE - 1);
                condition[MAX_LINE - 1] = '\0';
                i++;
            }

            int pos_done = trouver_fin_bloc(lignes, i + 1, fin, "while", "done");
            if (pos_done < 0) {
                fprintf(stderr, "ocsh: 'done' manquant\n");
                return;
            }

            while (evaluer_condition(condition) == 0) {
                executer_script_lignes(lignes, i + 1, pos_done - 1);
            }

            i = pos_done + 1;
            continue;
        }

        /* for VAR in ... ; do ... done */
        if (argc > 0 && strcmp(args[0], "for") == 0) {
            /* Relire la ligne originale pour le parsing */
            char orig[MAX_LINE];
            strncpy(orig, lignes[i], MAX_LINE - 1);
            orig[MAX_LINE - 1] = '\0';

            char *mots[MAX_ARGS];
            int nb_mots = decouper(orig, mots);

            if (nb_mots < 4 || strcmp(mots[2], "in") != 0) {
                fprintf(stderr, "ocsh: syntaxe for incorrecte\n");
                i++;
                continue;
            }

            char *var_nom = mots[1];

            /* Trouver les valeurs (entre "in" et ";" ou fin de ligne) */
            char *valeurs[MAX_ARGS];
            int nb_val = 0;
            for (int j = 3; j < nb_mots; j++) {
                if (strcmp(mots[j], ";") == 0 || strcmp(mots[j], "do") == 0)
                    break;
                /* Supprimer un éventuel ; collé à la fin */
                int len = strlen(mots[j]);
                if (len > 0 && mots[j][len - 1] == ';')
                    mots[j][len - 1] = '\0';
                valeurs[nb_val++] = mots[j];
            }

            /* "do" peut être sur la même ligne ou la suivante */
            if (!strstr(lignes[i], " do") && !strstr(lignes[i], ";do")
                && !strstr(lignes[i], "\tdo"))
                i++;

            int pos_done = trouver_fin_bloc(lignes, i + 1, fin, "for", "done");
            if (pos_done < 0) {
                fprintf(stderr, "ocsh: 'done' manquant\n");
                return;
            }

            for (int v = 0; v < nb_val; v++) {
                set_variable(var_nom, valeurs[v]);
                executer_script_lignes(lignes, i + 1, pos_done - 1);
            }

            i = pos_done + 1;
            continue;
        }

        /* Mots-clés à ignorer s'ils apparaissent seuls */
        if (argc > 0 && (strcmp(args[0], "then") == 0 ||
                         strcmp(args[0], "do") == 0 ||
                         strcmp(args[0], "done") == 0 ||
                         strcmp(args[0], "fi") == 0 ||
                         strcmp(args[0], "else") == 0)) {
            i++;
            continue;
        }

        /* Commande normale (l'indentation est gérée par est_affectation
           et par le découpage en arguments) */
        executer_ligne(lignes[i]);
        i++;
    }
}

void executer_script(char *fichier) {
    FILE *f = fopen(fichier, "r");
    if (!f) {
        perror("ocsh: script");
        return;
    }

    char *lignes[MAX_SCRIPT_LINES];
    int nb_lignes = 0;
    char buf[MAX_LINE];

    while (fgets(buf, sizeof(buf), f) && nb_lignes < MAX_SCRIPT_LINES) {
        buf[strcspn(buf, "\n")] = '\0';
        lignes[nb_lignes] = strdup(buf);
        nb_lignes++;
    }
    fclose(f);

    executer_script_lignes(lignes, 0, nb_lignes - 1);

    for (int i = 0; i < nb_lignes; i++)
        free(lignes[i]);
}

/* ===================== Exécution d'une ligne complète ===================== */

void executer_ligne(char *ligne) {
    char expanded[MAX_LINE];
    expand_variables(ligne, expanded, MAX_LINE);

    /* Vérifier si c'est une affectation VAR=valeur */
    if (est_affectation(expanded)) {
        traiter_affectation(expanded);
        return;
    }

    executer_pipeline(expanded);
}

/* ===================== Complétion simple (TAB) ===================== */

/* Complétion basique : cherche les fichiers du répertoire courant
   qui commencent par le préfixe donné */
void completer(char *ligne, int *pos) {
    /* Trouver le dernier mot */
    int debut_mot = *pos - 1;
    while (debut_mot >= 0 && ligne[debut_mot] != ' ') debut_mot--;
    debut_mot++;

    char prefixe[256];
    int len_pref = *pos - debut_mot;
    strncpy(prefixe, ligne + debut_mot, len_pref);
    prefixe[len_pref] = '\0';

    if (len_pref == 0) return;

    /* Chercher dans les builtins et alias d'abord (si c'est le premier mot) */
    if (debut_mot == 0) {
        char *completions[] = {"go", "quit", "path", "say", "info", "past",
                               "osinfo", "export", "list", "show", "copy",
                               "move", "del", "newdir", "newfile", NULL};
        for (int i = 0; completions[i]; i++) {
            if (strncmp(completions[i], prefixe, len_pref) == 0) {
                /* Compléter */
                char *reste = completions[i] + len_pref;
                int len_reste = strlen(reste);
                for (int j = 0; j < len_reste; j++)
                    ligne[(*pos)++] = reste[j];
                ligne[*pos] = '\0';
                printf("%s", reste);
                fflush(stdout);
                return;
            }
        }
    }

    /* Chercher dans les fichiers du répertoire courant (opendir/readdir,
       sans fork : plus simple et plus cohérent dans un shell) */
    DIR *rep = opendir(".");
    if (!rep) return;

    struct dirent *entree;
    while ((entree = readdir(rep)) != NULL) {
        /* Ne pas proposer les fichiers cachés sauf si demandés */
        if (entree->d_name[0] == '.' && prefixe[0] != '.')
            continue;
        if (strncmp(entree->d_name, prefixe, len_pref) != 0)
            continue;

        char *reste = entree->d_name + len_pref;
        int len_reste = strlen(reste);
        if (*pos + len_reste >= MAX_LINE - 1)
            continue;
        for (int j = 0; j < len_reste; j++)
            ligne[(*pos)++] = reste[j];
        ligne[*pos] = '\0';
        printf("%s", reste);
        fflush(stdout);
        closedir(rep);
        return;
    }
    closedir(rep);
}

/* ===================== Lecture interactive avec support TAB ===================== */

/* Lecture caractère par caractère pour gérer TAB et les flèches historique */
int lire_ligne(char *buf, int taille) {
    struct termios orig, raw;
    int pos = 0;
    int hist_index = nb_hist;

    /* Passer en mode raw pour capturer TAB et flèches */
    if (tcgetattr(STDIN_FILENO, &orig) < 0)
        return (fgets(buf, taille, stdin) != NULL);

    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    buf[0] = '\0';

    while (1) {
        char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) {
            /* Ctrl+C : on jette la ligne en cours de frappe et on
               rend la main au shell qui réaffiche un prompt propre. */
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            buf[0] = '\0';
            return 1;
        }
        if (n <= 0) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            return 0;
        }

        if (c == '\n' || c == '\r') {
            buf[pos] = '\0';
            printf("\n");
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            return 1;
        }

        if (c == '\t') {
            completer(buf, &pos);
            continue;
        }

        if (c == 127 || c == '\b') {
            /* Backspace */
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (c == 4) {
            /* Ctrl+D */
            if (pos == 0) {
                printf("\n");
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
                return 0;
            }
            continue;
        }

        if (c == 27) {
            /* Séquence d'échappement (flèches) */
            char seq[2];
            read(STDIN_FILENO, &seq[0], 1);
            read(STDIN_FILENO, &seq[1], 1);

            if (seq[0] == '[') {
                if (seq[1] == 'A') {
                    /* Flèche haut : historique précédent */
                    if (hist_index > 0) {
                        hist_index--;
                        /* Effacer la ligne actuelle */
                        while (pos > 0) {
                            printf("\b \b");
                            pos--;
                        }
                        strcpy(buf, historique[hist_index]);
                        pos = strlen(buf);
                        printf("%s", buf);
                        fflush(stdout);
                    }
                } else if (seq[1] == 'B') {
                    /* Flèche bas : historique suivant */
                    while (pos > 0) {
                        printf("\b \b");
                        pos--;
                    }
                    if (hist_index < nb_hist - 1) {
                        hist_index++;
                        strcpy(buf, historique[hist_index]);
                        pos = strlen(buf);
                        printf("%s", buf);
                    } else {
                        hist_index = nb_hist;
                        buf[0] = '\0';
                        pos = 0;
                    }
                    fflush(stdout);
                }
            }
            continue;
        }

        /* Caractère normal */
        if (pos < taille - 1) {
            buf[pos++] = c;
            buf[pos] = '\0';
            printf("%c", c);
            fflush(stdout);
        }
    }
}

/* ===================== Programme principal ===================== */

int main(int argc_main, char *argv[]) {
    /* Mode script : ocsh script.ocsh */
    if (argc_main >= 2) {
        executer_script(argv[1]);
        return dernier_code;
    }

    /* Mode interactif */
    init_signaux();
    charger_ocshrc();

    char ligne[MAX_LINE];

    while (1) {
        printf("ocsh> ");
        fflush(stdout);

        if (isatty(STDIN_FILENO)) {
            if (!lire_ligne(ligne, MAX_LINE))
                break;
        } else {
            if (fgets(ligne, MAX_LINE, stdin) == NULL) {
                if (errno == EINTR)
                    continue;  /* Ctrl+C : repartir sur un prompt propre */
                break;
            }
            ligne[strcspn(ligne, "\n")] = '\0';
        }

        /* Ignorer les lignes vides et les commentaires */
        char *p = ligne;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        ajouter_historique(p);
        executer_ligne(p);
    }

    printf("\nAu revoir !\n");

    /* Libérer l'historique */
    for (int i = 0; i < nb_hist; i++)
        free(historique[i]);

    return 0;
}
