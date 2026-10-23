#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef struct { int value; } task_result;
static struct { bool ASIC_initalized; } state = {.ASIC_initalized = true}, *g_state = &state;
static atomic_bool g_work_paused;
static pthread_mutex_t rx_mutex = PTHREAD_MUTEX_INITIALIZER;
static SemaphoreHandle_t g_asic_result_lock = &rx_mutex;
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned lock_calls, read_calls;
static bool read_active, release_read, waiter_done;
static task_result result = {42};

static int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) {
    pthread_mutex_lock(&test_mutex);
    ++lock_calls; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&test_mutex);
    if (timeout == portMAX_DELAY) {
        assert(pthread_mutex_lock(lock) == 0);
        return pdTRUE;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout / 1000;
    deadline.tv_nsec += (long)(timeout % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    const int err = pthread_mutex_timedlock(lock, &deadline);
    assert(err == 0 || err == ETIMEDOUT);
    return err == 0 ? pdTRUE : 0;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) { assert(pthread_mutex_unlock(lock) == 0); }
static task_result *BM1370_process_work(void *unused) {
    (void)unused;
    pthread_mutex_lock(&test_mutex);
    ++read_calls; read_active = true; pthread_cond_broadcast(&changed);
    while (!release_read) pthread_cond_wait(&changed, &test_mutex);
    read_active = false;
    pthread_mutex_unlock(&test_mutex);
    return &result;
}

/* ASIC_RX_UNDER_TEST */

static void *reader(void *unused) {
    (void)unused;
    assert(read_asic_result() == NULL); // A result from before the pause is discarded.
    return NULL;
}
static void *waiter(void *unused) {
    (void)unused;
    assert(stratum_minimal_wait_for_asic_rx_idle(500));
    pthread_mutex_lock(&test_mutex);
    assert(!read_active);
    waiter_done = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&test_mutex);
    return NULL;
}
static void wait_for(bool (*ready)(void)) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&test_mutex);
    while (!ready()) assert(pthread_cond_timedwait(&changed, &test_mutex, &deadline) == 0);
    pthread_mutex_unlock(&test_mutex);
}
static bool receiving(void) { return read_active; }
static bool waiter_attempted_lock(void) { return lock_calls >= 2; }

int main(void) {
    pthread_t rx, drain;
    assert(pthread_create(&rx, NULL, reader, NULL) == 0);
    wait_for(receiving);
    atomic_store(&g_work_paused, true);
    assert(pthread_create(&drain, NULL, waiter, NULL) == 0);
    wait_for(waiter_attempted_lock);
    pthread_mutex_lock(&test_mutex);
    assert(!waiter_done && read_active);
    release_read = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&test_mutex);
    pthread_join(rx, NULL); pthread_join(drain, NULL);
    assert(waiter_done && read_calls == 1);

    // No new UART read can sneak in after the drain while startup is paused.
    assert(read_asic_result() == NULL && read_calls == 1);
    atomic_store(&g_work_paused, false);
    g_state->ASIC_initalized = false;
    assert(read_asic_result() == NULL && read_calls == 1);
    g_state->ASIC_initalized = true;
    assert(read_asic_result() == &result && read_calls == 2);

    // A pending reader can outlast a short recovery window. Timing out must
    // not release its lock or start another receive; a later retry can drain it.
    release_read = false;
    assert(pthread_create(&rx, NULL, reader, NULL) == 0);
    wait_for(receiving);
    atomic_store(&g_work_paused, true);
    assert(!stratum_minimal_wait_for_asic_rx_idle(10));
    pthread_mutex_lock(&test_mutex);
    assert(read_active);
    release_read = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&test_mutex);
    pthread_join(rx, NULL);
    assert(stratum_minimal_wait_for_asic_rx_idle(10));
    assert(read_calls == 3);

    // Cold startup is valid before Stratum tasks and their mutex exist.
    g_asic_result_lock = NULL;
    assert(stratum_minimal_wait_for_asic_rx_idle(0));
    puts("ASIC RX transition quiesce tests passed");
}
