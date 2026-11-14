/* Host stubs and assertions; kernel functions inserted at marker. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#define UFSHCD_INIT 0
#define UFSHCD_LIVE 1
#define UFSHCD_STOPPING 2
#define UFSHCD_INIT_EH 1u
#define UFSHCD_INIT_EEH 2u
#define UFSHCD_INIT_RLS 4u
struct work_struct { int calls; bool pending; };
struct ufs_hba { int init_lifecycle; unsigned scan_pending; bool init_failed,init_scan_finished,init_scan_ready; unsigned saved_err,saved_uic_err,saved_ce_err;bool force_host_reset,auto_h8_err;void *dev;struct work_struct eh_work,eeh_work,rls_work;};
static int votes,clocks,h8,fail_clock,fail_h8;
static bool schedule_work(struct work_struct *w){if(w->pending)return false;w->calls++;w->pending=true;return true;}
static void pm_runtime_get_noresume(void *d){(void)d;votes++;}
static int ufshcd_hold(struct ufs_hba *h,bool a){(void)h;(void)a;if(fail_clock)return -EIO;clocks++;return 0;}
static int ufshcd_hibern8_hold(struct ufs_hba *h,bool a){(void)h;(void)a;if(fail_h8)return -ENOLINK;h8++;return 0;}
static void ufshcd_release(struct ufs_hba *h,bool a){(void)h;(void)a;clocks--;}

/* ACTUAL_SOURCE */

int main(void){int cases=0;
 {struct ufs_hba h={0}; ufshcd_queue_recovery(&h,&h.eh_work,1);
 h.saved_err=h.saved_uic_err=h.saved_ce_err=0;h.force_host_reset=h.auto_h8_err=false;
 assert(h.init_failed);assert(ufshcd_init_handoff(&h,0)==-EIO);assert(h.init_failed);
 assert(ufshcd_init_handoff(&h,0)==-EIO);}

 for(int events=0;events<8;events++)for(int fatal=0;fatal<64;fatal++){
 struct ufs_hba h={0};votes=0;
 if(events&1)ufshcd_queue_recovery(&h,&h.eh_work,1);
 if(events&2)ufshcd_queue_recovery(&h,&h.eeh_work,2);
 if(events&4)ufshcd_queue_recovery(&h,&h.rls_work,4);
 assert(!h.eh_work.calls&&!h.eeh_work.calls&&!h.rls_work.calls&&!votes);
 h.saved_err=!!(fatal&1);h.saved_uic_err=!!(fatal&2);h.saved_ce_err=!!(fatal&4);h.force_host_reset=!!(fatal&8);h.auto_h8_err=!!(fatal&16);
 int ret=ufshcd_init_handoff(&h,(fatal&32)?-ENOMEM:0);
 if(fatal||(events&1)){assert(ret<0);assert(h.init_lifecycle==UFSHCD_INIT);}
 else{
 assert(!ret&&h.init_lifecycle==UFSHCD_LIVE);
 assert(ufshcd_queue_recovery(&h,&h.eh_work,1)); /* async EH is not gated */
 ufshcd_scan_handoff(&h,0);
 assert(h.init_scan_ready&&h.init_scan_finished&&!h.scan_pending);
 assert(h.rls_work.calls==!!(events&4));assert(h.eeh_work.calls==!!(events&2));assert(votes==!!(events&2));
 }cases++;
 }
 {struct ufs_hba h={.init_lifecycle=UFSHCD_LIVE};votes=0;
 ufshcd_queue_recovery(&h,&h.eeh_work,2);ufshcd_queue_recovery(&h,&h.rls_work,4);
 ufshcd_scan_handoff(&h,-EIO);assert(!h.scan_pending&&!h.init_scan_ready&&h.init_scan_finished);
 assert(!votes&&!h.rls_work.calls&&!h.eeh_work.calls);
 assert(!ufshcd_queue_recovery(&h,&h.rls_work,4));assert(ufshcd_queue_recovery(&h,&h.eh_work,1));
 h.init_lifecycle=UFSHCD_STOPPING;assert(!ufshcd_queue_recovery(&h,&h.eeh_work,2));}
 for(fail_clock=0;fail_clock<2;fail_clock++)for(fail_h8=0;fail_h8<2;fail_h8++){
 struct ufs_hba h={0};clocks=h8=0;int ret=ufshcd_hold_all(&h);
 if(fail_clock||fail_h8){assert(ret<0);assert(!clocks&&!h8);}else assert(!ret&&clocks==1&&h8==1);
 }
 printf("PASS: %d sync-fatal/handoff cases, independent scan success/failure, async EH preserved, four hold unwind cases\n",cases);return 0;}
