/* Observation only: no skb contents, firmware descriptors or policy changes. */
#ifndef AIC_WIFI_DIAG_H
#define AIC_WIFI_DIAG_H
#include <linux/types.h>
struct sk_buff;
struct dentry;
enum wd_stage {
 WD_TX_ENTER, WD_TX_COPY, WD_TX_DROP_PREP, WD_TX_DROP_FREE,
 WD_SDIO_ENQUEUE, WD_SDIO_QUEUE_FULL, WD_SDIO_BUS_DOWN, WD_SDIO_DEQUEUE,
 WD_AGGR_BEGIN, WD_AGGR_ADD, WD_AGGR_ABORT, WD_SDIO_WRITE_BEGIN,
 WD_SDIO_WRITE_END, WD_SDIO_RX, WD_RX_DECODE, WD_RX_UP,
 WD_RX_NO_DESC, WD_RX_BAD_TID, WD_RX_REORDER_DROP,
 WD_CONNECT_REQ, WD_ASSOC_RESULT, WD_DISCONNECT_IND, WD_CFG_DISCONNECT,
 WD_STA_TEARDOWN, WD_TXQ_FLUSH, WD_MAP_MISS, WD_BATCH_OVERFLOW,
 WD_START, WD_FREEZE, WD_RX_NO_VIF, WD_HOST_DISCONNECT_REQ,
 WD_KEY_ADD_REQ, WD_KEY_DEL_REQ, WD_CONTROL_PORT_REQ, WD_PS_MODE_REQ,
 WD_LP_LEVEL_REQ, WD_CMD_ENTER, WD_CMD_QUEUE_RETURN, WD_CMD_QUEUED,
 WD_CMD_CFM, WD_CMD_TIMEOUT, WD_CMD_BUS_DOWN, WD_RX_VLAN_MISS, WD_MAX
};
struct wd_packet {
 u64 id, assoc, born_ns, enqueue_ns, replay;
 u32 ifindex;
 u16 len, key_info;
 u8 src[6], dst[6], type, valid;
};
void wd_init(struct dentry *parent);
void wd_exit(void);
void wd_tx_eth(enum wd_stage stage, struct sk_buff *skb, int ifindex);
void wd_tx_copy(struct sk_buff *old, struct sk_buff *new);
void wd_tx_host(enum wd_stage stage, struct sk_buff *skb, int qlen, int qmax, int ret, bool finish);
void wd_take(struct sk_buff *skb, struct wd_packet *packet);
void wd_batch_add(void *owner, const struct wd_packet *packet);
void wd_batch_io(void *owner, enum wd_stage stage, int ret, int bytes);
void wd_batch_reset(void *owner);
void wd_rx(enum wd_stage stage, struct sk_buff *skb, int ifindex, int detail, bool finish);
void wd_bus_rx(int ret, int bytes);
void wd_conn(enum wd_stage stage, int ifindex, const u8 *bssid, int freq, int reason, int flags);
void wd_cmd(enum wd_stage stage, unsigned int reqid, unsigned int token, int result, int detail);
void wd_cmd_cfm(unsigned int reqid, unsigned int token, const void *param, unsigned int len);
#endif
