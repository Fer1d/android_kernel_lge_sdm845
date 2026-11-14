/* Host environment, not Linux PM implementation. Actual init suffix and bus
 * probe functions are inserted unchanged. Trace assertions constrain ordering. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UFSHCD_INIT 0
#define UFSHCD_LIVE 1
#define UFSHCD_STOPPING 2
#define UFSHCD_INIT_EH 1
#define UFSHCD_INIT_EEH 2
#define UFSHCD_INIT_RLS 4
#define IRQF_SHARED 1
#define REG_INTERRUPT_ENABLE 1
#define UFS_SLEEP_PWR_MODE 1
#define UIC_LINK_HIBERN8_STATE 1
#define UFSHCD "ufs"
#define IORESOURCE_MEM 1
#define __iomem
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_name(d) "ufs-test"
#define INIT_WORK(...) ((void)0)
#define INIT_LIST_HEAD(...) ((void)0)
#define IS_ERR(p) false
#define PTR_ERR(p) (-EINVAL)
struct ufs_hba;
struct device {int usage,disabled,enabled;bool no_pm;void *dma_mask;int coherent_dma_mask;struct ufs_hba *hba;};
struct work_struct {int pending;};
struct Scsi_Host {int host_lock,host_no;};
struct ufs_hba_variant {int unused;};
struct ufs_hba {struct Scsi_Host *host;struct device *dev;int init_lifecycle;
 bool init_failed,init_scan_ready,init_scan_finished,init_irq_owned,is_irq_enabled;
 unsigned scan_pending,saved_err,saved_uic_err,saved_ce_err;
 bool force_host_reset,auto_h8_err;unsigned irq;
 struct work_struct eh_work,eeh_work,rls_work,card_detect_work;
 struct {struct work_struct suspend_work,resume_work;void *workq;} clk_scaling;
 int rpm_lvl,spm_lvl;struct ufs_hba_variant *var;};
struct platform_device {struct device dev;};
struct pci_dev {struct device dev;unsigned irq;};
struct pci_device_id {int unused;};
struct resource {int unused;};
static struct ufs_hba h;
static struct Scsi_Host host;
static int locked, stage, inject_at, irqfail, addfail, enablefail, workfail;
static int publishes,scheduled,freed,synced,removed,cancelled,stopped,enabled,pmcalls,gets;
static char trace[512];
static void event(char c){size_t n=strlen(trace);assert(n+1<sizeof(trace));trace[n]=c;trace[n+1]=0;}
#define spin_lock_irqsave(l,f) do {(void)(l);f=0;assert(!locked);locked=1;}while(0)
#define spin_unlock_irqrestore(l,f) do {(void)(l);(void)(f);assert(locked);locked=0;}while(0)
static bool ufshcd_queue_recovery(struct ufs_hba *,struct work_struct *,unsigned int);
static void inject(struct ufs_hba *b,int where){
 if(inject_at==where){unsigned long f;spin_lock_irqsave(b->host->host_lock,f);
 assert(!ufshcd_queue_recovery(b,&b->eh_work,UFSHCD_INIT_EH));
 assert(b->init_failed);b->saved_err=0;spin_unlock_irqrestore(b->host->host_lock,f);event('I');}
}
static bool schedule_work(struct work_struct *w){assert(locked);if(w->pending)return false;w->pending=1;return true;}
static void pm_runtime_get_noresume(struct device *d){event('N');gets++;if(!d->no_pm)d->usage++;inject(d->hba,3);}
static int pm_runtime_get_sync(struct device *d){event('G');gets++;if(!d->no_pm)d->usage++;inject(d->hba,3);return d->no_pm?-ENOSYS:(d->disabled?-EACCES:1);}
static void pm_runtime_put_noidle(struct device *d){event('P');if(!d->no_pm){assert(d->usage>0);d->usage--;}}
static void pm_runtime_set_active(struct device *d){assert(scheduled);event('A');(void)d;}
static void pm_runtime_enable(struct device *d){event('E');assert(scheduled);d->disabled=0;d->enabled=1;}
static void pm_runtime_allow(struct device *d){event('L');(void)d;}
static void *ufshcd_intr;
static void ufshcd_async_scan(void *a,int b){(void)a;(void)b;}
static int devm_request_irq(struct device *d,unsigned irq,void *fn,int flags,const char *name,void *b){
 (void)irq;(void)fn;(void)flags;(void)name;event('Q');inject(b,1);return irqfail?-EIO:0;}
static int scsi_add_host(struct Scsi_Host *s,struct device *d){(void)s;event('S');return addfail?-EIO:0;}
static void ufshcd_vops_full_reset(struct ufs_hba *b){(void)b;}
static int ufshcd_reset_device(struct ufs_hba *b){(void)b;return 0;}
static int ufshcd_hba_enable(struct ufs_hba *b){event('H');enabled++;inject(b,2);return enablefail?-EIO:0;}
static void ufshcd_print_host_regs(struct ufs_hba *b){(void)b;}
static void ufshcd_print_host_state(struct ufs_hba *b){(void)b;}
static bool ufshcd_is_clkscaling_supported(struct ufs_hba *b){(void)b;return true;}
static void *create_singlethread_workqueue(const char *n){(void)n;return workfail?NULL:(void *)1;}
static bool ufshcd_is_valid_pm_lvl(int l){return l==1;}
static int ufs_get_desired_pm_lvl_for_dev_link_state(int a,int b){(void)a;(void)b;return 1;}
static void ufshcd_set_ufs_dev_active(struct ufs_hba *b){(void)b;}
static void ufshcd_cmd_log_init(struct ufs_hba *b){inject(b,4);}
static void ufshcd_publish_init(struct ufs_hba *b){assert(b->init_lifecycle==UFSHCD_LIVE);assert(!b->init_failed);publishes++;event('U');}
static void async_schedule(void (*f)(void *,int),struct ufs_hba *b){(void)f;assert(publishes==1);assert(b->init_lifecycle==UFSHCD_LIVE);scheduled++;event('T');}
static void scsi_remove_host(struct Scsi_Host *s){(void)s;assert(h.init_lifecycle==UFSHCD_STOPPING);removed++;event('R');}
static void ufshcd_writel(struct ufs_hba *b,int v,int r){(void)b;(void)v;(void)r;event('M');}
static int ufshcd_readl(struct ufs_hba *b,int r){(void)b;(void)r;return 0;}
static void synchronize_irq(unsigned irq){(void)irq;assert(h.init_lifecycle==UFSHCD_STOPPING);synced++;event('Y');}
static void devm_free_irq(struct device *d,unsigned irq,struct ufs_hba *b){(void)d;(void)irq;assert(synced==1&&b->init_irq_owned);freed++;event('F');}
static void cancel_work_sync(struct work_struct *w){(void)w;assert(!h.init_irq_owned);cancelled++;event('C');}
static void ufshcd_exit_hibern8_on_idle(struct ufs_hba *b){(void)b;}
static void ufshcd_exit_clk_gating(struct ufs_hba *b){(void)b;}
static void __ufshcd_shutdown_clkscaling(struct ufs_hba *b){(void)b;}
static void destroy_workqueue(void *w){(void)w;}
static void ufshcd_hba_stop(struct ufs_hba *b,bool v){(void)b;(void)v;stopped++;event('B');}
static void ufshcd_hba_exit(struct ufs_hba *b){(void)b;event('X');}
static void dev_set_drvdata(struct device *d,void *b){(void)d;assert(!b);event('D');}
static struct resource *platform_get_resource(struct platform_device *p,int t,int i){(void)p;(void)t;(void)i;static struct resource r;return &r;}
static void *devm_ioremap_resource(struct device *d,struct resource *r){(void)d;(void)r;return (void *)1;}
static int platform_get_irq(struct platform_device *p,int i){(void)p;(void)i;return 1;}
static int ufshcd_alloc_host(struct device *d,struct ufs_hba **b){h.host=&host;h.dev=d;h.irq=1;d->hba=&h;*b=&h;return 0;}
static void ufshcd_dealloc_host(struct ufs_hba *b){(void)b;}
#define PARSER(n) static int n(struct ufs_hba *b){(void)b;return 0;}
PARSER(ufshcd_parse_clock_info)
PARSER(ufshcd_parse_regulator_info)
PARSER(ufshcd_parse_reset_info)
PARSER(ufshcd_parse_pinctrl_info)
PARSER(ufshcd_parse_dev_ref_clk_freq)
PARSER(ufshcd_parse_pm_levels)
PARSER(ufshcd_parse_gear_limits)
PARSER(ufshcd_parse_cmd_timeout)
PARSER(ufshcd_parse_extcon_info)
static int pcim_enable_device(struct pci_dev *p){(void)p;return 0;}
static void pci_set_master(struct pci_dev *p){(void)p;}
static int pcim_iomap_regions(struct pci_dev *p,int a,const char *s){(void)p;(void)a;(void)s;return 0;}
static void **pcim_iomap_table(struct pci_dev *p){(void)p;static void *a[]={(void *)1};return a;}
/* ACTUAL_SOURCE */
static void reset(void){memset(&h,0,sizeof(h));memset(trace,0,sizeof(trace));locked=stage=publishes=scheduled=freed=synced=removed=cancelled=stopped=enabled=pmcalls=gets=0;}
int main(void){int cases=0;
 /* caller 0: platform, PM disabled until probe returns; caller 1: PCI,
  * PM enabled, with the PCI core's existing usage reference; caller 2: !PM. */
 for(int caller=0;caller<3;caller++)for(int failure=0;failure<5;failure++)for(inject_at=0;inject_at<5;inject_at++){
 reset();irqfail=failure==1;addfail=failure==2;enablefail=failure==3;workfail=failure==4;
 struct platform_device platform={.dev={.disabled=1,.no_pm=caller==2}};
 struct pci_dev pci={.dev={.usage=1},.irq=1};
 struct device *d=caller==1?&pci.dev:&platform.dev;
 int ret=caller==1?ufshcd_pci_probe(&pci,NULL):ufshcd_pltfrm_init(&platform,NULL);
 bool reached_injection=(inject_at==1)||(inject_at==2&&!irqfail&&!addfail)||
  ((inject_at==3||inject_at==4)&&!failure);
 bool fail=failure||(inject_at&&reached_injection);
 if(!fail){
  assert(ret==0&&h.init_lifecycle==UFSHCD_LIVE&&publishes==1&&scheduled==1);
  assert(gets==1&&strchr(trace,'N')&&!strchr(trace,'G'));
  assert(d->usage==(caller==2?0:1));
  if(caller!=1){assert(strstr(trace,"TAE"));assert(d->enabled);}
  else assert(strstr(trace,"TPL"));
  /* Async scan owns the core reference, separately tested by scan suite. */
  pm_runtime_put_noidle(d);assert(!d->usage);
 }else{
  assert(ret<0&&!publishes&&!scheduled);assert(h.init_lifecycle==UFSHCD_STOPPING);
  assert(d->usage==(caller==1?1:0));assert(cancelled==4&&stopped==1);
  assert(synced==!irqfail&&freed==!irqfail&&!h.init_irq_owned);
  if(!irqfail)assert(strstr(trace,"YFCCCC"));
  assert(!strchr(trace,'E')&&!strchr(trace,'L'));
 }
 assert(!locked);cases++;
 }
 printf("PASS: %d real init-suffix + complete platform/PCI caller executions; disabled-PM/PCI/!PM, IRQ injections, failure ownership and PM balance\n",cases);
 return 0;
}
