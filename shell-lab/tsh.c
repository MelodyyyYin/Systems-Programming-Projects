/**
 * @file tsh.c
 * @brief A tiny shell that is implemented incrementally across lab stages.
 *
 * This file owns the shell read/eval loop, builtin dispatch, process creation,
 * and signal handling needed for the Shell Lab assignment. The implementation
 * in this checkpoint covers stage 0 through stage 5 of the project plan:
 * lifecycle management, the `quit` builtin, and basic foreground command
 * execution for non-builtin programs, background jobs, signal forwarding, and
 * job-control state transitions through `bg` and `fg`, plus command
 * redirection and final error handling.
 * @author Melody Yin <melodyyi@andrew.cmu.edu>
 */

#include "csapp.h"
#include "tsh_helper.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* Function prototypes */
void eval(const char *cmdline);

static void init_job_sigset(sigset_t *mask);
static void block_job_signals(sigset_t *prev_mask);
static void restore_signal_mask(const sigset_t *prev_mask);
static void waitfg(pid_t pid);
static bool apply_redirection(const struct cmdline_tokens *token);
static void builtin_jobs(const struct cmdline_tokens *token);
static bool parse_job_arg(const char *arg, bool *is_jid, int *value);
static void builtin_bgfg(const struct cmdline_tokens *token);
static void run_external(const char *cmdline,
                         const struct cmdline_tokens *token, job_state state);
static void forward_signal_to_fg(int sig);
static size_t append_literal(char *buf, size_t pos, const char *text);
static size_t append_unsigned(char *buf, size_t pos, unsigned long value);
static void write_all(int fd, const char *buf, size_t len);
static void print_job_signal_message(jid_t jid, pid_t pid, int sig,
                                     const char *action);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);
void cleanup(void);

/**
 * @brief Initialize shell process state, install handlers, and run the REPL.
 *
 * The shell sets up its environment and helper-managed job list, installs the
 * signal handlers required by later stages, and then repeatedly prompts for,
 * reads, and evaluates command lines until EOF or the `quit` builtin ends the
 * process.
 *
 * @param[in] argc Argument count from the host process.
 * @param[in] argv Argument vector from the host process.
 * @return This function only returns when the shell exits on EOF.
 */
int main(int argc, char **argv) {
    int c;
    char cmdline[MAXLINE_TSH]; // Cmdline for fgets
    bool emit_prompt = true;   // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        perror("dup2 error");
        exit(1);
    }

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h': // Prints help message
            usage();
            break;
        case 'v': // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p': // Disables prompt printing
            emit_prompt = false;
            break;
        default:
            usage();
        }
    }

    // Create environment variable
    if (putenv(strdup("MY_ENV=42")) < 0) {
        perror("putenv error");
        exit(1);
    }

    // Set buffering mode of stdout to line buffering.
    // This prevents lines from being printed in the wrong order.
    if (setvbuf(stdout, NULL, _IOLBF, 0) < 0) {
        perror("setvbuf error");
        exit(1);
    }

    // Initialize the job list
    init_job_list();

    // Register a function to clean up the job list on program termination.
    // The function may not run in the case of abnormal termination (e.g. when
    // using exit or terminating due to a signal handler), so in those cases,
    // we trust that the OS will clean up any remaining resources.
    if (atexit(cleanup) < 0) {
        perror("atexit error");
        exit(1);
    }

    // Install the signal handlers
    Signal(SIGINT, sigint_handler);   // Handles Ctrl-C
    Signal(SIGTSTP, sigtstp_handler); // Handles Ctrl-Z
    Signal(SIGCHLD, sigchld_handler); // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler);

    // Execute the shell's read/eval loop
    while (true) {
        if (emit_prompt) {
            printf("%s", prompt);

            // We must flush stdout since we are not printing a full line.
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin)) {
            perror("fgets error");
            exit(1);
        }

        if (feof(stdin)) {
            // End of file (Ctrl-D)
            printf("\n");
            return 0;
        }

        // Remove any trailing newline
        char *newline = strchr(cmdline, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        // Evaluate the command line
        eval(cmdline);
    }

    return -1; // control never reaches here
}

/**
 * @brief Build the signal set used to protect all job-list accesses.
 *
 * The helper job-list API requires SIGCHLD, SIGINT, and SIGTSTP to be blocked
 * whenever the shell reads or mutates shared job state.
 *
 * @param[out] mask Signal set populated for job critical sections.
 */
static void init_job_sigset(sigset_t *mask) {
    sigemptyset(mask);
    sigaddset(mask, SIGCHLD);
    sigaddset(mask, SIGINT);
    sigaddset(mask, SIGTSTP);
}

/**
 * @brief Block job-related signals for a critical section.
 *
 * @param[out] prev_mask Previous signal mask to restore after the critical
 *                       section completes.
 */
static void block_job_signals(sigset_t *prev_mask) {
    sigset_t mask;

    init_job_sigset(&mask);
    sigprocmask(SIG_BLOCK, &mask, prev_mask);
}

/**
 * @brief Restore a previously saved signal mask.
 *
 * @param[in] prev_mask Signal mask snapshot returned by block_job_signals.
 */
static void restore_signal_mask(const sigset_t *prev_mask) {
    sigprocmask(SIG_SETMASK, prev_mask, NULL);
}

/**
 * @brief Wait until a foreground job leaves the foreground state.
 *
 * Child status changes are reaped only in SIGCHLD, so this helper waits by
 * checking protected job-list state and then sleeping with sigsuspend.
 *
 * @param[in] pid Root process ID of the foreground job.
 */
static void waitfg(pid_t pid) {
    sigset_t prev_mask;
    jid_t jid;

    block_job_signals(&prev_mask);

    while (true) {
        jid = job_from_pid(pid);
        if (jid == 0 || job_get_state(jid) != FG) {
            break;
        }
        sigsuspend(&prev_mask);
    }

    restore_signal_mask(&prev_mask);
}

/**
 * @brief Apply input/output redirection described by a parsed command line.
 *
 * This helper is used in child processes before execve. It only redirects
 * stdin/stdout for the command being launched, and leaves the shell's own
 * descriptors untouched.
 *
 * @param[in] token Parsed command-line tokens describing any redirection.
 * @return True on success, false if opening or duping any file fails.
 */
static bool apply_redirection(const struct cmdline_tokens *token) {
    int fd;

    if (token->infile != NULL) {
        fd = open(token->infile, O_RDONLY);
        if (fd < 0) {
            perror(token->infile);
            return false;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("dup2 error");
            close(fd);
            return false;
        }
        close(fd);
    }

    if (token->outfile != NULL) {
        fd = open(token->outfile, O_WRONLY | O_CREAT | O_TRUNC, DEF_MODE);
        if (fd < 0) {
            perror(token->outfile);
            return false;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2 error");
            close(fd);
            return false;
        }
        close(fd);
    }

    return true;
}

/**
 * @brief Execute the jobs builtin using the helper-managed job list.
 *
 * If an output redirection target is present, the builtin writes the job list
 * there instead of to stdout. This builtin is the only required redirected
 * builtin in the lab.
 */
static void builtin_jobs(const struct cmdline_tokens *token) {
    sigset_t prev_mask;
    jid_t jid;
    pid_t pid;
    job_state state;
    const char *status;
    const char *cmdline;
    int output_fd = STDOUT_FILENO;
    bool should_close = false;

    if (token->outfile != NULL) {
        output_fd =
            open(token->outfile, O_WRONLY | O_CREAT | O_TRUNC, DEF_MODE);
        if (output_fd < 0) {
            perror(token->outfile);
            return;
        }
        should_close = true;
    }

    block_job_signals(&prev_mask);

    for (jid = 1; jid <= MAXJOBS; jid++) {
        if (!job_exists(jid)) {
            continue;
        }

        pid = job_get_pid(jid);
        state = job_get_state(jid);
        cmdline = job_get_cmdline(jid);

        switch (state) {
        case BG:
            status = "Running    ";
            break;
        case FG:
            status = "Foreground ";
            break;
        case ST:
            status = "Stopped    ";
            break;
        case UNDEF:
        default:
            continue;
        }

        dprintf(output_fd, "[%d] (%d) %s%s\n", jid, pid, status, cmdline);
    }

    restore_signal_mask(&prev_mask);

    if (should_close) {
        close(output_fd);
    }
}

/**
 * @brief Parse a bg/fg job argument as either a PID or %JID.
 *
 * Valid forms are a positive decimal PID such as `1234`, or a positive
 * decimal JID prefixed by `%` such as `%3`.
 *
 * @param[in] arg Argument string to parse.
 * @param[out] is_jid True when the parsed value is a JID.
 * @param[out] value Parsed positive integer value.
 * @return True if parsing succeeded, false otherwise.
 */
static bool parse_job_arg(const char *arg, bool *is_jid, int *value) {
    const char *number = arg;
    char *endptr;
    long parsed;

    if (arg == NULL || *arg == '\0') {
        return false;
    }

    *is_jid = false;
    if (*arg == '%') {
        *is_jid = true;
        number = arg + 1;
        if (*number == '\0') {
            return false;
        }
    }

    parsed = strtol(number, &endptr, 10);
    if (*endptr != '\0' || parsed <= 0 || parsed > INT_MAX) {
        return false;
    }

    *value = (int)parsed;
    return true;
}

/**
 * @brief Execute the bg or fg builtin on a target job.
 *
 * The builtin accepts either a PID or a %JID. Resolution, state transitions,
 * and process-group signaling all happen while job signals are blocked to keep
 * the job table consistent with concurrent SIGCHLD activity.
 *
 * @param[in] token Parsed command line tokens for the builtin.
 */
static void builtin_bgfg(const struct cmdline_tokens *token) {
    const char *cmd = token->argv[0];
    const char *arg = token->argv[1];
    bool is_jid;
    int value;
    sigset_t prev_mask;
    jid_t jid;
    pid_t pid;
    const char *cmdline;
    job_state state;

    if (arg == NULL) {
        printf("%s command requires PID or %%jobid argument\n", cmd);
        return;
    }

    if (!parse_job_arg(arg, &is_jid, &value)) {
        printf("%s: argument must be a PID or %%jobid\n", cmd);
        return;
    }

    block_job_signals(&prev_mask);

    if (is_jid) {
        jid = (jid_t)value;
        if (!job_exists(jid)) {
            restore_signal_mask(&prev_mask);
            printf("%s: No such job\n", arg);
            return;
        }
    } else {
        jid = job_from_pid((pid_t)value);
        if (jid == 0) {
            restore_signal_mask(&prev_mask);
            printf("(%d): No such process\n", value);
            return;
        }
    }

    pid = job_get_pid(jid);
    cmdline = job_get_cmdline(jid);
    state = (token->builtin == BUILTIN_BG) ? BG : FG;

    if (kill(-pid, SIGCONT) < 0 && errno == ESRCH) {
        restore_signal_mask(&prev_mask);
        return;
    }

    job_set_state(jid, state);

    if (state == BG) {
        printf("[%d] (%d) %s\n", jid, pid, cmdline);
    }

    restore_signal_mask(&prev_mask);

    if (state == FG) {
        waitfg(pid);
    }
}

/**
 * @brief Launch an external command as a foreground or background job.
 *
 * The parent installs the child in the helper-managed job list while signals
 * are blocked. Foreground waiting is signal-driven through waitfg, while
 * background jobs print their startup line and return immediately.
 *
 * @param[in] cmdline Original command line used to create the job.
 * @param[in] token Parsed command-line tokens for the program launch.
 * @param[in] state Initial job state, either FG or BG.
 */
static void run_external(const char *cmdline,
                         const struct cmdline_tokens *token, job_state state) {
    sigset_t prev_mask;
    pid_t pid;
    jid_t jid;

    block_job_signals(&prev_mask);

    pid = fork();
    if (pid < 0) {
        perror("fork error");
        restore_signal_mask(&prev_mask);
        return;
    }

    if (pid == 0) {
        if (setpgid(0, 0) < 0) {
            perror("setpgid error");
            _exit(1);
        }

        Signal(SIGINT, SIG_DFL);
        Signal(SIGTSTP, SIG_DFL);
        Signal(SIGCHLD, SIG_DFL);
        Signal(SIGQUIT, SIG_DFL);
        Signal(SIGTTIN, SIG_DFL);
        Signal(SIGTTOU, SIG_DFL);

        if (!apply_redirection(token)) {
            _exit(1);
        }

        restore_signal_mask(&prev_mask);
        execve(token->argv[0], token->argv, environ);
        perror(token->argv[0]);
        _exit(1);
    }

    if (setpgid(pid, pid) < 0) {
        if (errno != EACCES && errno != EPERM && errno != ESRCH) {
            perror("setpgid error");
            restore_signal_mask(&prev_mask);
            return;
        }
    }

    jid = add_job(pid, state, cmdline);
    if (jid == 0) {
        restore_signal_mask(&prev_mask);
        return;
    }

    if (state == BG) {
        printf("[%d] (%d) %s\n", jid, pid, cmdline);
    }

    restore_signal_mask(&prev_mask);

    if (state == FG) {
        waitfg(pid);
    }
}

/**
 * @brief Forward a signal to the current foreground process group.
 *
 * If no foreground job exists, the function returns without effect. The job
 * lookup and process-group PID fetch happen while job signals are blocked so
 * that the foreground selection is race-safe with concurrent reaping.
 *
 * @param[in] sig Signal number to forward.
 */
static void forward_signal_to_fg(int sig) {
    sigset_t prev_mask;
    jid_t jid;
    pid_t pid;

    block_job_signals(&prev_mask);

    jid = fg_job();
    if (jid != 0) {
        pid = job_get_pid(jid);
        kill(-pid, sig);
    }

    restore_signal_mask(&prev_mask);
}

/**
 * @brief Append a NUL-terminated literal string to a buffer.
 *
 * @param[out] buf Destination buffer.
 * @param[in] pos Current write position in the destination buffer.
 * @param[in] text NUL-terminated text to append.
 * @return Updated write position after the append completes.
 */
static size_t append_literal(char *buf, size_t pos, const char *text) {
    while (*text != '\0') {
        buf[pos++] = *text++;
    }
    return pos;
}

/**
 * @brief Append an unsigned decimal integer to a buffer.
 *
 * @param[out] buf Destination buffer.
 * @param[in] pos Current write position in the destination buffer.
 * @param[in] value Unsigned value to append in base 10.
 * @return Updated write position after the append completes.
 */
static size_t append_unsigned(char *buf, size_t pos, unsigned long value) {
    char digits[32];
    size_t ndigits = 0;

    do {
        digits[ndigits++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    } while (value != 0);

    while (ndigits > 0) {
        buf[pos++] = digits[--ndigits];
    }

    return pos;
}

/**
 * @brief Write a complete buffer to a file descriptor.
 *
 * @param[in] fd Destination file descriptor.
 * @param[in] buf Buffer to write.
 * @param[in] len Number of bytes to write.
 */
static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t nwritten = write(fd, buf, len);
        if (nwritten <= 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        buf += (size_t)nwritten;
        len -= (size_t)nwritten;
    }
}

/**
 * @brief Print a signal-caused job state change message.
 *
 * This helper is async-signal-safe and is intended for use from SIGCHLD.
 *
 * @param[in] jid Job ID to display.
 * @param[in] pid Root process ID of the job.
 * @param[in] sig Signal number associated with the state change.
 * @param[in] action Literal action text, such as "terminated by signal ".
 */
static void print_job_signal_message(jid_t jid, pid_t pid, int sig,
                                     const char *action) {
    char buf[128];
    size_t len = 0;

    len = append_literal(buf, len, "Job [");
    len = append_unsigned(buf, len, (unsigned long)jid);
    len = append_literal(buf, len, "] (");
    len = append_unsigned(buf, len, (unsigned long)pid);
    len = append_literal(buf, len, ") ");
    len = append_literal(buf, len, action);
    len = append_unsigned(buf, len, (unsigned long)sig);
    len = append_literal(buf, len, "\n");

    write_all(STDOUT_FILENO, buf, len);
}

/**
 * @brief Parse and execute one shell command line.
 *
 * This stage handles shell builtins, external command launch, helper-managed
 * job installation, the foreground/background split needed for stage 2, and
 * the bg/fg control builtins added in stage 4, along with stage 5 redirection
 * and final error handling.
 *
 * @param[in] cmdline Command line text without the trailing newline.
 */
void eval(const char *cmdline) {
    parseline_return parse_result;
    struct cmdline_tokens token;
    job_state state;

    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY) {
        return;
    }

    switch (token.builtin) {
    case BUILTIN_QUIT:
        exit(0);
    case BUILTIN_JOBS:
        builtin_jobs(&token);
        return;
    case BUILTIN_BG:
    case BUILTIN_FG:
        builtin_bgfg(&token);
        return;
    case BUILTIN_NONE:
        break;
    }

    state = (parse_result == PARSELINE_BG) ? BG : FG;
    run_external(cmdline, &token, state);
}

/*****************
 * Signal handlers
 *****************/

/**
 * @brief Reap children and update job state for exits and stops.
 *
 * Child lifecycle changes are centralized here so eval and waitfg do not race
 * against independent waitpid calls. Signal-caused termination and stop events
 * also print the required shell status messages from this handler.
 *
 * @param[in] sig Delivered signal number.
 */
void sigchld_handler(int sig) {
    int olderrno = errno;
    sigset_t prev_mask;
    pid_t pid;
    int status;
    jid_t jid;

    (void)sig;

    block_job_signals(&prev_mask);

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        jid = job_from_pid(pid);
        if (jid == 0) {
            continue;
        }

        if (WIFEXITED(status)) {
            delete_job(jid);
        } else if (WIFSIGNALED(status)) {
            print_job_signal_message(jid, pid, WTERMSIG(status),
                                     "terminated by signal ");
            delete_job(jid);
        } else if (WIFSTOPPED(status)) {
            job_set_state(jid, ST);
            print_job_signal_message(jid, pid, WSTOPSIG(status),
                                     "stopped by signal ");
        }
    }

    restore_signal_mask(&prev_mask);
    errno = olderrno;
}

/**
 * @brief Forward SIGINT to the current foreground process group, if any.
 *
 * @param[in] sig Delivered signal number.
 */
void sigint_handler(int sig) {
    int olderrno = errno;

    forward_signal_to_fg(sig);
    errno = olderrno;
}

/**
 * @brief Forward SIGTSTP to the current foreground process group, if any.
 *
 * @param[in] sig Delivered signal number.
 */
void sigtstp_handler(int sig) {
    int olderrno = errno;

    forward_signal_to_fg(sig);
    errno = olderrno;
}

/**
 * @brief Attempt to clean up global resources when the program exits.
 *
 * In particular, the job list must be freed at this time, since it may
 * contain leftover buffers from existing or even deleted jobs.
 */
void cleanup(void) {
    // Signals handlers need to be removed before destroying the joblist
    Signal(SIGINT, SIG_DFL);  // Handles Ctrl-C
    Signal(SIGTSTP, SIG_DFL); // Handles Ctrl-Z
    Signal(SIGCHLD, SIG_DFL); // Handles terminated or stopped child

    destroy_job_list();
}
