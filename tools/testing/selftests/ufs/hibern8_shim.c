/* Host stubs and assertions; kernel functions inserted at marker. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define UFSHCD_LIVE 1
#define UIC_HIBERN8_ENTER_RETRIES 3
#define READ_ONCE(x) (x)
#define UIC_CMD_DME_HIBER_ENTER 4
#define UIC_CMD_DME_HIBER_EXIT 5
#define UFS_ERR_HIBERN8_ENTER 6
#define UFS_ERR_HIBERN8_EXIT 7
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define trace_ufshcd_profile_hibern8(...) ((void)0)
typedef int ktime_t;
static int ktime_get(void){return 0;}
struct uic_command {int command;};
struct ufs_hba {int init_lifecycle;bool full_init_linereset;void *dev;struct {int last_hibern8_exit_tstamp,hibern8_exit_cnt;} ufs_stats;};
static int injected, recovery, calls, bugs;
#define BUG() do {bugs++;} while(0)
static int ufshcd_uic_pwr_ctrl(struct ufs_hba *h,struct uic_command *c){(void)h;(void)c;calls++;return injected;}
static void ufshcd_update_error_stats(struct ufs_hba *h,int e){(void)h;(void)e;}
static int ufshcd_link_recovery(struct ufs_hba *h){return h->init_lifecycle==UFSHCD_LIVE?recovery:-ENOLINK;}

/* ACTUAL_SOURCE */

int main(void){
 for(int phase=0;phase<3;phase++) for(int error=0;error<2;error++) for(int lr=0;lr<2;lr++) {
 struct ufs_hba h={.init_lifecycle=phase,.full_init_linereset=lr};
 injected=error?-EIO:0;recovery=-ENOLINK;calls=bugs=0;
 int ret=ufshcd_uic_hibern8_enter(&h);
 if(phase!=UFSHCD_LIVE) {assert(!bugs);assert(ret==((error||lr)?-ENOLINK:0));assert(calls==1);}
 else if(error)assert(bugs==3);else if(lr)assert(bugs==1);else assert(!ret&&!bugs);
 h.full_init_linereset=false;calls=bugs=0;
 ret=ufshcd_uic_hibern8_exit(&h);
 if(phase!=UFSHCD_LIVE){assert(!bugs);assert(ret==(error?-ENOLINK:0));}
 else if(error)assert(bugs==1);else assert(!ret&&!bugs);
 }
 puts("PASS: 12 extracted hibern8 enter + 12 exit executions; INIT LINERESET-only fails, LIVE BUG policy preserved");return 0;
}
