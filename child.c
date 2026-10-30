#define _POSIX_C_SOURCE 200809L

#include "shared.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int fd;
    shared_data_t *data;
    sem_t *empty;
    sem_t *ready;
} ipc_t;

static int write_all(int fd, const char *buffer, size_t length){
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

static void report_error(const char *message){
    size_t length = 0;
    while (message[length] != '\0') {
        length++;
    }
    (void)write_all(STDERR_FILENO, message, length);
}

static int open_ipc(ipc_t *ipc, const char *shm_name, const char *empty_name, const char *ready_name){
    ipc->fd = shm_open(shm_name, O_RDWR, 0);
    if (ipc->fd == -1) {
        perror("Cannot open shared memory");
        return -1;
    }

    struct stat info;
    if (fstat(ipc->fd, &info) == -1) {
        perror("Cannot get shared memory size");
        return -1;
    }
    if (info.st_size != (off_t)sizeof(shared_data_t)) {
        report_error("Unexpected shared memory size\n");
        return -1;
    }

    ipc->data = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, ipc->fd, 0);

    if (ipc->data == MAP_FAILED) {
        perror("Cannot map shared memory");
        return -1;
    }
    ipc->empty = sem_open(empty_name, 0);
    if (ipc->empty == SEM_FAILED) {
        perror("Cannot open empty semaphore");
        return -1;
    }
    ipc->ready = sem_open(ready_name, 0);
    if (ipc->ready == SEM_FAILED) {
        perror("Cannot open ready semaphore");
        return -1;
    }
    return 0;
}

static int close_ipc(ipc_t *ipc) {
    int failed = 0;
    if (ipc->ready != SEM_FAILED && sem_close(ipc->ready) == -1) {
        perror("Cannot close ready semaphore");
        failed = 1;
    }
    if (ipc->empty != SEM_FAILED && sem_close(ipc->empty) == -1) {
        perror("Cannot close empty semaphore");
        failed = 1;
    }
    if (ipc->data != MAP_FAILED &&
        munmap(ipc->data, sizeof(shared_data_t)) == -1) {
        perror("Cannot unmap shared memory");
        failed = 1;
    }
    if (ipc->fd != -1 && close(ipc->fd) == -1) {
        perror("Cannot close shared memory descriptor");
        failed = 1;
    }
    return failed ? -1 : 0;
}

static int send_message(ipc_t *ipc, message_type_t type, long long sum) {
    int result;
    do {
        result = sem_wait(ipc->empty);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        perror("Cannot wait for message slot");
        return -1;
    }
    ipc->data->type = type;
    ipc->data->sum = sum;
    if (sem_post(ipc->ready) == -1) {
        perror("Cannot notify parent");
        return -1;
    }
    return 0;
}

static int input_error(ipc_t *ipc, const char *message) {
    report_error(message);
    (void)send_message(ipc, MESSAGE_ERROR, 0);
    return -1;
}

static int process_input(ipc_t *ipc) {
    char buffer[4096];
    long long sum = 0;
    long long number = 0;
    int negative = 0;
    int in_number = 0;
    int has_digit = 0;
    int has_values = 0;

    while (1) {
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n == -1) {
            perror("Cannot read input file");
            (void)send_message(ipc, MESSAGE_ERROR, 0);
            return -1;
        }

        int end_of_file = n == 0;
        ssize_t count = end_of_file ? 1 : n;

        for (ssize_t i = 0; i < count; i++) {
            char c = end_of_file ? '\n' : buffer[i];

            if (c >= '0' && c <= '9') {
                if (!in_number) {
                    in_number = 1;
                    number = 0;
                    negative = 0;
                    has_digit = 0;
                }

                int digit = c - '0';
                long long limit = negative ? -(long long)INT_MIN : INT_MAX;
                if (number > (limit - digit) / 10) {
                    return input_error(ipc, "Input number is outside int range\n");
                }
                number = number * 10 + digit;
                has_digit = 1;
            } else if (c == '-' || c == '+') {
                if (in_number) {
                    return input_error(ipc, "Invalid sign in input number\n");
                }
                in_number = 1;
                number = 0;
                has_digit = 0;
                negative = c == '-';
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if (in_number) {
                    if (!has_digit) {
                        return input_error(ipc, "Input sign has no digits\n");
                    }
                    long long value = negative ? -number : number;
                    if ((value > 0 && sum > LLONG_MAX - value) ||
                        (value < 0 && sum < LLONG_MIN - value)) {
                        return input_error(ipc, "Sum is outside long long range\n");
                    }
                    sum += value;
                    has_values = 1;
                    in_number = 0;
                    has_digit = 0;
                }
                if (c == '\n' && has_values) {
                    if (send_message(ipc, MESSAGE_RESULT, sum) == -1) {
                        return -1;
                    }
                    sum = 0;
                    has_values = 0;
                }
            } else {
                return input_error(ipc, "Invalid character in input file\n");
            }
        }
        if (end_of_file) {
            return 0;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        report_error("Usage: ./child <shared_memory> <empty_semaphore> <ready_semaphore>\n");
        return 1;
    }

    ipc_t ipc = {
        .fd = -1,
        .data = MAP_FAILED,
        .empty = SEM_FAILED,
        .ready = SEM_FAILED
    };
    if (open_ipc(&ipc, argv[1], argv[2], argv[3]) == -1) {
        (void)close_ipc(&ipc);
        return 1;
    }
    int failed = process_input(&ipc) == -1;
    if (!failed && send_message(&ipc, MESSAGE_DONE, 0) == -1) {
        failed = 1;
    }
    if (close_ipc(&ipc) == -1) {
        failed = 1;
    }
    return failed ? 1 : 0;
}
