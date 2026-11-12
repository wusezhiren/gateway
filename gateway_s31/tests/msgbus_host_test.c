#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef enum { MSG_NONE = 0, MSG_KEY, MSG_WAKE, MSG_MAX } msg_id_t;
typedef struct { msg_id_t id; int value; } message_t;
typedef void (*callback_t)(const message_t *, void *);

#define MAX_SUBS 3
static struct { callback_t cb; void *ctx; } subs[MSG_MAX][MAX_SUBS];
static bool initialized;

static int init_bus(void) { initialized = true; memset(subs, 0, sizeof subs); return 0; }
static int subscribe(msg_id_t id, callback_t cb, void *ctx)
{
    if (!initialized || id <= MSG_NONE || id >= MSG_MAX || !cb) return -1;
    for (int i = 0; i < MAX_SUBS; ++i) if (!subs[id][i].cb) { subs[id][i] = (typeof(subs[id][i])){cb, ctx}; return 0; }
    return -2;
}
static int unsubscribe(msg_id_t id, callback_t cb, void *ctx)
{
    if (!initialized || id <= MSG_NONE || id >= MSG_MAX || !cb) return -1;
    for (int i = 0; i < MAX_SUBS; ++i) if (subs[id][i].cb == cb && subs[id][i].ctx == ctx) { memset(&subs[id][i], 0, sizeof subs[id][i]); return 0; }
    return -3;
}
static int publish(const message_t *m)
{
    if (!initialized || !m || m->id <= MSG_NONE || m->id >= MSG_MAX) return -1;
    for (int i = 0; i < MAX_SUBS; ++i) if (subs[m->id][i].cb) subs[m->id][i].cb(m, subs[m->id][i].ctx);
    return 0;
}

static void count_cb(const message_t *m, void *ctx) { assert(m->value == 42); (*(int *)ctx)++; }

int main(void)
{
    message_t valid = {.id = MSG_KEY, .value = 42};
    message_t invalid = {.id = MSG_NONE, .value = 0};
    int count_a = 0, count_b = 0;

    /* Negative: operations before init and invalid messages. */
    assert(subscribe(MSG_KEY, count_cb, &count_a) < 0);
    assert(publish(&valid) < 0);
    assert(init_bus() == 0);
    assert(subscribe(MSG_NONE, count_cb, &count_a) < 0);
    assert(subscribe(MSG_KEY, NULL, &count_a) < 0);
    assert(publish(NULL) < 0);
    assert(publish(&invalid) < 0);

    /* Positive: multiple subscribers receive one event. */
    assert(subscribe(MSG_KEY, count_cb, &count_a) == 0);
    assert(subscribe(MSG_KEY, count_cb, &count_b) == 0);
    assert(publish(&valid) == 0);
    assert(count_a == 1 && count_b == 1);

    /* Negative: subscription capacity and missing unsubscribe. */
    assert(subscribe(MSG_KEY, count_cb, (void *)&valid) == 0);
    assert(subscribe(MSG_KEY, count_cb, (void *)&invalid) < 0);
    assert(subscribe(MSG_KEY, count_cb, NULL) < 0);
    assert(unsubscribe(MSG_WAKE, count_cb, &count_a) < 0);

    /* Positive/negative: unsubscribe removes only the matching registration. */
    assert(unsubscribe(MSG_KEY, count_cb, &count_a) == 0);
    assert(publish(&valid) == 0);
    assert(count_a == 1 && count_b == 2);

    puts("S31 message bus host positive/negative tests passed");
    return 0;
}
