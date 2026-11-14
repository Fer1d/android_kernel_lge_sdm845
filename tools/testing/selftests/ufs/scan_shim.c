/* Host stubs and assertions; kernel functions inserted at marker. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#define UFSHCD_INIT_RLS 4u
#define UFSHCD_INIT_EEH 2u
#define MSEC_PER_SEC 1000
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define spin_lock_irqsave(lock,flags) do {(void)(lock);(flags)=0;locked++;}while(0)
#define spin_unlock_irqrestore(lock,flags) do {(void)(lock);(void)(flags);locked--;}while(0)
typedef int async_cookie_t;
struct work_struct {int calls;};
#define UFSHCD_INIT 0
#define UFSHCD_LIVE 1
#define UFSHCD_STOPPING 2
struct Scsi_Host {int host_lock;};
struct ufs_hba {int init_lifecycle;bool init_failed;struct work_struct eh_work;struct Scsi_Host *host;void *dev;unsigned scan_pending;bool init_scan_ready,init_scan_finished;bool eh_in_progress,pm_op_in_progress;bool online;void *sdev_ufs_device;struct work_struct card_detect_work,eeh_work,rls_work;};
static int ufshcd_queue_recovery_dummy;
static int locked,votes,probe_ret,hold_ret,release_count,pm_puts,suspends,extcons,irq_injected;
static bool schedule_work(struct work_struct *w){assert(locked);w->calls++;return true;}
static void pm_runtime_get_noresume(void *d){(void)d;votes++;}
static int ufshcd_hold_all(struct ufs_hba *h){(void)h;return hold_ret;}
static void ufshcd_release_all(struct ufs_hba *h){(void)h;release_count++;}
/* Deterministic IRQ interleaving at the probe-return boundary. */
static bool ufshcd_queue_recovery(struct ufs_hba *,struct work_struct *,unsigned int);
static int ufshcd_probe_hba(struct ufs_hba *h){h->eh_in_progress=true;h->pm_op_in_progress=true;irq_injected++;unsigned long f;spin_lock_irqsave(h->host->host_lock,f);assert(ufshcd_queue_recovery(h,&h->eh_work,1));spin_unlock_irqrestore(h->host->host_lock,f);return probe_ret;}
static int ufshcd_detect_device(struct ufs_hba *h){return ufshcd_probe_hba(h);}
static void pm_runtime_put_noidle(void *d){(void)d;pm_puts++;}
static void pm_schedule_suspend(void *d,int t){(void)d;(void)t;suspends++;}
static void ufshcd_extcon_register(struct ufs_hba *h){(void)h;extcons++;}
static void pm_runtime_get_sync(void *d){(void)d;}
static void pm_runtime_put_sync(void *d){(void)d;}
static bool ufshcd_is_card_online(struct ufs_hba *h){return h->online;}
static bool ufshcd_is_card_offline(struct ufs_hba *h){return !h->online;}
static void ufshcd_remove_device(struct ufs_hba *h){(void)h;}
static void ufsdbg_clr_err_state(struct ufs_hba *h){(void)h;}

/* ACTUAL_SOURCE */

int main(void){struct Scsi_Host host={0};
 for(int caller=0;caller<2;caller++)for(int fail=0;fail<2;fail++){
 struct ufs_hba h={.init_lifecycle=UFSHCD_LIVE,.host=&host,.scan_pending=6,.online=true};votes=0;probe_ret=fail?-EIO:0;hold_ret=0;irq_injected=0;
 if(caller)ufshcd_card_detect_handler(&h.card_detect_work);else ufshcd_async_scan(&h,0);
 assert(h.eh_work.calls==1);assert(irq_injected==1&&h.eh_in_progress&&h.pm_op_in_progress);assert(h.init_scan_finished&&!h.scan_pending&&!locked);
 assert(h.init_scan_ready==!fail);assert(h.rls_work.calls==!fail);assert(h.eeh_work.calls==!fail);assert(votes==!fail);
 }
 {struct ufs_hba h={.init_lifecycle=UFSHCD_LIVE,.host=&host,.scan_pending=6};hold_ret=-EIO;pm_puts=suspends=0;ufshcd_async_scan(&h,0);assert(h.init_scan_finished&&!h.init_scan_ready&&!h.scan_pending);assert(pm_puts==1&&suspends==1);}
 puts("PASS: extracted async/card callers commit success/failure despite EH+PM IRQ injection; async hold-failure commits failure");return 0;}
