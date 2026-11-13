/* Bounded, ownership-safe reply cache and asynchronous wait lifetime. */
#define SB26_XPC_CACHE_MAX 64
#define SB26_XPC_PENDING_MAX 64

static struct { char key[72]; xpc_object_t reply; } sb26_xpc_cache[SB26_XPC_CACHE_MAX];
static unsigned sb26_xpc_cache_n;
static unsigned sb26_xpc_cache_last;
static unsigned sb26_xpc_cache_next;
static pthread_mutex_t sb26_xpc_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static xpc_object_t
sb26_xpc_cache_get(const char *key)
{
    xpc_object_t reply = NULL;
    pthread_mutex_lock(&sb26_xpc_cache_lock);
    for (unsigned i = 0; i < sb26_xpc_cache_n; i++) {
        if (!strcmp(sb26_xpc_cache[i].key, key)) {
            reply = xpc_retain(sb26_xpc_cache[i].reply);
            break;
        }
    }
    pthread_mutex_unlock(&sb26_xpc_cache_lock);
    return reply;
}

static xpc_object_t
sb26_xpc_cache_any(void)
{
    xpc_object_t reply = NULL;
    pthread_mutex_lock(&sb26_xpc_cache_lock);
    if (sb26_xpc_cache_n != 0)
        reply = xpc_retain(sb26_xpc_cache[sb26_xpc_cache_last].reply);
    pthread_mutex_unlock(&sb26_xpc_cache_lock);
    return reply;
}

static void
sb26_xpc_cache_put(const char *key, xpc_object_t reply)
{
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY) return;

    xpc_object_t held = xpc_retain(reply);
    xpc_object_t old = NULL;
    pthread_mutex_lock(&sb26_xpc_cache_lock);
    unsigned slot;
    for (slot = 0; slot < sb26_xpc_cache_n; slot++)
        if (!strcmp(sb26_xpc_cache[slot].key, key)) break;
    if (slot == sb26_xpc_cache_n) {
        if (sb26_xpc_cache_n < SB26_XPC_CACHE_MAX) {
            slot = sb26_xpc_cache_n++;
        } else {
            slot = sb26_xpc_cache_next;
            sb26_xpc_cache_next = (sb26_xpc_cache_next + 1) % SB26_XPC_CACHE_MAX;
        }
    }
    old = sb26_xpc_cache[slot].reply;
    snprintf(sb26_xpc_cache[slot].key, sizeof(sb26_xpc_cache[slot].key), "%s", key);
    sb26_xpc_cache[slot].reply = held;
    sb26_xpc_cache_last = slot;
    pthread_mutex_unlock(&sb26_xpc_cache_lock);
    if (old) xpc_release(old);
}

static xpc_object_t
sb26_xpc_cache_fallback(const char *key, int *used_cache)
{
    xpc_object_t reply = sb26_xpc_cache_get(key);
    if (!reply) reply = sb26_xpc_cache_any();
    if (used_cache) *used_cache = reply != NULL;
    if (!reply) reply = xpc_dictionary_create(NULL, NULL, 0);
    return reply;
}

struct sb26_xpc_reply_wait {
    atomic_uint refs;
    pthread_mutex_t lock;
    dispatch_semaphore_t done;
    xpc_object_t reply;
};

static atomic_uint sb26_xpc_pending;

static int
sb26_xpc_pending_reserve(void)
{
    unsigned count = atomic_load(&sb26_xpc_pending);
    while (count < SB26_XPC_PENDING_MAX) {
        if (atomic_compare_exchange_weak(&sb26_xpc_pending, &count, count + 1))
            return 1;
    }
    return 0;
}

static struct sb26_xpc_reply_wait *
sb26_xpc_reply_wait_create(void)
{
    if (!sb26_xpc_pending_reserve()) return NULL;
    struct sb26_xpc_reply_wait *wait = calloc(1, sizeof(*wait));
    if (!wait) {
        atomic_fetch_sub(&sb26_xpc_pending, 1);
        return NULL;
    }
    if (pthread_mutex_init(&wait->lock, NULL) != 0) {
        free(wait);
        atomic_fetch_sub(&sb26_xpc_pending, 1);
        return NULL;
    }
    wait->done = dispatch_semaphore_create(0);
    if (!wait->done) {
        pthread_mutex_destroy(&wait->lock);
        free(wait);
        atomic_fetch_sub(&sb26_xpc_pending, 1);
        return NULL;
    }
    atomic_init(&wait->refs, 2); /* synchronous caller and eventual reply block */
    return wait;
}

static void
sb26_xpc_reply_wait_release(struct sb26_xpc_reply_wait *wait)
{
    if (atomic_fetch_sub(&wait->refs, 1) != 1) return;
    if (wait->reply) xpc_release(wait->reply);
    dispatch_release(wait->done);
    pthread_mutex_destroy(&wait->lock);
    free(wait);
}

static void
sb26_xpc_reply_wait_complete(struct sb26_xpc_reply_wait *wait, xpc_object_t reply)
{
    xpc_object_t held = reply ? xpc_retain(reply) : NULL;
    pthread_mutex_lock(&wait->lock);
    xpc_object_t old = wait->reply;
    wait->reply = held;
    pthread_mutex_unlock(&wait->lock);
    if (old) xpc_release(old);
    dispatch_semaphore_signal(wait->done);
    atomic_fetch_sub(&sb26_xpc_pending, 1);
    sb26_xpc_reply_wait_release(wait);
}

static xpc_object_t
sb26_xpc_reply_wait_take(struct sb26_xpc_reply_wait *wait)
{
    pthread_mutex_lock(&wait->lock);
    xpc_object_t reply = wait->reply;
    wait->reply = NULL;
    pthread_mutex_unlock(&wait->lock);
    return reply;
}
