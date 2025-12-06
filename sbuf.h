#include "csapp.h"
#include "limits.h"

typedef struct
{
    int *buf;
    int n;
    int front;
    int rear;
    int bound;
#ifdef __APPLE__
    sem_t *mutex;
    sem_t *slots;
    sem_t *items;
#else
    sem_t mutex;
    sem_t slots;
    sem_t items;
#endif
} sbuf_t;

void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);