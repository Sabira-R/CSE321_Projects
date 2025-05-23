#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>
#include <signal.h>

#define MAX_INPUT 2000
#define MAX_TOKENS 200
#define CMD_HISTORY 146

char* cmd_history[CMD_HISTORY];
int cmd_count = 0;
volatile sig_atomic_t running_pid = -1;

// -------------------------- Signal Handler ------------------------------------

void ctrl_c_handler(int signo) {
    if (running_pid > 0) {
        kill(running_pid, SIGINT);
    } else {
        printf("\nInterrupted.\nProject> ");
        fflush(stdout);
    }
}

// -------------------------------- History Tracking ----------------------------------

void save_history(const char *cmd) {
    if (cmd_count < CMD_HISTORY) {
        cmd_history[cmd_count++] = strdup(cmd);
    } else {
        free(cmd_history[0]);
        for (int i = 1; i < CMD_HISTORY; i++) {
            cmd_history[i - 1] = cmd_history[i];
        }
        cmd_history[CMD_HISTORY - 1] = strdup(cmd);
    }
}

void display_history() {
    for (int i = 0; i < cmd_count; i++) {
        printf("%d: %s", i + 1, cmd_history[i]);
    }
}
void replay_history(int index);

// ------------------------- Tokenizer ---------------------------------

void tokenize(char *line, char **tokens) {
    int pos = 0;
    tokens[pos] = strtok(line, " \t\n");
    while (tokens[pos] != NULL && pos < MAX_TOKENS - 1) {
        pos++;
        tokens[pos] = strtok(NULL, " \t\n");
    }
    tokens[pos] = NULL;
}

// ------------------------ Redirection ---------------------------

void handle_redirects(char **tokens) {
    for (int i = 0; tokens[i] != NULL; i++) {
        if (strcmp(tokens[i], "<") == 0 && tokens[i + 1]) {
            int in_fd = open(tokens[i + 1], O_RDONLY);
            if (in_fd < 0) {
                perror("Input redirection error");
                exit(EXIT_FAILURE);
            }
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
            tokens[i] = NULL;
        } else if (strcmp(tokens[i], ">") == 0 && tokens[i + 1]) {
            int out_fd = open(tokens[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd < 0) {
                perror("Output redirection error");
                exit(EXIT_FAILURE);
            }
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
            tokens[i] = NULL;
        } else if (strcmp(tokens[i], ">>") == 0 && tokens[i + 1]) {
            int app_fd = open(tokens[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (app_fd < 0) {
                perror("Append redirection error");
                exit(EXIT_FAILURE);
            }
            dup2(app_fd, STDOUT_FILENO);
            close(app_fd);
            tokens[i] = NULL;
        }
    }
}

// ------------------------------ Piped Execution ---------------------------

void handle_pipes(char *input_line) {
    char *segments[10];
    int total = 0;

    segments[total] = strtok(input_line, "|");
    while (segments[total] != NULL && total < 9) {
        total++;
        segments[total] = strtok(NULL, "|");
    }

    int pipes[2 * (total - 1)];
    for (int i = 0; i < total - 1; i++) {
        pipe(pipes + i * 2);
    }

    for (int i = 0; i < total; i++) {
        char *tokens[MAX_TOKENS];
        tokenize(segments[i], tokens);

        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0) dup2(pipes[(i - 1) * 2], STDIN_FILENO);
            if (i < total - 1) dup2(pipes[i * 2 + 1], STDOUT_FILENO);

            for (int j = 0; j < 2 * (total - 1); j++) close(pipes[j]);

            handle_redirects(tokens);
            execvp(tokens[0], tokens);
            perror("pipe exec failed");
            exit(EXIT_FAILURE);
        } else if (pid < 0) {
            perror("pipe fork error");
            return;
        }
    }

    for (int i = 0; i < 2 * (total - 1); i++) close(pipes[i]);
    for (int i = 0; i < total; i++) wait(NULL);
}

// ----------------------------------- Command Execution ----------------------------------

int run_command(char *line) {
    char *tokens[MAX_TOKENS];
    tokenize(line, tokens);

    if (tokens[0] == NULL) return 1;

    if (strcmp(tokens[0], "cd") == 0) {
        if (tokens[1] == NULL) {
            fprintf(stderr, "cd: missing path\n");
            return 0;
        } else if (chdir(tokens[1]) != 0) {
            perror("cd error");
            return 0;
        }
        return 1;
    }

    if (strcmp(tokens[0], "pwd") == 0) {
        char dir[MAX_INPUT];
        getcwd(dir, sizeof(dir));
        printf("%s\n", dir);
        return 1;
    }

    if (strcmp(tokens[0], "exit") == 0) {
        exit(0);
    }

    if (strcmp(tokens[0], "history") == 0) {
        display_history();
        return 1;
    }

    if (tokens[0][0] == '!' && isdigit(tokens[0][1])) {
        int idx = atoi(tokens[0] + 1);
        replay_history(idx);
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        handle_redirects(tokens);
        execvp(tokens[0], tokens);
        perror("exec failed");
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        int status;
        running_pid = pid;
        waitpid(pid, &status, 0);
        running_pid = -1;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 1; 
        } else {
            return 0; 
        }
    } else {
        perror("fork error");
        return 0;
    }
}
void replay_history(int index) {
    if (index >= 1 && index <= cmd_count) {
        printf("Running: %s", cmd_history[index - 1]);
        run_command(cmd_history[index - 1]);
    } else {
        printf("Invalid history index.\n");
    }}

// ------------------------------------ Command Sequencing --------------------------------------

int process_sequence(char *input) {
    char *part1;
    char *stmt = strtok_r(input, ";", &part1);

    while (stmt != NULL) {
        char *part2;
        char *substmt = strtok_r(stmt, "&&", &part2);
        int okay = 1;

        while (substmt != NULL) {
            while (*substmt == ' ') substmt++;
            size_t len = strlen(substmt);
            while (len > 0 && (substmt[len - 1] == ' ' || substmt[len - 1] == '\n')) {
                substmt[len - 1] = '\0';
                len--;
            }
            if (strlen(substmt) > 0 && okay) {
                okay = run_command(substmt); 
            }
            substmt = strtok_r(NULL, "&&", &part2);
        }
        stmt = strtok_r(NULL, ";", &part1);
    }

    return 1;
}

// ------------------------------------- Main ------------------------------------------------------

int main() {
    char user_input[MAX_INPUT];
    struct sigaction sa;
    sa.sa_handler = ctrl_c_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    while (1) {
        printf("Project> ");
        fflush(stdout);

        if (fgets(user_input, sizeof(user_input), stdin) == NULL) {
            perror("Reading input failed");
            continue;
        }
        if (user_input[0] != '\n') {
            save_history(user_input);
        }
        if (strchr(user_input, '|')) {
            handle_pipes(user_input);
        } else {
            process_sequence(user_input);
        }
    }

    for (int i = 0; i < cmd_count; i++) {
        free(cmd_history[i]);
    }

    return 0;
}

