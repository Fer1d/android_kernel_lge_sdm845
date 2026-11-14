/* Included AFTER the actual C translation unit: no copied scheduler policy. */
static unsigned long checks;
#define CHECK(x) do { assert(x); checks++; } while (0)
static struct lock qlock;
static struct request_queue q;
static const unsigned int group_op[] = {
 REQ_OP_READ, REQ_OP_WRITE, REQ_OP_DISCARD, REQ_OP_FLUSH
};
static void setup(void)
{
 memset(&q, 0, sizeof(q)); memset(&qlock, 0, sizeof(qlock));
 q.queue_lock = &qlock; INIT_LIST_HEAD(&q.queue_head);
 CHECK(iosched_adios.ops.elevator_init_fn(&q, &iosched_adios) == 0);
 CHECK(live_eq == 1 && live_data == 1);
}
static void finish(void)
{
 CHECK(q.nr_sorted == 0 && list_empty(&q.queue_head));
 iosched_adios.ops.elevator_exit_fn(q.elevator);
 kobject_put(&q.elevator->kobj);
 CHECK(!live_eq && !live_data);
}
static void add(struct request *r, unsigned int op, unsigned int id)
{
 memset(r, 0, sizeof(*r)); INIT_LIST_HEAD(&r->queuelist);
 r->op = op; r->id = id; r->hashed = 1; r->sector = id;
 take(q.queue_lock); q.nr_sorted++; q.last_merge = r;
 iosched_adios.ops.elevator_add_req_fn(&q, r); drop(q.queue_lock);
}
static struct request *dispatch(int force)
{
 struct request *r; unsigned int before = q.nr_sorted; int ret;
 take(q.queue_lock); ret = iosched_adios.ops.elevator_dispatch_fn(&q, force); drop(q.queue_lock);
 CHECK(ret == !!before); /* strongest no-false-stall invariant */
 if (!ret) return NULL;
 CHECK(q.nr_sorted == before - 1);
 CHECK(!list_empty(&q.queue_head));
 r = list_first_entry(&q.queue_head, struct request, queuelist);
 CHECK(r->queuelist.next == &q.queue_head && !r->hashed);
 CHECK(q.last_merge != r && q.boundary_rq == r && q.end_sector == r->sector + 1);
 list_del_init(&r->queuelist); return r;
}
static void merge(struct request *survivor, struct request *victim)
{
 /* Model core elv_merge_requests sorted-victim accounting. */
 take(q.queue_lock);
 iosched_adios.ops.elevator_merge_req_fn(&q, survivor, victim);
 q.nr_sorted--; victim->hashed = 0; q.last_merge = survivor;
 drop(q.queue_lock);
 CHECK(list_empty(&victim->queuelist));
}
static void test_init_failures(void)
{
 struct elevator_queue sentinel;
 memset(&q, 0, sizeof(q)); q.queue_lock = &qlock; q.elevator = &sentinel;
 fail_eq = 1;
 CHECK(adios_init_queue(&q, &iosched_adios) == -ENOMEM);
 CHECK(q.elevator == &sentinel && !live_eq && !live_data);
 fail_eq = 0; fail_data = 1;
 CHECK(adios_init_queue(&q, &iosched_adios) == -ENOMEM);
 CHECK(q.elevator == &sentinel && !live_eq && !live_data);
 fail_data = 0;
}
static void test_sysfs(void)
{
 const char *bad[] = { "0", "257", "-1", "", " ", "1x", "1 2", "1\n\n",
  " 1", "1 ", "0x10", "4294967296", "4294967297", "18446744073709551616",
  "999999999999999999999999999999999999999999999999999999999999" };
 char buf[128], value[32]; unsigned int i, op, n; ssize_t ret;
 struct adios_data *ad;
 setup(); ad = q.elevator->elevator_data;
 CHECK(adios_version_show(q.elevator, buf) > 0 && strstr(buf, "not ADIOS 3.3"));
 for (op = 0; op < ADIOS_OPTYPES; op++) {
  struct elv_fs_entry *attr = &adios_attrs[op + 1];
  CHECK(attr->mode == 0644 && attr->show && attr->store);
  CHECK(attr->show(q.elevator, buf) > 0);
  CHECK((unsigned int)atoi(buf) == default_batch_limit[op]);
  for (i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
   n = ad->batch_limit[op]; ret = attr->store(q.elevator, bad[i], strlen(bad[i]));
   CHECK(ret < 0 && ad->batch_limit[op] == n);
  }
  for (n = 1; n <= 256; n++) {
   sprintf(value, "%u\n", n);
   CHECK(attr->store(q.elevator, value, strlen(value)) == (ssize_t)strlen(value));
   CHECK(attr->show(q.elevator, buf) > 0 && !strcmp(buf, value));
   CHECK(ad->batch_limit[op] == n);
  }
  CHECK(attr->store(q.elevator, "+1\n", 3) == 3);
 }
 CHECK(!adios_attrs[5].name); finish();
}
static void test_merge(void)
{
 struct request r[6]; struct request *p; unsigned int i;
 setup();
 for (i = 0; i < 4; i++) add(&r[i], REQ_OP_READ, i);
 merge(&r[1], &r[0]); /* first */
 merge(&r[1], &r[2]); /* middle */
 merge(&r[1], &r[3]); /* last */
 /* Front/back sector changes need no tree reindex and retain FIFO age. */
 r[1].sector = 1000;
 p = dispatch(0); CHECK(p == &r[1] && list_empty(&r[1].queuelist));
 CHECK(dispatch(0) == NULL);
 add(&r[0], REQ_OP_WRITE, 0); add(&r[1], REQ_OP_WRITE, 1);
 merge(&r[0], &r[1]); CHECK(dispatch(1) == &r[0]);
 /* singleton victim in another FIFO: unlink lifecycle only (core decides
  * merge compatibility); callback must not corrupt any queue. */
 add(&r[0], REQ_OP_READ, 0); add(&r[1], REQ_OP_WRITE, 1);
 merge(&r[0], &r[1]); CHECK(dispatch(1) == &r[0]);
 add(&r[1], REQ_OP_READ, 1); CHECK(dispatch(0) == &r[1]);
 CHECK(dispatch(1) == NULL); finish();
}
static void test_batches(void)
{
 struct request *r; unsigned int limit, i, op, round, pos, count;
 struct adios_data *ad;
 /* Exhaust every valid batch value, all groups saturated for two turns. */
 for (limit = 1; limit <= 256; limit++) {
  setup(); ad = q.elevator->elevator_data;
  count = 8 * limit; r = calloc(count, sizeof(*r)); CHECK(r != NULL);
  for (op = 0; op < 4; op++) {
   char buf[16]; sprintf(buf, "%u", limit);
   CHECK(adios_attrs[op + 1].store(q.elevator, buf, strlen(buf)) > 0);
   for (i = 0; i < 2 * limit; i++) add(&r[op * 2 * limit + i], group_op[op], i);
  }
  for (round = 0; round < 2; round++)
   for (op = 0; op < 4; op++)
    for (pos = 0; pos < limit; pos++) {
     CHECK(dispatch(round) == &r[op * 2 * limit + round * limit + pos]);
     CHECK(ad->batch_count <= limit);
    }
  CHECK(dispatch(0) == NULL); finish(); free(r);
 }
 /* One populated group must never stall, even at limit 1. */
 for (op = 0; op < 4; op++) {
  struct request single[9];
  setup(); ad = q.elevator->elevator_data; ad->batch_limit[op] = 1;
  for (i = 0; i < 9; i++) add(&single[i], group_op[op], i);
  for (i = 0; i < 9; i++) CHECK(dispatch(i & 1) == &single[i]);
  CHECK(!dispatch(0)); finish();
 }
}
static void test_defaults_and_change(void)
{
 struct request r[160]; unsigned int op, i, n = 0, at = 0;
 unsigned int defaults[] = {32,16,8,8};
 setup();
 for (op = 0; op < 4; op++)
  for (i = 0; i < defaults[op]; i++) { add(&r[n], group_op[op], n); n++; }
 for (op = 0; op < 4; op++)
  for (i = 0; i < defaults[op]; i++) CHECK(dispatch(0) == &r[at++]);
 CHECK(!dispatch(0)); finish();
 setup();
 for (i = 0; i < 40; i++) add(&r[i], REQ_OP_READ, i);
 add(&r[40], REQ_OP_WRITE, 40);
 for (i = 0; i < 10; i++) CHECK(dispatch(0) == &r[i]);
 CHECK(adios_batch_limit_read_store(q.elevator, "1", 1) == 1);
 CHECK(dispatch(0) == &r[40]); /* shrinking current batch takes effect */
 for (i = 10; i < 40; i++) CHECK(dispatch(1) == &r[i]);
 CHECK(!dispatch(0)); finish();
}
static void test_sparse_masks(void)
{
 unsigned int mask, start, op, step, n;
 struct request r[4]; struct adios_data *ad;
 /* Every nonempty subset and starting cursor, with exhausted turns. */
 for (mask = 1; mask < 16; mask++)
  for (start = 0; start < 4; start++) {
   setup(); ad = q.elevator->elevator_data;
   ad->current_optype = start; ad->batch_count = 256;
   for (op = 0; op < 4; op++)
    if (mask & (1U << op)) add(&r[op], group_op[op], op);
   n = 0;
   for (step = 1; step <= 4; step++) {
    op = (start + step) % 4;
    if (mask & (1U << op)) { CHECK(dispatch(step & 1) == &r[op]); n++; }
   }
   CHECK(n && !dispatch(0)); finish();
  }
 /* A recycled hot read cannot starve already waiting other classes. */
 setup(); ad = q.elevator->elevator_data;
 for (op = 0; op < 4; op++) add(&r[op], group_op[op], op);
 for (step = 0; step < 32; step++) {
  CHECK(dispatch(0) == &r[0]); add(&r[0], REQ_OP_READ, step);
 }
 CHECK(dispatch(0) == &r[1]); CHECK(dispatch(0) == &r[2]);
 CHECK(dispatch(0) == &r[3]); CHECK(dispatch(1) == &r[0]);
 CHECK(!dispatch(0)); finish();
}
static void test_classification(void)
{
 struct request r[4];
 setup();
 add(&r[0], REQ_OP_SECURE_ERASE, 0); add(&r[1], REQ_OP_DISCARD, 1);
 add(&r[2], REQ_OP_FLUSH, 2); add(&r[3], REQ_OP_WRITE_SAME, 3);
 CHECK(adios_optype(&r[0]) == ADIOS_DISCARD);
 CHECK(adios_optype(&r[2]) == ADIOS_OTHER);
 CHECK(adios_optype(&r[3]) == ADIOS_OTHER);
 CHECK(dispatch(0) == &r[0]); CHECK(dispatch(0) == &r[1]);
 CHECK(dispatch(0) == &r[2]); CHECK(dispatch(0) == &r[3]);
 CHECK(!dispatch(0)); finish();
}
static void test_nonempty_exit(void)
{
 pid_t pid; int status; struct request r;
 setup(); add(&r, REQ_OP_READ, 0);
 pid = fork(); CHECK(pid >= 0);
 if (!pid) { adios_exit_queue(q.elevator); _exit(77); }
 CHECK(waitpid(pid, &status, 0) == pid);
 CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
 CHECK(dispatch(1) == &r); finish();
}
static int pre_p2_init(struct request_queue *queue, struct elevator_type *type)
{
 /* Negative control: exact old allocation/release order, real core release. */
 struct elevator_queue *eq = elevator_alloc(queue, type);
 void *data;
 if (!eq) return -ENOMEM;
 data = kzalloc_node(sizeof(struct adios_data), GFP_KERNEL, queue->node);
 if (!data) { kobject_put(&eq->kobj); return -ENOMEM; }
 abort(); /* this control is used only with fail_data */
}
static void reset_modules(void)
{
 memset(&adios_module, 0, sizeof(adios_module));
 memset(&old_module, 0, sizeof(old_module));
}
static int initial(void)
{
 int ret;
 take(&q.sysfs_lock); ret = elevator_init(&q, "adios"); drop(&q.sysfs_lock);
 return ret;
}
static void test_core_refs(void)
{
 struct elevator_type old_type = iosched_adios;
 struct elevator_queue *old;
 int stage, builtin, reg;
 old_type.elevator_name = "old"; old_type.elevator_owner = &old_module;
 mock_types[0] = &iosched_adios; mock_types[1] = &old_type;
 /* First prove the old test missed the reported double-put. */
 memset(&q, 0, sizeof(q)); q.queue_lock = &qlock;
 reset_modules(); iosched_adios.elevator_owner = &adios_module;
 iosched_adios.ops.elevator_init_fn = pre_p2_init; fail_data = 1;
 CHECK(initial() == -ENOMEM);
 CHECK(adios_module.gets == 1 && adios_module.puts == 2);
 CHECK(adios_module.refs == -1 && adios_module.underflows == 1);
 CHECK(!live_eq && !live_data && !q.elevator);
 iosched_adios.ops.elevator_init_fn = adios_init_queue; fail_data = 0;
 for (builtin = 0; builtin <= 1; builtin++) {
  iosched_adios.elevator_owner = builtin ? NULL : &adios_module;
  for (stage = 0; stage < 3; stage++) {
   reset_modules(); memset(&q, 0, sizeof(q)); q.queue_lock = &qlock;
   fail_data = stage == 0; fail_eq = stage == 1;
   CHECK(initial() == (stage < 2 ? -ENOMEM : 0));
   CHECK(!adios_module.underflows);
   CHECK(adios_module.refs == (!builtin && stage == 2));
   if (stage == 2) {
    CHECK(initial() == 0); /* already initialised: no extra get */
    CHECK(elevator_change(&q, " adios\n") == 0); /* same-type get/put */
    CHECK(elevator_change(&q, "missing") == -EINVAL);
    CHECK(adios_module.refs == !builtin);
    elevator_exit(q.elevator);
   }
   CHECK(!adios_module.refs && !live_eq && !live_data);
   fail_data = fail_eq = 0;
  }
  for (reg = 0; reg <= 1; reg++)
   for (stage = 0; stage < 4; stage++) {
    reset_modules(); memset(&q, 0, sizeof(q)); q.queue_lock = &qlock;
    CHECK(elevator_get("old", true) == &old_type);
    CHECK(adios_init_queue(&q, &old_type) == 0);
    old = q.elevator; old->registered = reg;
    CHECK(old_module.refs == 1);
    fail_data = stage == 0; fail_eq = stage == 1;
    fail_register = stage == 2 && reg;
    CHECK(elevator_change(&q, "adios") ==
          (stage < 2 ? -ENOMEM : (stage == 2 && reg ? -EIO : 0)));
    CHECK(!q.bypass && bypass_starts == bypass_ends);
    CHECK(!adios_module.underflows && !old_module.underflows);
    if (stage < 2) {
     CHECK(q.elevator == old && old_module.refs == 1);
     /* Known core negative regression: fail_init leaks the acquired
      * module ref. Assert it remains visible; do NOT mask in the mock.
      * Built-in owner NULL avoids it, hence Kconfig bool is necessary.
      */
     CHECK(adios_module.refs == !builtin);
     if (!builtin) module_put(&adios_module); /* harness-only leak cleanup */
    } else if (stage == 2 && reg) {
     CHECK(q.elevator == old && old_module.refs == 1 && !adios_module.refs);
    } else {
     CHECK(q.elevator != old && !old_module.refs);
     CHECK(adios_module.refs == !builtin);
    }
    fail_data = fail_eq = fail_register = 0;
    elevator_exit(q.elevator);
    CHECK(!adios_module.refs && !old_module.refs && !live_eq && !live_data);
   }
 }
 iosched_adios.elevator_owner = NULL;
 printf("PASS: core negative controls expose pre-P2 double-put and modular switch fail_init leak; built-in paths balanced\n");
}
int main(void)
{
 CHECK(test_module_init() == 0 && registered);
 test_init_failures(); test_sysfs(); test_merge(); test_batches();
 test_defaults_and_change(); test_sparse_masks(); test_classification(); test_nonempty_exit();
 test_core_refs();
 test_module_exit(); CHECK(!registered && !live_eq && !live_data);
 printf("PASS: %lu checks; actual scheduler C + actual kstrtouint/dispatch-tail; ASan/UBSan\n", checks);
 return 0;
}
