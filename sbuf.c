#include "sbuf.h"

//Create an empty, bounded, shared FIFO buffer with n slots.
void sbuf_init(sbuf_t *sp, int n)
{
    sp->buf = (int *)Calloc(n, sizeof(int));
    sp->n = n;
    sp->front = sp->rear = 0;

    sp->bound = n; //when sp->front or sp->rear equal to sp->bound, reset their value by 0
    while (sp->bound * 2 <= INT_MAX/2)
    {
        sp->bound *= 2;
    };

#ifdef __APPLE__
    // macOS平台使用命名信号量
    Sem_init(&(sp->mutex), "/tinyweb_sbuf_mutex", 0, 1);
    Sem_init(&(sp->items), "/tinyweb_sbuf_items", 0, 0);
    Sem_init(&(sp->slots), "/tinyweb_sbuf_slots", 0, n);
#else
    // Linux及其他Unix-like平台使用未命名信号量
    Sem_init(&sp->mutex, 0, 1);
    Sem_init(&sp->items, 0, 0);
    Sem_init(&sp->slots, 0, n);
#endif
}

//Clean up buffer sp
void sbuf_deinit(sbuf_t *sp)
{
#ifdef __APPLE__
    // macOS平台关闭并删除命名信号量
    Sem_close(sp->mutex);
    Sem_close(sp->items);
    Sem_close(sp->slots);
    Sem_unlink("/tinyweb_sbuf_mutex");
    Sem_unlink("/tinyweb_sbuf_items");
    Sem_unlink("/tinyweb_sbuf_slots");
#endif
    Free(sp->buf);
}

//Insert item onto the rear of shared buffer sp
void sbuf_insert(sbuf_t *sp, int item)
{
    P(&sp->slots);
    P(&sp->mutex);
    sp->buf[(++(sp->rear))%(sp->n)] = item;
    if((sp->rear) == (sp->bound))  //Reset the counter rear regularly if it wouldn't break the rear position
        sp->rear = 0;
    V(&sp->mutex);
    V(&sp->items);
}

//Remove and return the first item from buffer sp
int sbuf_remove(sbuf_t *sp)
{
    int item;
    P(&sp->items);
    P(&sp->mutex);
    item = sp->buf[(++(sp->front))%(sp->n)];
    if((sp->front) == (sp->bound)) //Reset the counter front regularly if it wouldn't break the rear position
        sp->front = 0;
    V(&sp->mutex);
    V(&sp->slots);
    return item;
}