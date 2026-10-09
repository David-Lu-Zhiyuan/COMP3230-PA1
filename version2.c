// COMP3230 Programming Assignment 1
// Name: Lu Zhiyuan
// UID: 3036127517
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

#define MAX_INPUT_LENGTH 4096
#define MAX_ARGS 30
#define MAX_PIPES 4
#define WATCH_INTERVAL 2  // Monitoring interval (seconds)

// Global variable for signal handling
volatile sig_atomic_t child_pid = 0;
volatile sig_atomic_t should_exit_watch = 0;

// Signal handler for watch command
void watch_sigint_handler(int sig) {
    should_exit_watch = 1;
}

// Signal handler function for main shell
void sigint_handler(int sig) {
    if (child_pid > 0) {
        // Forward SIGINT to child process
        kill(child_pid, SIGINT);
    } else {
        // Just print new prompt, don't terminate shell
        printf("\n## 3230yash>> ");
        fflush(stdout);
    }
}

// 新增：统一打印信号终止消息
static void print_signal_message(const char *cmd_name, int sig) {
    if (sig == SIGPIPE) {
        fprintf(stderr, "%s: Broken pipe\n", cmd_name);
    } else if (sig == SIGINT) {
        fprintf(stderr, "%s: Interrupt\n", cmd_name);
    } else if (sig == SIGKILL) {
        fprintf(stderr, "%s: Killed\n", cmd_name);
    } else {
        fprintf(stderr, "%s: Terminated by signal: %d\n", cmd_name, sig);
    }
}

// Parse input command
int parse_input(char *input, char **args) {
    int argc = 0;
    char *token = strtok(input, " \t\n");
    
    while (token != NULL) {
        if (argc >= MAX_ARGS) {
            return -1; // Too many arguments
        }
        args[argc++] = token;
        token = strtok(NULL, " \t\n");
    }
    args[argc] = NULL;
    return argc;
}

// Check pipe syntax
// 返回值：0 合法，-1 管道在开头/结尾，-2 连续管道
int check_pipe_syntax(char **args, int argc) {
    if (argc == 0) return 0;
    
    if (strcmp(args[0], "|") == 0 || strcmp(args[argc-1], "|") == 0) {
        return -1;  // 管道在开头或结尾
    }
    
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(args[i], "|") == 0 && strcmp(args[i+1], "|") == 0) {
            return -2;  // 连续管道
        }
    }
    
    return 0;
}

// Execute single command
int execute_command(char **args, int input_fd, int output_fd) {
    pid_t pid = fork();
    
    if (pid == 0) {
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);  // 修复：子进程恢复默认 SIGINT
        
        if (input_fd != STDIN_FILENO) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        if (output_fd != STDOUT_FILENO) {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }
        
        execvp(args[0], args);
        fprintf(stderr, "3230yash: '%s': %s\n", args[0], strerror(errno));  // 加单引号
        exit(EXIT_FAILURE);
        
    } else if (pid > 0) {
        child_pid = pid;
        int status;
        waitpid(pid, &status, 0);
        child_pid = 0;
        
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            print_signal_message(args[0], sig);  // 统一格式
            return -1;
        }
    } else {
        perror("3230yash: fork");
        return -1;
    }
    return 0;
}

// Handle pipe commands
int handle_pipes(char **args, int argc) {
    int pipe_count = 0;
    int pipe_positions[MAX_PIPES] = {0};
    
    // Find all pipe positions
    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "|") == 0) {
            pipe_positions[pipe_count++] = i;
            args[i] = NULL;
        }
    }
    
    if (pipe_count == 0) {
        return execute_command(args, STDIN_FILENO, STDOUT_FILENO);
    }
    
    // 新增：管道数量上限检查
    if (pipe_count > MAX_PIPES) {
        fprintf(stderr, "3230yash: Too many pipes\n");
        return -1;
    }
    
    int pipefds[2 * pipe_count];
    pid_t pids[pipe_count + 1];
    
    // Create all pipes
    for (int i = 0; i < pipe_count; i++) {
        if (pipe(pipefds + 2*i) == -1) {
            perror("3230yash: pipe");
            return -1;
        }
    }
    
    // Create child processes
    for (int cmd_index = 0; cmd_index <= pipe_count; cmd_index++) {
        pids[cmd_index] = fork();
        
        if (pids[cmd_index] == 0) {
            // Child process code
            struct sigaction sa;
            sa.sa_handler = SIG_DFL;
            sa.sa_flags = 0;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGPIPE, &sa, NULL);
            sigaction(SIGINT, &sa, NULL);
            
            // Set input/output redirection
            if (cmd_index > 0) {
                close(pipefds[2*(cmd_index-1) + 1]);
                dup2(pipefds[2*(cmd_index-1)], STDIN_FILENO);
            }
            if (cmd_index < pipe_count) {
                close(pipefds[2*cmd_index]);
                dup2(pipefds[2*cmd_index + 1], STDOUT_FILENO);
            }
            
            // Close all pipe file descriptors
            for (int j = 0; j < 2 * pipe_count; j++) {
                close(pipefds[j]);
            }
            
            // Execute command
            int start = (cmd_index == 0) ? 0 : (pipe_positions[cmd_index-1] + 1);
            int end = (cmd_index < pipe_count) ? pipe_positions[cmd_index] : argc;
            
            char *cmd_args[MAX_ARGS + 1];
            int arg_count = 0;
            for (int i = start; i < end && args[i] != NULL; i++) {
                cmd_args[arg_count++] = args[i];
            }
            cmd_args[arg_count] = NULL;
            
            if (arg_count > 0) {
                execvp(cmd_args[0], cmd_args);
                fprintf(stderr, "3230yash: '%s': %s\n", cmd_args[0], strerror(errno));
                exit(EXIT_FAILURE);
            }
            exit(EXIT_FAILURE);
            
        } else if (pids[cmd_index] < 0) {
            perror("3230yash: fork");
            for (int j = 0; j < 2 * pipe_count; j++) {
                close(pipefds[j]);
            }
            return -1;
        }
    }
    
    // Parent process: Close all pipe file descriptors
    for (int i = 0; i < 2 * pipe_count; i++) {
        close(pipefds[i]);
    }
    
    int last_status = 0;
    
    // Phase 1: First wait for all non-last processes
    for (int i = 0; i < pipe_count; i++) {
        int status;
        pid_t waited_pid = waitpid(pids[i], &status, 0);
        
        if (waited_pid > 0) {
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                char *cmd_name = "unknown";
                int start = (i == 0) ? 0 : (pipe_positions[i-1] + 1);
                int end = (i < pipe_count) ? pipe_positions[i] : argc;
                
                if (start < end && args[start] != NULL) {
                    cmd_name = args[start];
                }
                
                print_signal_message(cmd_name, sig);
            }
        }
    }
    
    // Phase 2: Finally wait for the last process
    int status;
    pid_t waited_pid = waitpid(pids[pipe_count], &status, 0);
    
    if (waited_pid > 0) {
        last_status = status;
        
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            char *cmd_name = "unknown";
            int start = pipe_positions[pipe_count-1] + 1;
            int end = argc;
            
            if (start < end && args[start] != NULL) {
                cmd_name = args[start];
            }
            
            print_signal_message(cmd_name, sig);
        }
    }
    
    return WEXITSTATUS(last_status);
}

// Function to read process statistics from /proc/{pid}/stat
int read_proc_stat(pid_t pid, char *state, int *cpu_id, unsigned long *utime, 
                   unsigned long *stime, unsigned long *vsize, 
                   unsigned long *minflt, unsigned long *majflt) {
    char stat_path[256];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    
    FILE *fp = fopen(stat_path, "r");
    if (!fp) {
        return -1;
    }
    
    // Read the entire stat file
    char line[1024];
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    
    // Parse the stat file according to the manual page format
    int parsed_pid;
    char comm[256];
    char proc_state;
    
    int fields = sscanf(line, "%d %s %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %lu %*d %*u %lu %lu",
                       &parsed_pid, comm, &proc_state, utime, stime, vsize, minflt, majflt);
    
    if (fields < 8) {
        return -1;
    }
    
    *state = proc_state;
    
    // Get CPU ID from field 39 (0-indexed)
    char *token = strtok(line, " ");
    for (int i = 0; i < 38 && token != NULL; i++) {
        token = strtok(NULL, " ");
    }
    if (token != NULL) {
        *cpu_id = atoi(token);
    } else {
        *cpu_id = -1;
    }
    
    return 0;
}

// Function to monitor a single process
void monitor_process(pid_t pid) {
    // Print table header (without HTML tags)
    printf("STATE CPUID UTIME STIME VSIZE MINFLT MAJFLT\n");
    
    int last_cpu_id = -1;
    char last_state = '?';
    unsigned long last_utime = 0, last_stime = 0;
    unsigned long last_vsize = 0, last_minflt = 0, last_majflt = 0;
    
    int first_print = 1;
    
    // Set up signal handler for watch command
    struct sigaction sa;
    sa.sa_handler = watch_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    while (!should_exit_watch) {
        char state;
        int cpu_id;
        unsigned long utime, stime, vsize, minflt, majflt;
        
        if (read_proc_stat(pid, &state, &cpu_id, &utime, &stime, &vsize, &minflt, &majflt) == 0) {
            // Only print if there are changes or it's the first print
            if (first_print || state != last_state || cpu_id != last_cpu_id ||
                utime != last_utime || stime != last_stime ||
                vsize != last_vsize || minflt != last_minflt || majflt != last_majflt) {
                
                // Convert clock ticks to seconds (divide by sysconf(_SC_CLK_TCK))
                long clk_tck = sysconf(_SC_CLK_TCK);
                double utime_sec = (double)utime / clk_tck;
                double stime_sec = (double)stime / clk_tck;
                
                // Print data in table format (without HTML tags)
                printf("%c %d %.2f %.2f %lu %lu %lu\n", 
                       state, cpu_id, utime_sec, stime_sec, vsize, minflt, majflt);
                fflush(stdout);
                
                last_state = state;
                last_cpu_id = cpu_id;
                last_utime = utime;
                last_stime = stime;
                last_vsize = vsize;
                last_minflt = minflt;
                last_majflt = majflt;
                first_print = 0;
            }
        } else {
            // Process no longer exists
            break;
        }
        
        // Check if process still exists
        if (kill(pid, 0) == -1 && errno == ESRCH) {
            break;
        }
        
        // Sleep with interruptible sleep to allow signal handling
        for (int i = 0; i < WATCH_INTERVAL * 10 && !should_exit_watch; i++) {
            usleep(100000); // Sleep 100ms at a time
        }
        
        if (should_exit_watch) {
            // Kill the monitored process if user pressed Ctrl+C
            kill(pid, SIGTERM);
            break;
        }
    }
    
    // Wait for the process to finish
    int status;
    waitpid(pid, &status, 0);
    
    // Restore original signal handler
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    should_exit_watch = 0;
}

// Handle built-in commands
int handle_builtin(char **args, int argc) {
    if (argc == 0) return 0;

    if (strcmp(args[0], "exit") == 0) {
        if (argc == 1) {
            printf("3230yash: Terminated\n");
            exit(EXIT_SUCCESS);
        } else {
            printf("3230yash: \"exit\" with other arguments!!!\n");
            return 0;
        }
    }
    
    if (strcmp(args[0], "watch") == 0) {
        if (argc == 1) {
            printf("3230yash: watch: missing command\n");
            return 0;
        }
        
        // Check if the command contains pipes
        for (int i = 1; i < argc; i++) {
            if (strcmp(args[i], "|") == 0) {
                printf("3230yash: Cannot watch a pipe sequence\n");
                return 0;
            }
        }
        
        // Fork and execute the command to monitor
        pid_t pid = fork();
        
        if (pid == 0) {
            // Child process: execute the command
            char *cmd_args[MAX_ARGS];
            int cmd_argc = 0;
            
            for (int i = 1; i < argc; i++) {
                cmd_args[cmd_argc++] = args[i];
            }
            cmd_args[cmd_argc] = NULL;
            
            execvp(cmd_args[0], cmd_args);
            fprintf(stderr, "3230yash: '%s': %s\n", cmd_args[0], strerror(errno));
            exit(EXIT_FAILURE);
            
        } else if (pid > 0) {
            // Parent process: monitor the child
            monitor_process(pid);
            return 0;
        } else {
            perror("3230yash: fork");
            return -1;
        }
    }
    
    return -1; // Not a built-in command
}

// 新增：初始化 shell
static void setup_shell(void) {
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("3230yash: sigaction");
    }
    
    setlinebuf(stdout);
}

// 新增：主循环
static void shell_loop(void) {
    char input[MAX_INPUT_LENGTH];
    
    while (1) {
        printf("## 3230yash>> ");
        fflush(stdout);
        
        if (fgets(input, MAX_INPUT_LENGTH, stdin) == NULL) {
            if (feof(stdin)) {
                printf("\n");
                break;
            }
            continue;
        }
        
        input[strcspn(input, "\n")] = '\0';
        
        if (strlen(input) == 0) {
            continue;
        }
        
        char command_copy[MAX_INPUT_LENGTH];
        strncpy(command_copy, input, MAX_INPUT_LENGTH - 1);
        command_copy[MAX_INPUT_LENGTH - 1] = '\0';
        
        char *args[MAX_ARGS + 1];
        int argc = parse_input(command_copy, args);
        
        if (argc == -1) {
            printf("3230yash: Too many arguments\n");
            continue;
        }
        if (argc == 0) continue;
        
        int pipe_check = check_pipe_syntax(args, argc);
        if (pipe_check == -1) {
            printf("3230yash: Incorrect pipe sequence\n");
            continue;
        } else if (pipe_check == -2) {
            printf("3230yash: should not have two consecutive | without in-between command\n");
            continue;
        }
        
        if (handle_builtin(args, argc) == 0) {
            continue;
        }
        
        handle_pipes(args, argc);
    }
}

int main() {
    setup_shell();
    shell_loop();
    return EXIT_SUCCESS;
}
