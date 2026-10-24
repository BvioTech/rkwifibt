/* Bounded, disabled-by-default diagnostic recorder. Not a recovery mechanism. */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <asm/unaligned.h>
#include "rwnx_defs.h"
#include "rwnx_tx.h"
#include "wifi_diag.h"

#define WD_CAP 8192
#define WD_MAP 512
#define WD_BATCH 128
#define WD_BUILD "20261021-d2"
MODULE_INFO(wifi_diag, WD_BUILD);
static const char * const names[WD_MAX] = {
 "TX_ENTER", "TX_COPY", "TX_DROP_PREP", "TX_DROP_FREE",
 "SDIO_ENQUEUE", "SDIO_QUEUE_FULL", "SDIO_BUS_DOWN", "SDIO_DEQUEUE",
 "AGGR_BEGIN", "AGGR_ADD", "AGGR_ABORT", "SDIO_WRITE_BEGIN",
 "SDIO_WRITE_END", "SDIO_RX", "RX_DECODE", "RX_UP",
 "RX_NO_DESC", "RX_BAD_TID", "RX_REORDER_DROP",
 "CONNECT_REQ", "ASSOC_RESULT", "DISCONNECT_IND", "CFG_DISCONNECT",
 "STA_TEARDOWN", "TXQ_FLUSH", "MAP_MISS", "BATCH_OVERFLOW",
 "START", "FREEZE", "RX_NO_VIF", "HOST_DISCONNECT_REQ",
 "KEY_ADD_REQ", "KEY_DEL_REQ", "CONTROL_PORT_REQ", "PS_MODE_REQ",
 "LP_LEVEL_REQ", "CMD_ENTER", "CMD_QUEUE_RETURN", "CMD_QUEUED",
 "CMD_CFM", "CMD_TIMEOUT", "CMD_BUS_DOWN", "RX_VLAN_MISS"
};
struct wd_event {
 struct wd_packet p;
 u64 seq, ns, batch;
 s32 stage, ret, qlen, qmax;
 u32 freq;
 u8 bssid[6];
};
struct wd_map { const struct sk_buff *key; struct wd_packet p; };
struct wd_batch { void *owner; u64 id; unsigned int nr; bool ended; struct wd_packet p[WD_BATCH]; };
static DEFINE_SPINLOCK(wd_lock);
static DEFINE_MUTEX(wd_control_lock);
static struct {
 bool enabled;
 u32 ifindex, freq, highwater;
 u8 bssid[6];
 u64 seq, packet, assoc, batch_seq, trigger_ns, overwrite, map_lost, batch_lost;
 u64 last_disconnect_ns;
 int last_disconnect_reason;
 struct wd_event *ring;
 struct wd_map map[WD_MAP];
 struct wd_batch batch[2];
 atomic64_t count[WD_MAX];
 atomic64_t recorded[WD_MAX];
} d;

static bool active(void) { return READ_ONCE(d.enabled); }
static bool target(int index) { return !index || index == READ_ONCE(d.ifindex); }
static void append(enum wd_stage stage, const struct wd_packet *p, int ret, int qlen, int qmax, u64 batch)
{
 struct wd_event *e;
 if (!d.enabled || !d.ring) return;
 atomic64_inc(&d.recorded[stage]);
 e = &d.ring[d.seq % WD_CAP];
 memset(e, 0, sizeof(*e));
 if (p) e->p = *p;
 e->seq = ++d.seq; e->ns = ktime_get_ns(); e->stage = stage;
 e->ret = ret; e->qlen = qlen; e->qmax = qmax; e->batch = batch;
 e->freq = d.freq; ether_addr_copy(e->bssid, d.bssid);
 if (d.seq > WD_CAP) d.overwrite++;
}
static struct wd_map *lookup(const struct sk_buff *skb)
{
 unsigned int i;
 for (i = 0; i < WD_MAP; i++) if (d.map[i].key == skb) return &d.map[i];
 return NULL;
}
static struct wd_map *remember(const struct sk_buff *skb, struct wd_packet *p, bool expected_new)
{
 struct wd_map *m = lookup(skb);
 unsigned int i;
 if (m && expected_new) { d.map_lost++; m->key = NULL; m = NULL; }
 if (m) return m;
 p->id = ++d.packet; p->assoc = d.assoc; p->born_ns = ktime_get_ns();
 if (!expected_new) { d.map_lost++; append(WD_MAP_MISS, p, -ENOENT, 0, 0, 0); }
 for (i = 0; i < WD_MAP; i++) if (!d.map[i].key) {
  d.map[i].key = skb; d.map[i].p = *p; return &d.map[i];
 }
 d.map_lost++; append(WD_MAP_MISS, p, -ENOSPC, 0, 0, 0); return NULL;
}
/* Never pulls/linearizes or writes skb. Only control-header fields are copied. */
static bool parse(struct sk_buff *skb, int offset, int index, struct wd_packet *p, bool ethernet)
{
 u8 h[ETH_HLEN + 17];
 int n = ethernet ? ETH_HLEN : 0;
 memset(p, 0, sizeof(*p));
 if (skb_copy_bits(skb, offset, h, n + 4)) return false;
 if (ethernet && get_unaligned_be16(h + 12) != ETH_P_PAE) return false;
 p->valid = 1; p->ifindex = index; p->len = min_t(unsigned int, skb->len, 65535);
 if (ethernet) { ether_addr_copy(p->dst, h); ether_addr_copy(p->src, h + 6); }
 p->type = h[n + 1];
 if (p->type == 3 && !skb_copy_bits(skb, offset, h, n + 17)) {
  p->key_info = get_unaligned_be16(h + n + 5);
  p->replay = get_unaligned_be64(h + n + 9);
 }
 return true;
}
void wd_tx_eth(enum wd_stage stage, struct sk_buff *skb, int index)
{
 struct wd_packet p; struct wd_map *m; unsigned long flags;
 if (!active() || !target(index)) return;
 atomic64_inc(&d.count[stage]);
 if (stage != WD_TX_ENTER) {
  spin_lock_irqsave(&wd_lock, flags);
  m = lookup(skb);
  if (d.enabled && m) { append(stage, &m->p, 0, 0, 0, 0); m->key = NULL; }
  spin_unlock_irqrestore(&wd_lock, flags);
  return;
 }
 if (!parse(skb, 0, index, &p, true)) return;
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled) {
  m = remember(skb, &p, stage == WD_TX_ENTER);
  append(stage, m ? &m->p : &p, 0, 0, 0, 0);
  if (m && stage != WD_TX_ENTER) m->key = NULL;
 }
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_tx_copy(struct sk_buff *old, struct sk_buff *new)
{
 struct wd_map *m; unsigned long flags;
 if (!active()) return;
 spin_lock_irqsave(&wd_lock, flags);
 m = lookup(old);
 if (d.enabled && m) { m->key = new; append(WD_TX_COPY, &m->p, 0, 0, 0, 0); }
 spin_unlock_irqrestore(&wd_lock, flags);
}
/* The queued skb owns sw_hdr, but not the originating VIF/netdev lifetime. */
static bool host_eapol(struct sk_buff *skb)
{
 struct rwnx_txhdr *h;
 if (!skb || skb->len < sizeof(*h)) return false;
 h = (struct rwnx_txhdr *)skb->data;
 return h->sw_hdr && h->sw_hdr->desc.host.ethertype == htons(ETH_P_PAE);
}
void wd_tx_host(enum wd_stage stage, struct sk_buff *skb, int qlen, int qmax, int ret, bool finish)
{
 struct wd_map *m; unsigned long flags;
 if (!active()) return;
 atomic64_inc(&d.count[stage]);
 /* Queue depth is passed while the original queue lock is already held. */
 if ((stage == WD_SDIO_ENQUEUE || stage == WD_SDIO_QUEUE_FULL) && qlen > READ_ONCE(d.highwater)) {
  spin_lock_irqsave(&wd_lock, flags);
  if (d.enabled && qlen > d.highwater) d.highwater = qlen;
  spin_unlock_irqrestore(&wd_lock, flags);
 }
 if (!host_eapol(skb)) return;
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled) {
  m = lookup(skb);
  /* Identity was captured while the TX_ENTER VIF was live. Never recover it
   * through sw_hdr->rwnx_vif after the packet has been queued.
   */
  if (!m) {
   d.map_lost++; append(WD_MAP_MISS, NULL, -ENOENT, 0, 0, 0);
   spin_unlock_irqrestore(&wd_lock, flags);
   return;
  }
  if (stage == WD_SDIO_ENQUEUE) m->p.enqueue_ns = ktime_get_ns();
  append(stage, &m->p, ret, qlen, qmax, 0);
  if (finish) m->key = NULL;
 }
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_take(struct sk_buff *skb, struct wd_packet *p)
{
 struct wd_map *m; unsigned long flags;
 p->valid = 0;
 if (!active() || !host_eapol(skb)) return;
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled) {
  m = lookup(skb);
  if (m) {
   *p = m->p; m->key = NULL;
   append(WD_AGGR_BEGIN, p, 0, 0, 0, 0);
  } else {
   d.map_lost++; append(WD_MAP_MISS, NULL, -ENOENT, 0, 0, 0);
  }
 } else p->valid = 0;
 spin_unlock_irqrestore(&wd_lock, flags);
}
static struct wd_batch *batch(void *owner, bool create)
{
 unsigned int i;
 for (i = 0; i < ARRAY_SIZE(d.batch); i++) if (d.batch[i].owner == owner) return &d.batch[i];
 if (create) for (i = 0; i < ARRAY_SIZE(d.batch); i++) if (!d.batch[i].owner) {
  d.batch[i].owner = owner; d.batch[i].id = ++d.batch_seq; return &d.batch[i];
 }
 return NULL;
}
void wd_batch_add(void *owner, const struct wd_packet *p)
{
 struct wd_batch *b; unsigned long flags;
 if (!active() || !p->valid) return;
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled) {
  b = batch(owner, true);
  if (b && b->nr < WD_BATCH) { b->p[b->nr++] = *p; append(WD_AGGR_ADD, p, 0, 0, 0, b->id); }
  else { d.batch_lost++; append(WD_BATCH_OVERFLOW, p, -ENOSPC, 0, 0, 0); }
 }
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_batch_io(void *owner, enum wd_stage stage, int ret, int bytes)
{
 struct wd_batch *b; unsigned int i; unsigned long flags;
 if (!active()) return;
 atomic64_inc(&d.count[stage]);
 spin_lock_irqsave(&wd_lock, flags);
 b = batch(owner, false);
 if (d.enabled && b) {
  for (i = 0; i < b->nr; i++) append(stage, &b->p[i], ret, bytes, 0, b->id);
  if (stage == WD_SDIO_WRITE_END) b->ended = true;
 } else if (d.enabled && ret) append(stage, NULL, ret, bytes, 0, 0);
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_batch_reset(void *owner)
{
 struct wd_batch *b; unsigned int i; unsigned long flags;
 if (!active()) return;
 spin_lock_irqsave(&wd_lock, flags);
 b = batch(owner, false);
 if (b) {
  if (d.enabled && !b->ended) for (i = 0; i < b->nr; i++) append(WD_AGGR_ABORT, &b->p[i], -ECANCELED, 0, 0, b->id);
  memset(b, 0, sizeof(*b));
 }
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_rx(enum wd_stage stage, struct sk_buff *skb, int index, int detail, bool finish)
{
 struct wd_packet p; struct wd_map *m; unsigned long flags;
 if (!active() || !target(index)) return;
 atomic64_inc(&d.count[stage]);
 if (!skb || !parse(skb, 0, index, &p, true)) return;
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled) {
  m = remember(skb, &p, stage == WD_RX_DECODE || stage == WD_RX_NO_VIF);
  append(stage, m ? &m->p : &p, detail, 0, 0, 0);
  if (m && finish) m->key = NULL;
 }
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_bus_rx(int ret, int bytes)
{
 unsigned long flags;
 if (!active()) return;
 atomic64_inc(&d.count[WD_SDIO_RX]);
 if (!ret) return;
 spin_lock_irqsave(&wd_lock, flags);
 append(WD_SDIO_RX, NULL, ret, bytes, 0, 0);
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_conn(enum wd_stage stage, int index, const u8 *bssid, int freq, int reason, int details)
{
 struct wd_packet p = { .ifindex = index }; unsigned long flags;
 if (!active() || !target(index)) return;
 atomic64_inc(&d.count[stage]);
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled) {
  if (stage == WD_CONNECT_REQ) d.assoc++;
  if (bssid) ether_addr_copy(d.bssid, bssid);
  if (freq) d.freq = freq;
  p.assoc = d.assoc;
  append(stage, &p, reason, details, 0, 0);
  if (stage == WD_DISCONNECT_IND) {
   d.last_disconnect_ns = ktime_get_ns(); d.last_disconnect_reason = reason;
   if (!d.trigger_ns) d.trigger_ns = d.last_disconnect_ns;
  }
 }
 spin_unlock_irqrestore(&wd_lock, flags);
}

/* Low-rate control-path observation only. No command payload or key material. */
static bool watched_command(unsigned int reqid)
{
 return reqid == SM_CONNECT_CFM || reqid == SM_DISCONNECT_CFM ||
  reqid == MM_KEY_ADD_CFM || reqid == MM_KEY_DEL_CFM ||
  reqid == ME_SET_CONTROL_PORT_CFM || reqid == ME_SET_PS_MODE_CFM ||
  reqid == ME_SET_LP_LEVEL_CFM;
}
void wd_cmd(enum wd_stage stage, unsigned int reqid, unsigned int token, int result, int detail)
{
 struct wd_packet p = { 0 }; unsigned long flags;
 if (!active() || !watched_command(reqid)) return;
 atomic64_inc(&d.count[stage]);
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled) {
  p.assoc = d.assoc; p.ifindex = d.ifindex;
  append(stage, &p, result, reqid, detail, token);
 }
 spin_unlock_irqrestore(&wd_lock, flags);
}
void wd_cmd_cfm(unsigned int reqid, unsigned int token, const void *param, unsigned int len)
{
 int status = -1;
 if (!active() || !watched_command(reqid)) return;
 /* Only these two replies have a verified first-byte status field. */
 if ((reqid == MM_KEY_ADD_CFM || reqid == SM_CONNECT_CFM) && param && len)
  status = *(const u8 *)param;
 wd_cmd(WD_CMD_CFM, reqid, token, status, len);
}

struct wd_snapshot { u64 nr; struct wd_event events[]; };
static int events_show(struct seq_file *s, void *unused)
{
 struct wd_snapshot *snap = s->private; u64 i;
 seq_puts(s, "seq,mono_ns,assoc,packet_id,stage,ifindex,src,dst,eapol_type,key_info,replay,len,enqueue_ns,born_ns,batch,ret,qlen,qmax,bssid,freq\n");
 for (i = 0; i < snap->nr; i++) {
  struct wd_event *e = &snap->events[i]; struct wd_packet *p = &e->p;
  seq_printf(s, "%llu,%llu,%llu,%llu,%s,%u,%pM,%pM,%u,%04x,%016llx,%u,%llu,%llu,%llu,%d,%d,%d,%pM,%u\n",
   e->seq, e->ns, p->assoc, p->id, names[e->stage], p->ifindex, p->src, p->dst,
   p->type, p->key_info, p->replay, p->len, p->enqueue_ns, p->born_ns, e->batch,
   e->ret, e->qlen, e->qmax, e->bssid, e->freq);
 }
 return 0;
}
static int events_open(struct inode *inode, struct file *f)
{
 struct wd_snapshot *snap; unsigned long flags; u64 i, first; int ret;
 if (active()) return -EBUSY;
 snap = kvzalloc(struct_size(snap, events, WD_CAP), GFP_KERNEL);
 if (!snap) return -ENOMEM;
 spin_lock_irqsave(&wd_lock, flags);
 if (d.enabled || !d.ring) { spin_unlock_irqrestore(&wd_lock, flags); kvfree(snap); return -EBUSY; }
 snap->nr = min_t(u64, d.seq, WD_CAP); first = d.seq - snap->nr;
 for (i = 0; i < snap->nr; i++) snap->events[i] = d.ring[(first + i) % WD_CAP];
 spin_unlock_irqrestore(&wd_lock, flags);
 ret = single_open(f, events_show, snap);
 if (ret) kvfree(snap);
 return ret;
}
static int events_release(struct inode *inode, struct file *f)
{
 struct seq_file *s = f->private_data; kvfree(s->private); return single_release(inode, f);
}
static const struct file_operations events_ops = { .owner = THIS_MODULE, .open = events_open, .read = seq_read, .llseek = seq_lseek, .release = events_release };
static int status_show(struct seq_file *s, void *unused)
{
 unsigned long flags; unsigned int i; u64 oldest = 0;
 spin_lock_irqsave(&wd_lock, flags);
 if (d.ring && d.seq) oldest = d.ring[(d.seq > WD_CAP ? d.seq - WD_CAP : 0) % WD_CAP].ns;
 seq_printf(s, "build=%s\nenabled=%u\nifindex=%u\nseq=%llu\nassoc=%llu\ntrigger_ns=%llu\noldest_ns=%llu\nring_overwrite=%llu\nmap_lost=%llu\nbatch_lost=%llu\nqueue_highwater=%u\ncapacity=%u\n",
  WD_BUILD, d.enabled, d.ifindex, d.seq, d.assoc, d.trigger_ns, oldest, d.overwrite, d.map_lost, d.batch_lost, d.highwater, WD_CAP);
 seq_printf(s, "last_disconnect_ns=%llu\nlast_disconnect_reason=%d\n", d.last_disconnect_ns, d.last_disconnect_reason);
 spin_unlock_irqrestore(&wd_lock, flags);
 for (i = 0; i < WD_MAX; i++) {
  seq_printf(s, "hook_calls.%s=%lld\n", names[i], atomic64_read(&d.count[i]));
  seq_printf(s, "record.%s=%lld\n", names[i], atomic64_read(&d.recorded[i]));
 }
 return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);
static ssize_t control_write(struct file *f, const char __user *user, size_t len, loff_t *pos)
{
 char text[48]; unsigned int index, i; unsigned long flags; int ret = 0;
 if (!len || len >= sizeof(text)) return -EINVAL;
 if (copy_from_user(text, user, len)) return -EFAULT;
 text[len] = 0;
 mutex_lock(&wd_control_lock);
 spin_lock_irqsave(&wd_lock, flags);
 if (!d.ring) ret = -ENODEV;
 else if (sscanf(text, "start %u", &index) == 1 && index) {
  if (d.enabled) ret = -EBUSY;
  else {
   memset(d.map, 0, sizeof(d.map)); memset(d.batch, 0, sizeof(d.batch));
   d.seq = d.packet = d.assoc = d.batch_seq = d.trigger_ns = 0;
   d.last_disconnect_ns = 0; d.last_disconnect_reason = 0;
   d.overwrite = d.map_lost = d.batch_lost = d.highwater = d.freq = 0;
   eth_zero_addr(d.bssid); d.ifindex = index;
   for (i = 0; i < WD_MAX; i++) { atomic64_set(&d.count[i], 0); atomic64_set(&d.recorded[i], 0); }
   d.enabled = true; append(WD_START, NULL, 0, 0, 0, 0);
  }
 } else if (sysfs_streq(text, "freeze")) {
  append(WD_FREEZE, NULL, 0, 0, 0, 0); d.enabled = false;
 } else ret = -EINVAL;
 spin_unlock_irqrestore(&wd_lock, flags);
 mutex_unlock(&wd_control_lock);
 return ret ? ret : len;
}
static const struct file_operations control_ops = { .owner = THIS_MODULE, .write = control_write, .llseek = no_llseek };
void wd_init(struct dentry *parent)
{
 struct dentry *dir;
 if (d.ring) return;
 d.ring = kvcalloc(WD_CAP, sizeof(*d.ring), GFP_KERNEL);
 if (!d.ring) { pr_warn("aic wifi_diag allocation failed; data path unchanged\n"); return; }
 dir = debugfs_create_dir("wifi_diag", parent);
 if (IS_ERR_OR_NULL(dir)) { kvfree(d.ring); d.ring = NULL; return; }
 debugfs_create_file("status", 0400, dir, NULL, &status_fops);
 debugfs_create_file("events", 0400, dir, NULL, &events_ops);
 debugfs_create_file("control", 0200, dir, NULL, &control_ops);
}
void wd_exit(void)
{
 struct wd_event *ring; unsigned long flags;
 mutex_lock(&wd_control_lock);
 spin_lock_irqsave(&wd_lock, flags);
 d.enabled = false; ring = d.ring; d.ring = NULL;
 spin_unlock_irqrestore(&wd_lock, flags);
 kvfree(ring); mutex_unlock(&wd_control_lock);
}
