#ifndef VENUS_BT_QUEUE_H
#define VENUS_BT_QUEUE_H
#include <stddef.h>
#include <stdint.h>
#define VENUS_BT_QUEUE_SIZE 65536u
struct venus_bt_queue { uint8_t bytes[VENUS_BT_QUEUE_SIZE]; size_t head, used; };
/* Caller serializes access. Failed writes leave all queued bytes untouched. */
static inline int venus_bt_queue_push(struct venus_bt_queue *q, const uint8_t *data, size_t size) {
    if (size > VENUS_BT_QUEUE_SIZE - q->used) return 0;
    for (size_t i = 0; i < size; i++) q->bytes[(q->head + q->used + i) % VENUS_BT_QUEUE_SIZE] = data[i];
    q->used += size;
    return 1;
}
static inline size_t venus_bt_queue_pop(struct venus_bt_queue *q, uint8_t *data, size_t capacity) {
    size_t size = q->used < capacity ? q->used : capacity;
    for (size_t i = 0; i < size; i++) data[i] = q->bytes[(q->head + i) % VENUS_BT_QUEUE_SIZE];
    q->used -= size; q->head = (q->head + size) % VENUS_BT_QUEUE_SIZE;
    return size;
}
#endif
