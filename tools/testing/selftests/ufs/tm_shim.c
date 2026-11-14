#include <assert.h>
#include <errno.h>
#include <stdio.h>
#define TM_CMD_SEND 1
struct Scsi_Host {int unused;};
struct ufs_hba {struct Scsi_Host *host;int tm_tag_wq;struct {struct {int ctx;} clk_hold;} ufs_stats;};
static int slots,wakes,injected;
static int ufshcd_get_tm_free_slot(struct ufs_hba *h,int *slot){(void)h;*slot=3;slots++;return 1;}
#define wait_event(wq,cond) do {(void)(wq);assert(cond);}while(0)
static int ufshcd_hold_all(struct ufs_hba *h){(void)h;return injected;}
static void ufshcd_put_tm_slot(struct ufs_hba *h,int slot){(void)h;assert(slot==3);slots--;}
static void wake_up(int *w){(void)w;wakes++;}
static int actual_tm_prefix(struct ufs_hba *hba){struct Scsi_Host *host;int free_slot,err;

/* ACTUAL_SOURCE */

(void)host;ufshcd_put_tm_slot(hba,free_slot);return 0;}
int main(void){struct ufs_hba h={0};injected=-EIO;assert(actual_tm_prefix(&h)==-EIO);assert(slots==0&&wakes==1);injected=0;assert(actual_tm_prefix(&h)==0);assert(!slots);puts("PASS: actual TM acquisition/hold-failure prefix releases slot and wakes waiter");}
