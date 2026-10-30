#ifndef OS_LAB3_SHARED_H
#define OS_LAB3_SHARED_H

typedef enum {
    MESSAGE_RESULT = 1,
    MESSAGE_DONE,
    MESSAGE_ERROR
} message_type_t;

typedef struct {
    message_type_t type;
    long long sum;
} shared_data_t;

#endif
