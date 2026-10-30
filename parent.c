#define _POSIX_C_SOURCE 200809L

#include "shared.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    int fd;
    shared_data_t *data;
    sem_t *empty;
    sem_t *ready;
    char shm_name[128];
    char empty_name[128];
    char ready_name[128];
} ipc_t;

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(fd, buffer + sent, length - sent);
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void report_error(const char *message)
{
    size_t length = 0;
    while (message[length] != '\0') {
        length++;
    }
    (void)write_all(STDERR_FILENO, message, length);
}

static int print_number(long long value)
{
    char reversed[3 * sizeof(long long) + 3];
    size_t length = 0;
    long long current = value;
    do {
        int digit = (int)(current % 10);
        if (digit < 0) {
            digit = -digit;
        }
        reversed[length++] = (char)('0' + digit);
        current /= 10;
    } while (current != 0);
    if (value < 0) {
        reversed[length++] = '-';
    }

    char output[sizeof(reversed)];
    for (size_t i = 0; i < length; i++) {
        output[i] = reversed[length - 1 - i];
    }
    output[length] = '\n';
    return write_all(STDOUT_FILENO, output, length + 1);
}

static int create_ipc(ipc_t *ipc)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
        perror("Cannot get time for IPC names");
        return -1;
    }

    char prefix[96];
    int length = snprintf(prefix, sizeof(prefix), "/os_lab3_%ld_%lld_%ld",
                          (long)getpid(), (long long)now.tv_sec, now.tv_nsec);
    if (length < 0 || (size_t)length >= sizeof(prefix)) {
        report_error("Cannot generate IPC names\n");
        return -1;
    }
    int shm_length = snprintf(ipc->shm_name, sizeof(ipc->shm_name),
                              "%s_shm", prefix);
    int empty_length = snprintf(ipc->empty_name, sizeof(ipc->empty_name),
                                "%s_empty", prefix);
    int ready_length = snprintf(ipc->ready_name, sizeof(ipc->ready_name),
                                "%s_ready", prefix);
    if (shm_length < 0 || (size_t)shm_length >= sizeof(ipc->shm_name) ||
        empty_length < 0 || (size_t)empty_length >= sizeof(ipc->empty_name) ||
        ready_length < 0 || (size_t)ready_length >= sizeof(ipc->ready_name)) {
        report_error("Cannot generate IPC names\n");
        return -1;
    }

    ipc->fd = shm_open(ipc->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (ipc->fd == -1) {
        perror("Cannot create shared memory");
        return -1;
    }
    if (ftruncate(ipc->fd, (off_t)sizeof(shared_data_t)) == -1) {
        perror("Cannot set shared memory size");
        return -1;
    }
    ipc->data = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, ipc->fd, 0);
    if (ipc->data == MAP_FAILED) {
        perror("Cannot map shared memory");
        return -1;
    }

    ipc->empty = sem_open(ipc->empty_name, O_CREAT | O_EXCL, 0600, 1);
    if (ipc->empty == SEM_FAILED) {
        perror("Cannot create empty semaphore");
        return -1;
    }
    ipc->ready = sem_open(ipc->ready_name, O_CREAT | O_EXCL, 0600, 0);
    if (ipc->ready == SEM_FAILED) {
        perror("Cannot create ready semaphore");
        return -1;
    }
    return 0;
}

static int destroy_ipc(ipc_t *ipc)
{
    int failed = 0;
    if (ipc->ready != SEM_FAILED) {
        if (sem_unlink(ipc->ready_name) == -1) {
            perror("Cannot unlink ready semaphore");
            failed = 1;
        }
        if (sem_close(ipc->ready) == -1) {
            perror("Cannot close ready semaphore");
            failed = 1;
        }
    }
    if (ipc->empty != SEM_FAILED) {
        if (sem_unlink(ipc->empty_name) == -1) {
            perror("Cannot unlink empty semaphore");
            failed = 1;
        }
        if (sem_close(ipc->empty) == -1) {
            perror("Cannot close empty semaphore");
            failed = 1;
        }
    }
    if (ipc->data != MAP_FAILED &&
        munmap(ipc->data, sizeof(shared_data_t)) == -1) {
        perror("Cannot unmap shared memory");
        failed = 1;
    }
    if (ipc->fd != -1) {
        if (shm_unlink(ipc->shm_name) == -1) {
            perror("Cannot unlink shared memory");
            failed = 1;
        }
        if (close(ipc->fd) == -1) {
            perror("Cannot close shared memory descriptor");
            failed = 1;
        }
    }
    return failed ? -1 : 0;
}

static int wait_for_message(sem_t *ready, pid_t pid, int *status, int *reaped)
{
    while (1) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) == -1) {
            return -1;
        }
        deadline.tv_sec++;

        int result;
        do {
            result = sem_timedwait(ready, &deadline);
        } while (result == -1 && errno == EINTR);
        if (result == 0) {
            return 0;
        }
        if (errno != ETIMEDOUT) {
            return -1;
        }

        if (!*reaped) {
            pid_t waited;
            do {
                waited = waitpid(pid, status, WNOHANG);
            } while (waited == -1 && errno == EINTR);
            if (waited == -1) {
                return -1;
            }
            *reaped = waited == pid;
        }
        if (*reaped) {
            do {
                result = sem_trywait(ready);
            } while (result == -1 && errno == EINTR);
            if (result == -1 && errno == EAGAIN) {
                errno = EPIPE;
            }
            return result;
        }
    }
}

int main(void)
{
    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGPIPE, &action, NULL) == -1) {
        perror("Cannot configure SIGPIPE handling");
        return 1;
    }

    const char prompt[] = "Enter filename: ";
    if (write_all(STDERR_FILENO, prompt, sizeof(prompt) - 1) == -1) {
        return 1;
    }
    char filename[256];
    if (scanf("%255s", filename) != 1) {
        report_error("Cannot read filename\n");
        return 1;
    }
    int filefd = open(filename, O_RDONLY);
    if (filefd == -1) {
        perror("Cannot open input file");
        return 1;
    }

    ipc_t ipc = {
        .fd = -1,
        .data = MAP_FAILED,
        .empty = SEM_FAILED,
        .ready = SEM_FAILED
    };
    if (create_ipc(&ipc) == -1) {
        (void)destroy_ipc(&ipc);
        if (close(filefd) == -1) {
            perror("Cannot close input file");
        }
        return 1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("Cannot fork");
        (void)destroy_ipc(&ipc);
        if (close(filefd) == -1) {
            perror("Cannot close input file");
        }
        return 1;
    }
    if (pid == 0) {
        if (dup2(filefd, STDIN_FILENO) == -1) {
            perror("Cannot redirect child stdin");
            _exit(1);
        }
        if (filefd != STDIN_FILENO && close(filefd) == -1) {
            perror("Cannot close child input file");
            _exit(1);
        }
        char *const args[] = {
            "./child", ipc.shm_name, ipc.empty_name, ipc.ready_name, NULL
        };
        execve("./child", args, environ);
        perror("Cannot execute child");
        _exit(127);
    }

    int failed = 0;
    int reaped = 0;
    int status = 0;
    if (close(filefd) == -1) {
        perror("Cannot close input file");
        failed = 1;
    }

    while (!failed) {
        if (wait_for_message(ipc.ready, pid, &status, &reaped) == -1) {
            perror("Cannot receive child message");
            failed = 1;
            break;
        }

        message_type_t type = ipc.data->type;
        long long sum = ipc.data->sum;
        if (sem_post(ipc.empty) == -1) {
            perror("Cannot release message slot");
            failed = 1;
            break;
        }
        if (type == MESSAGE_DONE) {
            break;
        }
        if (type == MESSAGE_ERROR) {
            report_error("Child reported an error\n");
            failed = 1;
            break;
        }
        if (type != MESSAGE_RESULT) {
            report_error("Unknown child message type\n");
            failed = 1;
            break;
        }
        if (print_number(sum) == -1) {
            perror("Cannot write result");
            failed = 1;
        }
    }

    if (failed && !reaped && kill(pid, SIGTERM) == -1 && errno != ESRCH) {
        perror("Cannot stop child");
    }
    if (!reaped) {
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited == -1) {
            perror("Cannot wait for child");
            failed = 1;
        } else {
            reaped = 1;
        }
    }
    if (reaped && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        report_error("Child failed\n");
        failed = 1;
    }
    if (destroy_ipc(&ipc) == -1) {
        failed = 1;
    }
    return failed ? 1 : 0;
}
