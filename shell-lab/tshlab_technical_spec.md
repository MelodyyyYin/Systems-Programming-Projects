# TSH Lab Technical Implementation Document

## Source and intent
This document converts the Spring 2026 Shell Lab PDF into an implementation-facing technical spec for an execution agent. It is designed to stay faithful to the handout while turning the assignment into a concrete build and validation plan.

Primary source: `tshlab.pdf`.

---

## 1. Assignment summary

Implement a small Linux shell named `tsh` (tiny shell) in C. The shell must support:

- command parsing and execution
- foreground and background jobs
- job control using PID/JID
- built-in commands: `quit`, `jobs`, `bg`, `fg`
- signal handling for `SIGCHLD`, `SIGINT`, `SIGTSTP`
- I/O redirection for external commands, plus output redirection for `jobs`
- correct output behavior matching the reference shell `tshref`

The implementation target is the provided shell skeleton in `tsh.c`, using the supplied helper code in `tsh_helper.{c,h}` for command parsing and job-list manipulation.

The handout explicitly identifies four functions as the required unfinished work:

- `eval`
- `sigchld_handler`
- `sigint_handler`
- `sigtstp_handler`

This is an **individual** project and must be developed and tested on a **class shark machine**.

---

## 2. Required deliverables

### Final code deliverable
The final submission must include a working `tsh` implementation that passes the official driver-based tests and is suitable for Autolab hand-in.

### Practical deliverable checklist
An implementation is considered complete only if all of the following are true:

1. `make` succeeds on a shark machine.
2. Running `./tsh` launches the shell and accepts commands.
3. `./sdriver` passes all active traces.
4. Shell output matches `tshref` except for the allowed differences:
   - PIDs vary naturally across runs.
   - `/bin/ps` output differs in traces 26 and 27, but the running states of `mysplit` processes must still match.
5. The implementation does not rely on race-prone hacks such as `sleep` loops.
6. The solution is submission-ready through either:
   - `make` producing `tshlab-handin.tar`, or
   - `make submit` on shark.

### Grading-relevant deliverable expectations
The handout emphasizes two kinds of deliverables beyond merely “passing traces”:

- **Correctness deliverable**: behaviorally correct shell under the trace driver and race conditions.
- **Style/engineering deliverable**: readable, modular, well-commented code with explicit error handling.

---

## 3. Scope and non-scope

### In scope
The shell must implement:

- interactive prompt/command loop
- external program execution via child processes
- foreground/background execution
- job tracking
- PID and JID addressing
- signal forwarding to the foreground process group
- correct child reaping
- stop/continue behavior for jobs
- `bg` / `fg` / `jobs` / `quit`
- input redirection `<`
- output redirection `>`
- combined input and output redirection on the same command line
- output redirection for `jobs`
- required error handling behavior

### Explicitly out of scope
Do **not** implement or rely on:

- pipes
- terminal control APIs such as `tcsetpgrp` or `tcsetattr`
- hacks that only satisfy the traces without being actually correct
- busy waiting (`while(1)`, polling with `sleep`, etc.)

---

## 4. Files, interfaces, and starting point

### Provided implementation support
The handout says the student is given:

- `tsh.c`: skeleton shell
- `tsh_helper.{c,h}`: helper code for
  - command-line parsing
  - job-list operations

### What the agent should treat as authoritative
The following must be read before implementation decisions are finalized:

1. `tsh.c`
2. `tsh_helper.h`
3. `tsh_helper.c`
4. trace files `trace00.txt` through `trace32.txt` (with trace10 noted as removed/buggy)
5. `wrapper.c` / other driver-side wrappers if debugging trace synchronization issues

The helper API defines the shell’s intended job abstraction. The implementation should adapt to that API instead of inventing a conflicting job-management layer.

---

## 5. Functional requirements

## 5.1 Command loop and shell entry behavior

The shell must:

- repeatedly print a prompt
- read a command line from `stdin`
- parse and interpret the command
- either execute a built-in command directly or launch an external program in a child process

### EOF behavior
Trace 00 validates termination on EOF. The shell must exit cleanly if input ends.

---

## 5.2 Built-in commands

The shell must support the following built-ins:

### `quit`
- Terminates the shell.
- Implement early because trace 01 depends on it.

### `jobs`
- Lists all background jobs.
- Must support output redirection (`jobs > file`).
- The reference shell redirects all built-ins, but this lab only requires redirection support for `jobs`.

### `bg <job>`
- Resume a stopped job by sending `SIGCONT`.
- Then mark/run it in the background.
- `<job>` may be either:
  - a PID, or
  - a JID with `%` prefix

### `fg <job>`
- Resume a stopped/background job by sending `SIGCONT`.
- Then move it to the foreground.
- The shell must wait until it is no longer the foreground job.
- `<job>` may be either PID or `%JID`.

### Built-in argument handling
The shell must correctly interpret whether an operand represents:

- PID: plain decimal integer, e.g. `1234`
- JID: prefixed form, e.g. `%3`

The implementation must reject invalid/missing job arguments with the exact behavior expected by `tshref`.

---

## 5.3 External command execution

For non-built-in commands, the shell must:

1. parse the command line into executable + argv + redirection metadata + foreground/background mode
2. fork a child
3. in the child:
   - restore signal mask/state as required
   - set process group correctly
   - apply I/O redirection if requested
   - exec the target program
4. in the parent:
   - add the new job to the job list in the appropriate state
   - for foreground jobs: wait until it is no longer the foreground job
   - for background jobs: print required launch message and return prompt immediately

### Environment handling
The shell must correctly pass along arguments and environment state to child processes. Trace 02 depends on this.

---

## 5.4 Foreground vs background jobs

### Foreground rule
A command **without** trailing `&` runs in the foreground.

### Background rule
A command **with** trailing `&` runs in the background.

### Background launch output
When a background job starts, the shell must print a line of the form:

```text
[JID] (PID) command line
```

Example from the handout:

```text
[1] (32757) /bin/ls &
```

The prompt must reappear immediately after launching a background job.

---

## 5.5 Job identity model

Each job is addressable by:

- **PID**: system process ID
- **JID**: shell-assigned positive integer

Command-line semantics:

- `%5` means JID 5
- `5` means PID 5

The shell must keep its internal job list synchronized with process lifecycle changes.

---

## 5.6 Job states

The implementation should model at least these logical states:

- **Foreground (FG)**
- **Background (BG)**
- **Stopped (ST)**

Even if the helper API names states differently, the effective behavior must preserve these three modes.

Required state transitions include:

- new foreground job -> FG
- new background job -> BG
- foreground/background stopped by signal -> ST
- stopped job resumed with `bg` -> BG
- stopped/background job resumed with `fg` -> FG
- terminated job -> removed from job list

---

## 5.7 Reaping child processes

The shell must reap zombie children **within a bounded amount of time**.

### Critical rule
Do **not** defer reaping until the next user command or until a foreground wait path happens to execute. Reaping must happen promptly when children change state.

### Required design direction
The handout strongly indicates that `sigchld_handler` is the correct place to reap children.

### Important consequences
- multiple child status changes may coalesce into one `SIGCHLD`
- therefore the handler must reap **all** waitable children, not just one
- stopped children must also be observed
- the shell’s job list must be updated to reflect exit, signal termination, and stop events

### Wait strategy constraint
The handout explicitly warns against calling `waitpid` in multiple places. Centralize child reaping logic inside the `SIGCHLD` path.

### Expected wait semantics
The implementation must be consistent with a design that uses `waitpid` in a loop with options that allow:

- nonblocking reaping
- observing stopped children

The handout specifically points to `WNOHANG` and `WUNTRACED` as important.

---

## 5.8 Waiting for a foreground job

Because child reaping should be centralized in `sigchld_handler`, the foreground wait path in `eval` should **not** directly compete with the handler for `waitpid`.

### Required strategy
The handout explicitly points to:

- `sigsuspend`
- job-interface helpers

Therefore the foreground wait mechanism should be:

1. block relevant signals while setting up foreground state
2. once the job is safely installed as FG, repeatedly suspend until a signal arrives
3. after each wake-up, check whether that job is still the foreground job
4. exit wait loop when the job has terminated or moved out of foreground state

### Forbidden strategy
Do not busy-wait and do not poll with `sleep`.

---

## 5.9 Signal forwarding behavior

The shell must handle:

- `SIGINT` (Ctrl-C)
- `SIGTSTP` (Ctrl-Z)

### If no foreground job exists
These signals must have **no effect**.

### If a foreground job exists
The shell must forward the signal to the **entire foreground process group**, not just one process.

This behavior is essential for traces 15–18.

---

## 5.10 Process groups

### Problem to solve
If the shell forks a child and leaves it in the shell’s own foreground process group, terminal-generated signals such as Ctrl-C would hit both the shell and its child. That is incorrect.

### Required design
After `fork`, but before `execve`, the child should be placed into a new process group whose PGID equals the child PID.

### Implication
Signal forwarding from the shell should target the job’s process group so that all processes in the foreground job receive the signal.

### Constraint
Do **not** use terminal-group management calls such as `tcsetpgrp`; the handout explicitly says they are out of scope and will break autograding.

---

## 5.11 Signal handler semantics

### `sigchld_handler`
Responsibilities:

- reap all children that have changed state
- detect normal exit, signal termination, and stop events
- update/remove job-list entries accordingly
- print required messages when jobs terminate or stop due to uncaught signals
- preserve and restore `errno`
- use only async-signal-safe operations

### `sigint_handler`
Responsibilities:

- identify current foreground job, if any
- forward `SIGINT` to its entire process group
- avoid affecting the shell itself
- preserve/restore `errno`

### `sigtstp_handler`
Responsibilities:

- identify current foreground job, if any
- forward `SIGTSTP` to its entire process group
- preserve/restore `errno`

### Child-side signal semantics after exec
The handout notes that `execve` resets signal handlers to default actions in the child. The implementation must also ensure the child does not inherit an inappropriate blocked-signal state from the parent.

---

## 5.12 Signal safety and race conditions

This is one of the most important parts of the assignment.

### Race condition rule
Signal handlers can interrupt the shell at almost any time. Therefore, every access or mutation of the shared job list must be done under a signal-blocking discipline.

### Minimum blocking requirement called out by the handout
The handout specifically says to protect against:

- `SIGCHLD`
- `SIGINT`
- `SIGTSTP`

when accessing the job list.

### Consequences for implementation
The agent should treat the following as critical sections:

- adding a new job after `fork`
- deleting a job
- changing job state
- reading foreground job info used for signal forwarding
- resolving PID/JID operands for `bg` / `fg`

### Design principle
There should be no window where:

- a child exits before being inserted into the job list,
- a handler reaps a child while the parent is still updating that same job entry,
- `fg` / `bg` act on a job entry while a handler concurrently deletes or changes it.

---

## 5.13 Async-signal-safe output

Inside signal handlers, the implementation must **not** use non-async-signal-safe functions such as `printf`.

The handout explicitly recommends using `sio_printf` from the provided CS:APP support library.

This applies particularly to job-status messages printed from `sigchld_handler`.

---

## 5.14 Required shell output on job stop/termination

If a job terminates or stops because of a signal it did not catch, the shell must print a message including:

- JID
- PID
- signal number

Examples from the handout:

```text
Job [1] (1778) terminated by signal 2
Job [2] (1836) stopped by signal 20
```

Output formatting must match `tshref` exactly aside from PID variability.

---

## 5.15 I/O redirection requirements

The shell must support:

- input redirection with `<`
- output redirection with `>`
- simultaneous input and output redirection on the same command line
- either ordering of input/output redirection tokens, as traces 30 and 31 cover ordering/permissions cases

### Redirection scope
For external commands, redirection should be applied in the child process after `fork` and before `execve`.

### Built-in redirection requirement
The shell is required to support output redirection for `jobs`. It is **not** required to support general built-in redirection beyond that.

### Safety requirement
Do not modify the shell process’s own persistent standard descriptors in a way that breaks the interactive shell after command completion.

### Resource management
Do not leak file descriptors.

---

## 5.16 Error handling requirements

The shell must distinguish among at least three classes of error response:

- **silent ignore**
- **notify the user**
- **fail/abort/exit**

The correct choice depends on context.

### Practical error-handling rules
- The shell should **not** die on routine user errors such as invalid filenames.
- The shell may exit on unrecoverable internal failures like memory-allocation failure.
- Error messages should use `perror` or `strerror` when appropriate.
- System call return values must generally be checked.

### Explicit exemptions from required error checking
The handout says you may assume success for:

- `getpgid`
- `getpid`
- `getppid`
- `sigaddset`
- `sigdelset`
- `sigemptyset`
- `sigfillset`
- `sigismember`
- `sigprocmask`
- `setpgid`
- `sigsuspend`

Even so, the implementation should remain logically correct around them.

---

## 6. Recommended internal design

This section is an implementation blueprint inferred from the handout. It is not prescribed source code, but it is the intended execution plan for an agent.

## 6.1 Core control flow in `eval`

Recommended `eval` pipeline:

1. Parse the command line via helper/parser API.
2. If parsing indicates empty line, return.
3. If command is a built-in:
   - `quit`: exit
   - `jobs`: optionally perform output redirection for the builtin, then print job list
   - `bg` / `fg`: dispatch to a helper that resolves PID/JID and updates job state
   - return
4. Otherwise treat as external command:
   - build signal mask for job-list critical section
   - block relevant signals before `fork`
   - `fork`
   - child path:
     - restore mask/unblock signals
     - establish child process group with `setpgid(0, 0)`-style behavior
     - restore default signal behavior as needed
     - apply redirection
     - `execve`
     - if exec fails, print appropriate error and exit child
   - parent path:
     - add job to job list while signals remain blocked
     - if BG: print `[jid] (pid) cmdline`
     - unblock signals
     - if FG: wait with a `sigsuspend`-based loop until no foreground job remains

---

## 6.2 Suggested helper decomposition

To satisfy the style requirement, do not leave all logic inside `eval`.

Suggested helper functions:

- `bool is_builtin(...)`
- `void run_builtin(...)`
- `void run_external(...)`
- `void waitfg(pid_t pid)` or equivalent foreground wait helper
- `bool parse_job_arg(const char *arg, bool *is_jid, int *value)`
- `job_t *resolve_job_from_arg(...)`
- `void builtin_bgfg(...)`
- `bool apply_redirection(...)`
- `void print_job_state_change(...)`
- `void block_job_signals(sigset_t *prev)`
- `void restore_signals(const sigset_t *prev)`

Exact function names can differ, but the code should be factored along these responsibilities.

---

## 6.3 Job-list ownership model

The implementation should adopt this invariant:

- all job-list mutations happen with job-related signals blocked
- all child lifecycle transitions enter through `sigchld_handler`
- `eval` installs new jobs; `sigchld_handler` tears them down or changes state when the OS reports status changes

This keeps ownership clean and avoids duplicate wait logic.

---

## 6.4 Foreground waiting contract

The foreground wait helper should not “wait for a pid” using `waitpid` directly. Instead it should:

- repeatedly ask the job layer whether a foreground job still exists (or whether the given pid is still FG)
- call `sigsuspend` while waiting
- tolerate spurious wakeups or unrelated signals

This is the handout-aligned way to avoid race-prone dual waiting logic.

---

## 6.5 Child launch contract

Before `execve`, the child should satisfy all of these conditions:

- no inherited blocked-signal state that would break target program behavior
- not in the shell’s process group
- redirections applied if specified
- error paths produce the right user-visible behavior and then exit child cleanly

---

## 7. Acceptance criteria by feature

## 7.1 Minimum baseline
The implementation is not considered viable until all of the following work:

- EOF exits shell
- `quit` works
- a simple foreground command like `/bin/ls` runs
- command arguments are preserved
- environment-dependent traces pass

## 7.2 Job control baseline
Then all of the following must work:

- background commands launch with `&`
- prompt returns immediately for background jobs
- `jobs` lists background jobs correctly
- zombies are reaped
- multiple child exits can be handled under one `SIGCHLD`

## 7.3 Signal baseline
Then all of the following must work:

- foreground `SIGINT` forwarding
- foreground `SIGTSTP` forwarding
- signal-forwarding to entire process group
- correct stop/termination messages
- race-safe behavior around very fast exits and handler interleavings

## 7.4 Builtin control baseline
Then all of the following must work:

- `bg` resumes stopped jobs in background
- `fg` resumes jobs in foreground and blocks shell until completion/state change
- PID and JID forms both work
- invalid job arguments are handled like `tshref`

## 7.5 Redirection baseline
Finally all of the following must work:

- input redirection
- output redirection
- combined redirection
- different ordering of redirection tokens
- `jobs > file`
- error handling on invalid files/permissions

---

## 8. Trace-aligned implementation roadmap

This roadmap is directly aligned with the handout’s incremental guidance.

## Phase 0: bootstrapping
Target traces:

- trace00
- trace01

Goals:

- shell startup and prompt loop
- EOF exit
- `quit` builtin

## Phase 1: foreground execution
Target traces:

- trace02
- trace03
- trace04

Goals:

- fork + exec external commands
- pass argv/env correctly
- initial foreground synchronization

## Phase 2: background execution and basic jobs
Target traces:

- trace05
- trace06
- trace07
- trace08

Goals:

- recognize `&`
- print background job line
- job-list insertion/listing
- centralized `SIGCHLD`-based reaping
- reap multiple children in a single handler execution

## Phase 3: signal handling
Target traces:

- trace09
- trace11
- trace12
- trace13
- trace14
- trace15
- trace16
- trace17
- trace18
- trace19
- trace20
- trace21

Notes:

- trace10 is explicitly noted in the handout as buggy/removed.

Goals:

- `SIGINT` / `SIGTSTP` handlers
- process-group-aware signal forwarding
- stop/terminate message printing
- race-safe signal handling

## Phase 4: `bg` / `fg`
Target traces:

- trace22
- trace23
- trace24
- trace25
- trace26
- trace27

Goals:

- PID/JID parsing
- resume with `SIGCONT`
- correct foreground/background state transitions
- `fg` waits correctly
- robust behavior under race conditions involving built-ins

## Phase 5: redirection and final error handling
Target traces:

- trace28
- trace29
- trace30
- trace31
- trace32

Goals:

- child-side descriptor redirection
- built-in `jobs > file`
- permissions/error cases
- final polish of user-visible error behavior

---

## 9. Full trace coverage map

| Trace range | Theme | Required capability |
|---|---|---|
| 00-01 | shell lifecycle | EOF exit, `quit` |
| 02-04 | foreground exec | fork/exec, argv/env correctness |
| 05-08 | background + reaping | `&`, job list, jobs builtin, multi-child `SIGCHLD` handling |
| 09-21 | signals | self-signals, terminal signals, forwarding, process groups, race cases |
| 22-27 | `bg` / `fg` | PID/JID parsing, state transitions, `SIGCONT`, wait correctness |
| 28-32 | redirection + errors | `<`, `>`, combined redirection, ordering, permissions, error handling |

---

## 10. Testing and validation procedure

## 10.1 Manual smoke tests first
Before using the driver, manually verify at least:

```bash
./tsh
quit
/bin/ls
/bin/echo hello
/usr/bin/sleep 5 &
jobs
```

Also manually test:

```bash
/bin/cat < input.txt
/bin/echo hi > out.txt
/bin/cat < input.txt > out.txt
jobs > jobs.txt
```

## 10.2 Compare behavior to `tshref`
For any uncertainty in semantics or output formatting, run the reference shell and mirror it exactly.

## 10.3 Use `runtrace` for focused debugging
Recommended usage:

```bash
./runtrace -f traces/trace05.txt -s ./tsh
./runtrace -f traces/trace12.txt -s ./tsh
./runtrace -f traces/trace25.txt -s ./tsh
```

## 10.4 Use `sdriver` for official regression coverage
Recommended usage:

```bash
./sdriver
./sdriver 6
./sdriver -i 1
```

Interpret failures using the diff against `tshref`.

## 10.5 Debugging principle
Do not assume “passes once” means correct. The driver intentionally reruns traces multiple times to expose races.

---

## 11. Style and engineering requirements

The code should be implementation-ready for human review, not just trace-passing.

### Required style properties
- break large functions into smaller helpers
- avoid duplicated logic
- write high-level block comments
- keep lines within 80 characters
- use consistent indentation
- use descriptive names
- group logic with whitespace
- check return values and handle errors appropriately

### Required file comments
The handout expects:

- file-level descriptive block comment
- routine-level high-level block comments
- block comments before logically related code sections

### Anti-patterns likely to lose points
- huge monolithic `eval`
- duplicated foreground/background launch logic
- race conditions masked with sleeps
- handler code that uses unsafe functions
- missing error checks

---

## 12. Common failure modes to guard against

1. **Fork/add-job race**  
   Child exits before parent adds it to the job list.

2. **Double waiting**  
   Calling `waitpid` both in `eval` and in `sigchld_handler`.

3. **Shell killed by Ctrl-C**  
   Forgetting to isolate child into a separate process group.

4. **Only first child reaped**  
   Handler processes one wait event instead of looping.

5. **Busy wait on FG job**  
   Uses polling or sleeps instead of `sigsuspend`.

6. **Unsafe printing in handlers**  
   Uses `printf` in signal context.

7. **Broken shell stdio after redirection**  
   Accidentally redirects the shell’s own descriptors instead of only the child / temporary builtin path.

8. **Incorrect `bg`/`fg` operand parsing**  
   Confuses PID and `%JID`, or mishandles invalid input.

9. **Missing stopped-child handling**  
   Shell only notices exits, not stops.

10. **Incorrect signal target**  
    Signal sent to one pid instead of the full process group.

---

## 13. Suggested implementation order for an execution agent

1. Read `tsh_helper.h` carefully and enumerate available job-list/query/update APIs.
2. Implement `quit`.
3. Implement minimal foreground exec.
4. Add parent/child signal-mask discipline around `fork`.
5. Add job-list insertion and foreground waiting via `sigsuspend`.
6. Implement `sigchld_handler` loop and job-state updates.
7. Add background jobs and `jobs` listing.
8. Add `sigint_handler` and `sigtstp_handler` signal forwarding.
9. Add process-group creation in child.
10. Add `bg` / `fg` helpers with PID/JID parsing.
11. Add redirection for child exec path.
12. Add `jobs > file` support.
13. Tighten error handling and output formatting against `tshref`.
14. Run full `sdriver` repeatedly until stable.

---

## 14. Submission protocol

### Due information from the handout
- Assigned: Tue, Mar 24
- Due: Tue, Apr 7, 11:59 PM
- Last possible handin: Fri, Apr 10, 11:59 PM

### Submission methods
- `make` -> upload `tshlab-handin.tar` to Autolab
- `make submit` on shark

### Final submission rule
Autolab grades the **last** submission before deadline.

### Required final check
After submission, verify that the expected score actually appears in Autolab and inspect autograder output for issues.

---

## 15. Notes on handout ambiguities

The evaluation section says the score is computed out of a maximum of **103** points, but the visible breakdown in the same section is **96 correctness + 4 style**, which sums to **100**. Treat the breakdown and the trace/style requirements as the actionable rubric unless course staff clarify otherwise.

---

## 16. Definition of done

The implementation is done when:

- all active traces pass under `sdriver`
- repeated runs remain stable
- there are no known race-condition windows in job creation/reaping/signal forwarding
- output matches `tshref`
- all required features from the handout are present
- code is modular and readable enough to pass style review
- submission artifact can be generated and uploaded successfully

---

## 17. Agent execution summary

If another agent is implementing this lab, it should follow this exact order of priority:

1. preserve correctness under signals and concurrency
2. centralize child reaping in `sigchld_handler`
3. use process groups correctly
4. make `fg` waiting signal-driven, not polling
5. match `tshref` output byte-for-byte where applicable
6. only after correctness is stable, optimize structure/style/error handling

This lab is fundamentally a **process control + signals + race avoidance** assignment. The hardest bugs are not syntax bugs; they are ordering bugs.
