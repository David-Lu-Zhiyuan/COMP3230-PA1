#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>

#define SH_MAX_LINE     4096
#define SH_MAX_WORDS    30
#define SH_MAX_PIPES    4
#define SH_WATCH_SEC    2
#define SH_PROMPT       "## 3230yash>> "

typedef struct {
    volatile sig_atomic_t fg_pid;
    volatile sig_atomic_t stop_watch;
} ShellState;

static ShellState g_sh = {0, 0};

static void sh_sigint_watch(int sig);
static void sh_sigint_main(int sig);
static void sh_print_signal(const char *name, int sig);
static int  sh_split(char *line, char **words);
static int  sh_check_pipes(char **words, int n);
static int  sh_exec_one(char **words, int in_fd, int out_fd);
static int  sh_exec_pipe(char **words, int n);
static int  sh_read_stat(pid_t pid, char *state, int *cpu,
                         unsigned long *utime, unsigned long *stime,
                         unsigned long *vsize, unsigned long *minflt,
                         unsigned long *majflt);
static void sh_watch_child(pid_t pid);
static int  sh_builtin(char **words, int n);
static void sh_init(void);
static void sh_loop(void);

static void sh_sigint_watch(int sig) {
    g_sh.stop_watch = 1;
}

static void sh_sigint_main(int sig) {
    if (g_sh.fg_pid > 0) {
        kill(g_sh.fg_pid, SIGINT);
    } else {
        printf("\n" SH_PROMPT);
        fflush(stdout);
    }
}

static void sh_print_signal(const char *name, int sig) {
    if (sig == SIGPIPE) {
        fprintf(stderr, "%s: Broken pipe\n", name);
    } else if (sig == SIGINT) {
        fprintf(stderr, "%s: Interrupt\n", name);
    } else if (sig == SIGKILL) {
        fprintf(stderr, "%s: Killed\n", name);
    } else {
        fprintf(stderr, "%s: Terminated by signal: %d\n", name, sig);
    }
}

static int sh_split(char *line, char **words) {
    int n = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == '\0') break;

        if (n >= SH_MAX_WORDS) return -1;

        words[n++] = p;

        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    words[n] = NULL;
    return n;
}

static int sh_check_pipes(char **words, int n) {
    if (n == 0) return 0;

    if (strcmp(words[0], "|") == 0 || strcmp(words[n - 1], "|") == 0) {
        return -1;
    }

    for (int i = 0; i < n - 1; i++) {
        if (strcmp(words[i], "|") == 0 && strcmp(words[i + 1], "|") == 0) {
            return -2;
        }
    }
    return 0;
}

static int sh_exec_one(char **words, int in_fd, int out_fd) {
    pid_t pid = fork();

    if (pid == 0) {
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);

        if (in_fd != STDIN_FILENO) {
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
        }
        if (out_fd != STDOUT_FILENO) {
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
        }

        execvp(words[0], words);
        fprintf(stderr, "3230yash: '%s': %s\n", words[0], strerror(errno));
        exit(EXIT_FAILURE);

    } else if (pid > 0) {
        g_sh.fg_pid = pid;
        int status;
        waitpid(pid, &status, 0);
        g_sh.fg_pid = 0;

        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            sh_print_signal(words[0], sig);
            return -1;
        }
    } else {
        perror("3230yash: fork");
        return -1;
    }
    return 0;
}

static int sh_exec_pipe(char **words, int n) {
    pid_t pids[SH_MAX_PIPES + 1];
    char *cmd_names[SH_MAX_PIPES + 1];
    int cmd_count = 0;

    int prev_read_fd = STDIN_FILENO;
    int i = 0;

    while (i < n) {
        int start = i;
        int end = i;
        while (end < n && strcmp(words[end], "|") != 0) {
            end++;
        }

        char *sub[SH_MAX_WORDS + 1];
        int sub_n = 0;
        for (int j = start; j < end; j++) {
            sub[sub_n++] = words[j];
        }
        sub[sub_n] = NULL;

        if (sub_n == 0) {
            return -1;
        }

        int out_fd;
        int pipe_fds[2] = {-1, -1};

        if (end < n) {
            if (pipe(pipe_fds) == -1) {
                perror("3230yash: pipe");
                return -1;
            }
            out_fd = pipe_fds[1];
        } else {
            out_fd = STDOUT_FILENO;
        }

        pid_t pid = fork();
        if (pid == 0) {
            struct sigaction sa;
            sa.sa_handler = SIG_DFL;
            sa.sa_flags = 0;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGPIPE, &sa, NULL);
            sigaction(SIGINT, &sa, NULL);

            if (prev_read_fd != STDIN_FILENO) {
                dup2(prev_read_fd, STDIN_FILENO);
                close(prev_read_fd);
            }
            if (out_fd != STDOUT_FILENO) {
                dup2(out_fd, STDOUT_FILENO);
                close(out_fd);
            }
            if (pipe_fds[0] != -1) {
                close(pipe_fds[0]);
            }

            execvp(sub[0], sub);
            fprintf(stderr, "3230yash: '%s': %s\n", sub[0], strerror(errno));
            exit(EXIT_FAILURE);
        } else if (pid > 0) {
            pids[cmd_count] = pid;
            cmd_names[cmd_count] = sub[0];
            cmd_count++;

            if (prev_read_fd != STDIN_FILENO) {
                close(prev_read_fd);
            }
            if (out_fd != STDOUT_FILENO) {
                close(out_fd);
            }
            if (pipe_fds[0] != -1) {
                prev_read_fd = pipe_fds[0];
            } else {
                prev_read_fd = STDIN_FILENO;
            }
        } else {
            perror("3230yash: fork");
            return -1;
        }

        if (end < n) {
            i = end + 1;
        } else {
            break;
        }
    }

    if (prev_read_fd != STDIN_FILENO) {
        close(prev_read_fd);
    }

    int last_status = 0;
    for (int k = 0; k < cmd_count - 1; k++) {
        int status;
        pid_t waited = waitpid(pids[k], &status, 0);
        if (waited > 0 && WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            sh_print_signal(cmd_names[k], sig);
        }
    }
    if (cmd_count > 0) {
        int status;
        pid_t waited = waitpid(pids[cmd_count - 1], &status, 0);
        if (waited > 0) {
            last_status = status;
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                sh_print_signal(cmd_names[cmd_count - 1], sig);
            }
        }
    }

    return WEXITSTATUS(last_status);
}

static int sh_read_stat(pid_t pid, char *state, int *cpu,
                        unsigned long *utime, unsigned long *stime,
                        unsigned long *vsize, unsigned long *minflt,
                        unsigned long *majflt) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    char *lparen = strchr(line, '(');
    char *rparen = strrchr(line, ')');
    if (!lparen || !rparen || rparen < lparen) return -1;

    char *p = rparen + 1;
    while (*p == ' ') p++;

    char *fields[64];
    int nfields = 0;

    while (*p && nfields < 64) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        fields[nfields++] = p;

        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }

    if (nfields < 37) return -1;

    *state  = fields[0][0];
    *minflt = strtoul(fields[7], NULL, 10);
    *majflt = strtoul(fields[9], NULL, 10);
    *utime  = strtoul(fields[11], NULL, 10);
    *stime  = strtoul(fields[12], NULL, 10);
    *vsize  = strtoul(fields[20], NULL, 10);
    *cpu    = atoi(fields[36]);

    return 0;
}

static void sh_watch_child(pid_t pid) {
    printf("STATE CPUID UTIME STIME VSIZE MINFLT MAJFLT\n");

    int last_cpu = -1;
    char last_state = '?';
    unsigned long last_utime = 0, last_stime = 0;
    unsigned long last_vsize = 0, last_minflt = 0, last_majflt = 0;
    int first = 1;

    struct sigaction sa;
    sa.sa_handler = sh_sigint_watch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    struct timespec ts;
    ts.tv_sec = SH_WATCH_SEC;
    ts.tv_nsec = 0;

    while (!g_sh.stop_watch) {
        char state;
        int cpu;
        unsigned long utime, stime, vsize, minflt, majflt;

        if (sh_read_stat(pid, &state, &cpu, &utime, &stime,
                         &vsize, &minflt, &majflt) == 0) {
            if (first || state != last_state || cpu != last_cpu ||
                utime != last_utime || stime != last_stime ||
                vsize != last_vsize || minflt != last_minflt ||
                majflt != last_majflt) {

                long clk = sysconf(_SC_CLK_TCK);
                double ut = (double)utime / clk;
                double st = (double)stime / clk;

                printf("%c %d %.2f %.2f %lu %lu %lu\n",
                       state, cpu, ut, st, vsize, minflt, majflt);
                fflush(stdout);

                last_state = state;
                last_cpu = cpu;
                last_utime = utime;
                last_stime = stime;
                last_vsize = vsize;
                last_minflt = minflt;
                last_majflt = majflt;
                first = 0;
            }
        } else {
            break;
        }

        if (kill(pid, 0) == -1 && errno == ESRCH) break;

        if (nanosleep(&ts, NULL) == -1 && errno == EINTR) {
        }

        if (g_sh.stop_watch) {
            kill(pid, SIGTERM);
            break;
        }
    }

    int status;
    waitpid(pid, &status, 0);

    sa.sa_handler = sh_sigint_main;
    sigaction(SIGINT, &sa, NULL);
    g_sh.stop_watch = 0;
}

static int sh_builtin(char **words, int n) {
    if (n == 0) return 0;

    if (strcmp(words[0], "exit") == 0) {
        if (n == 1) {
            printf("3230yash: Terminated\n");
            exit(EXIT_SUCCESS);
        } else {
            printf("3230yash: \"exit\" with other arguments!!!\n");
            return 0;
        }
    }

    if (strcmp(words[0], "watch") == 0) {
        if (n == 1) {
            printf("3230yash: watch: missing command\n");
            return 0;
        }

        for (int i = 1; i < n; i++) {
            if (strcmp(words[i], "|") == 0) {
                printf("3230yash: Cannot watch a pipe sequence\n");
                return 0;
            }
        }

        pid_t pid = fork();

        if (pid == 0) {
            char *sub[SH_MAX_WORDS];
            int sub_n = 0;
            for (int i = 1; i < n; i++) sub[sub_n++] = words[i];
            sub[sub_n] = NULL;

            execvp(sub[0], sub);
            fprintf(stderr, "3230yash: '%s': %s\n", sub[0], strerror(errno));
            exit(EXIT_FAILURE);

        } else if (pid > 0) {
            sh_watch_child(pid);
            return 0;
        } else {
            perror("3230yash: fork");
            return -1;
        }
    }

    return -1;
}

static void sh_init(void) {
    struct sigaction sa;
    sa.sa_handler = sh_sigint_main;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("3230yash: sigaction");
    }
    setlinebuf(stdout);
}

static void sh_loop(void) {
    char line[SH_MAX_LINE];

    while (1) {
        printf(SH_PROMPT);
        fflush(stdout);

        if (fgets(line, SH_MAX_LINE, stdin) == NULL) {
            if (feof(stdin)) {
                printf("\n");
                break;
            }
            continue;
        }

        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;

        char copy[SH_MAX_LINE];
        strncpy(copy, line, SH_MAX_LINE - 1);
        copy[SH_MAX_LINE - 1] = '\0';

        char *words[SH_MAX_WORDS + 1];
        int n = sh_split(copy, words);

        if (n == -1) {
            printf("3230yash: Too many arguments\n");
            continue;
        }
        if (n == 0) continue;

        int pipe_check = sh_check_pipes(words, n);
        if (pipe_check == -1) {
            printf("3230yash: Incorrect pipe sequence\n");
            continue;
        } else if (pipe_check == -2) {
            printf("3230yash: should not have two consecutive | without in-between command\n");
            continue;
        }

        if (sh_builtin(words, n) == 0) continue;

        int has_pipe = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(words[i], "|") == 0) {
                has_pipe = 1;
                break;
            }
        }

        if (!has_pipe) {
            sh_exec_one(words, STDIN_FILENO, STDOUT_FILENO);
        } else {
            sh_exec_pipe(words, n);
        }
    }
}

int main(void) {
    sh_init();
    sh_loop();
    return EXIT_SUCCESS;
}
