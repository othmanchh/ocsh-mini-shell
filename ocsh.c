/*
 * ocsh — Mini-shell en C
 * Projet 3 — Systèmes d'Exploitation — UIR/ESIN
 */

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

/* ===================== Gestion des signaux ===================== */

/* Eviter les processus zombies */
void handle_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* Ignorer SIGINT dans le shell parent */
void handle_sigint(int sig) {
    (void)sig;
    write(1, "\nocsh> ", 7);
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
    sa.sa_flags = SA_RESTART;
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

                /* Expansion des variables dans la commande substituée */
                char cmd_expanded[MAX_LINE];
                expand_variables(cmd, cmd_expanded, MAX_LINE);

                /* Exécuter la commande et capturer la sortie */
                int pipefd[2];
                pipe(pipefd);
                pid_t pid = fork();
                if (pid == 0) {
                    close(pipefd[0]);
                    dup2(pipefd[1], STDOUT_FILENO);
                    close(pipefd[1]);
                    execlp("/bin/sh", "sh", "-c", cmd_expanded, NULL);
                    exit(1);
                } else {
                    close(pipefd[1]);
                    char buf_sub[MAX_LINE];
                    int n = read(pipefd[0], buf_sub, MAX_LINE - 1);
                    close(pipefd[0]);
                    waitpid(pid, NULL, 0);
                    if (n > 0) {
                        /* Supprimer le \n final */
                        if (buf_sub[n - 1] == '\n') n--;
                        for (int m = 0; m < n && j < dest_size - 1; m++)
                            dest[j++] = buf_sub[m];
                    }
                }
                continue;
            } else if (src[i] == '?' || src[i] == '!') {
                nom_var[k++] = src[i++];
            } else {
                while ((isalnum(src[i]) || src[i] == '_') && k < 255) {
                    nom_var[k++] = src[i++];
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

            /* Expansion des variables dans la commande */
            char cmd_bt_exp[MAX_LINE];
            expand_variables(cmd, cmd_bt_exp, MAX_LINE);

            int pipefd[2];
            pipe(pipefd);
            pid_t pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
                execlp("/bin/sh", "sh", "-c", cmd_bt_exp, NULL);
                exit(1);
            } else {
                close(pipefd[1]);
                char buf_sub[MAX_LINE];
                int n = read(pipefd[0], buf_sub, MAX_LINE - 1);
                close(pipefd[0]);
                waitpid(pid, NULL, 0);
                if (n > 0) {
                    if (buf_sub[n - 1] == '\n') n--;
                    for (int m = 0; m < n && j < dest_size - 1; m++)
                        dest[j++] = buf_sub[m];
                }
            }
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

int executer_builtin(char **args, int argc) {
    if (strcmp(args[0], "go") == 0) {
        char *dir = (argc > 1) ? args[1] : getenv("HOME");
        if (chdir(dir) != 0)
            perror("go");
        return 1;
    }

    if (strcmp(args[0], "quit") == 0) {
        printf("Au revoir !\n");
        exit(0);
    }

    if (strcmp(args[0], "path") == 0) {
        char cwd[MAX_PATH];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
            printf("%s\n", cwd);
        else
            perror("path");
        return 1;
    }

    if (strcmp(args[0], "say") == 0) {
        for (int i = 1; i < argc; i++) {
            printf("%s", args[i]);
            if (i < argc - 1) printf(" ");
        }
        printf("\n");
        return 1;
    }

    if (strcmp(args[0], "info") == 0) {
        printf("ocsh — Mini-shell en C\n");
        printf("Commandes intégrées : go, quit, path, say, info, past, osinfo\n");
        printf("Alias : list, show, copy, move, del, newdir, newfile\n");
        printf("Redirections : > >> <\n");
        printf("Pipe : ->\n");
        printf("Arrière-plan : &\n");
        return 1;
    }

    if (strcmp(args[0], "past") == 0) {
        afficher_historique();
        return 1;
    }

    if (strcmp(args[0], "osinfo") == 0) {
        printf("Systèmes d'Exploitation -- UIR/ESIN\n");
        return 1;
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
        return 1;
    }

    return 0;
}

/* ===================== Gestion des redirections ===================== */

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
                strcmp(args[i], "<") == 0) {
                a_redirection = 1;
                break;
            }
        }

        if (a_redirection) {
            pid_t pid = fork();
            if (pid == 0) {
                signal(SIGINT, SIG_DFL);
                appliquer_redirections(args, &argc);
                executer_builtin(args, argc);
                exit(0);
            } else if (pid > 0) {
                if (!background) {
                    int status;
                    waitpid(pid, &status, 0);
                    if (WIFEXITED(status))
                        dernier_code = WEXITSTATUS(status);
                } else {
                    dernier_pid_bg = pid;
                    printf("[bg] %d\n", pid);
                }
            }
        } else {
            executer_builtin(args, argc);
            dernier_code = 0;
        }
        return;
    }

    /* Traduire les alias avant d'exécuter */
    traduire_alias(args);

    /* fork + exec pour les commandes externes */
    pid_t pid = fork();
    if (pid == 0) {
        /* Processus fils : rétablir SIGINT par défaut */
        signal(SIGINT, SIG_DFL);
        appliquer_redirections(args, &argc);
        execvp(args[0], args);
        /* Si execvp échoue */
        fprintf(stderr, "ocsh: commande introuvable: %s\n", args[0]);
        exit(127);
    } else if (pid > 0) {
        if (!background) {
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status))
                dernier_code = WEXITSTATUS(status);
            else
                dernier_code = 1;
        } else {
            dernier_pid_bg = pid;
            printf("[bg] %d\n", pid);
        }
    } else {
        perror("fork");
    }
}

/* ===================== Exécution avec pipes (->) ===================== */

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
        int background = 0;

        int argc = decouper(commandes[0], args);
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

    /* Pipeline avec plusieurs commandes */
    int i;
    int prev_fd = -1;  /* fd de lecture du pipe précédent */

    for (i = 0; i < nb_cmd; i++) {
        char *args[MAX_ARGS];
        int argc = decouper(commandes[i], args);
        if (argc == 0) continue;

        traduire_alias(args);

        int pipefd[2];
        if (i < nb_cmd - 1) {
            /* Créer un pipe sauf pour la dernière commande */
            if (pipe(pipefd) < 0) {
                perror("pipe");
                return;
            }
        }

        pid_t pid = fork();
        if (pid == 0) {
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
                executer_builtin(args, argc);
                exit(0);
            }

            execvp(args[0], args);
            fprintf(stderr, "ocsh: commande introuvable: %s\n", args[0]);
            exit(127);
        } else if (pid < 0) {
            perror("fork");
            return;
        }

        /* Parent : fermer les descripteurs */
        if (prev_fd != -1)
            close(prev_fd);
        if (i < nb_cmd - 1) {
            close(pipefd[1]);
            prev_fd = pipefd[0];
        }
    }

    /* Attendre tous les processus du pipeline */
    for (i = 0; i < nb_cmd; i++) {
        int status;
        wait(&status);
        if (i == nb_cmd - 1 && WIFEXITED(status))
            dernier_code = WEXITSTATUS(status);
    }
}

/* ===================== Gestion VAR=valeur ===================== */

int est_affectation(char *ligne) {
    char *egal = strchr(ligne, '=');
    if (!egal || egal == ligne) return 0;
    /* Vérifier que la partie avant = est un nom valide */
    for (char *p = ligne; p < egal; p++) {
        if (!isalnum(*p) && *p != '_') return 0;
    }
    return 1;
}

void traiter_affectation(char *ligne) {
    char *egal = strchr(ligne, '=');
    *egal = '\0';
    set_variable(ligne, egal + 1);
}

/* ===================== Fichier .ocshrc ===================== */

void executer_ligne(char *ligne);  /* déclaration anticipée */

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
    strncpy(copie, expanded, MAX_LINE - 1);
    copie[MAX_LINE - 1] = '\0';

    char *args[MAX_ARGS];
    int argc = decouper(copie, args);
    if (argc == 0) return 1;

    traduire_alias(args);

    pid_t pid = fork();
    if (pid == 0) {
        /* Rediriger la sortie vers /dev/null pour pas polluer */
        appliquer_redirections(args, &argc);
        if (est_builtin(args[0])) {
            executer_builtin(args, argc);
            exit(0);
        }
        execvp(args[0], args);
        exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
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

        /* Commande normale */
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

    /* Chercher dans les fichiers du répertoire courant */
    /* On utilise popen avec ls pour rester simple */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -1 2>/dev/null");
    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    char nom[256];
    while (fgets(nom, sizeof(nom), fp)) {
        nom[strcspn(nom, "\n")] = '\0';
        if (strncmp(nom, prefixe, len_pref) == 0) {
            char *reste = nom + len_pref;
            int len_reste = strlen(reste);
            for (int j = 0; j < len_reste; j++)
                ligne[(*pos)++] = reste[j];
            ligne[*pos] = '\0';
            printf("%s", reste);
            fflush(stdout);
            pclose(fp);
            return;
        }
    }
    pclose(fp);
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
            if (fgets(ligne, MAX_LINE, stdin) == NULL)
                break;
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
