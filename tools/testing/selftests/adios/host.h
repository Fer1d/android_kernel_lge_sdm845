/* Host kernel API mock, NOT an in-kernel integration test.
 * Lists implement intrusive kernel semantics and assert reciprocal links.
 * Locks assert required serialisation, but do not model concurrency/IRQ/SMP.
 * Allocation/kobject mocks track ownership and allow exact failure injection.
 * Request op values are symbolic; core merge hash is a membership boolean.
 * No real bio, flush machine, rb-tree, sysfs/VFS, module loader or device I/O.
 * Core alloc/release/get/put/init/exit/switch/change, dispatch tail and
 * kstrtouint are extracted from this tree, not reimplemented. Module mocks
 * count get/put and underflow, including known-broken negative controls.
 * Registry lookup, sysfs registration and bypass/drain boundaries are mocks;
 * switching tests use empty queues, not the real block-layer drain or RCU.
 * These tests cannot certify device/concurrent lifecycle correctness.
 * Keep candidate built-in only and default n.
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define ELV_NAME_MAX 32
#define CONFIG_DEFAULT_IOSCHED "adios"
#define KERN_ERR ""
#define printk(...) ((void)0)
#define lockdep_assert_held(l) assert((l)->held)
#define hash_init(h) ((void)(h))
#define mutex_init(l) memset(l, 0, sizeof(*(l)))
#define mutex_lock(l) take(l)
#define mutex_unlock(l) drop(l)
#define spin_lock(l) take(l)
#define spin_unlock(l) drop(l)
struct module { int refs, gets, puts, underflows; };
static struct module adios_module, old_module;
static int try_module_get(struct module *m)
{
 if (m) { m->refs++; m->gets++; } return 1;
}
static void module_put(struct module *m)
{
 if (m) { m->refs--; m->puts++; if (m->refs < 0) m->underflows++; }
}
#define unlikely(x) (x)
#undef _tolower
#define _tolower(c) tolower((unsigned char)(c))
#define div_u64(a, b) ((a) / (b))
#define KSTRTOX_OVERFLOW (1U << 31)
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define __init
#define __exit
#define BUG_ON(x) do { if (x) abort(); } while (0)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define module_init(x) static int (*test_module_init)(void) = x
#define module_exit(x) static void (*test_module_exit)(void) = x
struct list_head { struct list_head *next, *prev; };
#define INIT_LIST_HEAD(h) do { (h)->next = (h); (h)->prev = (h); } while (0)
static int list_empty(const struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{
 assert(h->prev->next == h && h->next->prev == h);
 n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static void list_del_init(struct list_head *n)
{
 assert(n->prev->next == n && n->next->prev == n);
 n->prev->next = n->next; n->next->prev = n->prev; INIT_LIST_HEAD(n);
}
#define list_first_entry(h, t, m) ((t *)((char *)(h)->next - offsetof(t, m)))
enum { REQ_OP_READ, REQ_OP_WRITE, REQ_OP_DISCARD, REQ_OP_SECURE_ERASE,
       REQ_OP_FLUSH, REQ_OP_WRITE_SAME };
struct request { struct list_head queuelist; unsigned int op, id; int hashed; unsigned long sector; };
#define req_op(r) ((r)->op)
#define rq_end_sector(r) ((r)->sector + 1)
struct lock { int held; };
static void take(struct lock *l) { assert(!l->held); l->held = 1; }
static void drop(struct lock *l) { assert(l->held); l->held = 0; }
#define spin_lock_irq(l) take(l)
#define spin_unlock_irq(l) drop(l)
#define spin_lock_irqsave(l, f) do { (f) = 0; take(l); } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); drop(l); } while (0)
struct elevator_type;
struct kobject;
struct kobj_type { void (*release)(struct kobject *); };
struct kobject { int live; struct kobj_type *type; };
struct elevator_queue {
 void *elevator_data; struct kobject kobj; struct elevator_type *type;
 struct lock sysfs_lock; int hash, registered;
};
struct request_queue {
 struct elevator_queue *elevator;
 struct lock *queue_lock;
 struct lock sysfs_lock;
 int bypass;
 int node;
 struct list_head queue_head;
 struct request *last_merge, *boundary_rq;
 unsigned int nr_sorted;
 unsigned long end_sector;
};
struct elevator_ops {
 void (*elevator_merge_req_fn)(struct request_queue *, struct request *, struct request *);
 int (*elevator_dispatch_fn)(struct request_queue *, int);
 void (*elevator_add_req_fn)(struct request_queue *, struct request *);
 int (*elevator_init_fn)(struct request_queue *, struct elevator_type *);
 void (*elevator_exit_fn)(struct elevator_queue *);
};
struct elv_fs_entry {
 const char *name; unsigned int mode;
 ssize_t (*show)(struct elevator_queue *, char *);
 ssize_t (*store)(struct elevator_queue *, const char *, size_t);
};
#define __ATTR(n, m, sh, st) { #n, m, sh, st }
#define __ATTR_NULL { NULL, 0, NULL, NULL }
struct elevator_type {
 struct elevator_ops ops; struct elv_fs_entry *elevator_attrs;
 const char *elevator_name; void *elevator_owner;
};
static int fail_eq, fail_data, live_eq, live_data, registered;
static void *eq_allocations[8];
static void kobject_init(struct kobject *k, struct kobj_type *t)
{ k->live = 1; k->type = t; }
static void kobject_put(struct kobject *k)
{ assert(k->live); k->live = 0; k->type->release(k); }
static void *kzalloc_node(size_t n, int flags, int node)
{
 void *p; unsigned int i;
 if (n == sizeof(struct elevator_queue)) {
  if (fail_eq) return NULL;
  p = calloc(1, n); assert(p);
  for (i = 0; i < 8 && eq_allocations[i]; i++);
  assert(i < 8); eq_allocations[i] = p; live_eq++; return p;
 }
 if (fail_data) return NULL;
 p = calloc(1, n); assert(p); live_data++; return p;
}
static void kfree(void *p)
{
 unsigned int i; assert(p);
 for (i = 0; i < 8; i++) if (eq_allocations[i] == p) {
  eq_allocations[i] = NULL; live_eq--; free(p); return;
 }
 assert(live_data > 0); live_data--; free(p);
}
static struct elevator_type *mock_types[2];
static struct lock elv_list_lock;
static struct elevator_type *elevator_find(const char *name)
{
 unsigned int i;
 for (i = 0; i < 2; i++)
  if (mock_types[i] && !strcmp(name, mock_types[i]->elevator_name)) return mock_types[i];
 return NULL;
}
#define request_module(...) ((void)0)
static int fail_register, bypass_starts, bypass_ends;
static void blk_queue_bypass_start(struct request_queue *q)
{ assert(!q->bypass && !q->nr_sorted); q->bypass = 1; bypass_starts++; }
static void blk_queue_bypass_end(struct request_queue *q)
{ assert(q->bypass); q->bypass = 0; bypass_ends++; }
static void elv_unregister_queue(struct request_queue *q) { q->elevator->registered = 0; }
static int elv_register_queue(struct request_queue *q)
{
 if (fail_register) { fail_register = 0; return -EIO; }
 q->elevator->registered = 1; return 0;
}
static void ioc_clear_queue(struct request_queue *q) { assert(q->queue_lock->held); }
static size_t strlcpy(char *dst, const char *src, size_t n)
{ size_t len = strlen(src); if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(dst, src, k); dst[k] = 0; } return len; }
static char *strstrip(char *s)
{ char *end; while (isspace((unsigned char)*s)) s++; end = s + strlen(s); while (end > s && isspace((unsigned char)end[-1])) *--end = 0; return s; }
#define blk_add_trace_msg(q, ...) ((void)(q))
static void elv_rqhash_del(struct request_queue *q, struct request *r)
{
 assert(q->queue_lock->held && q->nr_sorted); r->hashed = 0;
}
static int elv_register(struct elevator_type *e)
{
 assert(!registered && !strcmp(e->elevator_name, "adios")); registered = 1; return 0;
}
static void elv_unregister(struct elevator_type *e) { assert(registered); registered = 0; }
