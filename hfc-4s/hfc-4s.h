/*
 *
 *
 * Copyright (C) 2004 Daniele Orlandi
 *
 * Daniele "Vihai" Orlandi <daniele@orlandi.com> 
 *
 * This program is free software and may be modified and
 * distributed under the terms of the GNU Public License.
 *
 */

#ifndef _HFC_4S_H
#define _HFC_4S_H

#define hfc_DRIVER_NAME "hfc-4s"
#define hfc_DRIVER_PREFIX hfc_DRIVER_NAME ": "
#define hfc_DRIVER_DESCR "HFC-4S HFC-8S Driver"

#define hfc_MAX_BOARDS 32

#ifndef PCI_DMA_32BIT
#define PCI_DMA_32BIT	0x00000000ffffffffULL
#endif

#ifndef PCI_DEVICE_ID_CCD_08B4
#define PCI_DEVICE_ID_CCD_08B4		0x08b4
#endif

#ifndef PCI_DEVICE_ID_CCD_16B8
#define PCI_DEVICE_ID_CCD_16B8		0x16B8
#endif

#define hfc_CLKDEL_TE	0x0E
#define hfc_CLKDEL_NT	0x0C

#define hfc_PCI_MEM_SIZE	0x1000

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

struct hfc_chan;

struct hfc_chan_simplex {
	struct hfc_chan_duplex *chan;

	void *z1_base,*z2_base;
	void *fifo_base;
	void *z_base;
	u16 z_min;
	u16 z_max;
	u16 fifo_size;

	u8 *f1,*f2;
	u8 f_min;
	u8 f_max;
	u8 f_num;

	unsigned long long frames;
	unsigned long long bytes;
	unsigned long long fifo_full;
	unsigned long long crc;
};

enum hfc_chan_status {
	free,
	open_framed,
	open_voice,
	sniff_aux,
	loopback,
};

struct hfc_port;
struct hfc_chan_duplex {
	struct hfc_port *port;

	char *name;
	int number;

	int fifo_id;

	enum hfc_chan_status status;

	unsigned short protocol;

	spinlock_t lock;

	struct hfc_chan_simplex rx;
	struct hfc_chan_simplex tx;

	struct net_device *netdev;
	struct net_device_stats net_device_stats;
};

struct hfc_port
{
	struct hfc_card *card;

	int port_id;

	struct hfc_chan_duplex chans[4];
	int echo_enabled;
	int nt_mode;
	u8 l1_state;
};

enum hfc_chip_type
{
	HFC_CHIPTYPE_4S,
	HFC_CHIPTYPE_8S,
};

typedef struct hfc_card {
	int card_id;
	spinlock_t lock;

	int ticks;

	struct pci_dev *pcidev;

	struct proc_dir_entry *proc_dir;
	char proc_dir_name[32];

	struct proc_dir_entry *proc_info;
	struct proc_dir_entry *proc_fifos;

	enum hfc_chip_type chip_type;
	int num_ports;
	struct hfc_port ports[8];

	unsigned long io_bus_mem;
	void *io_mem;

	int sync_loss_reported;
	int late_irqs;

	int ignore_first_timer_interrupt;

	struct {
		u8 m1;
		u8 m2;
		u8 trm;
		u8 connect;
		u8 sctrl;
		u8 sctrl_r;
		u8 sctrl_e;
		u8 ctmt;
		u8 cirm;
	} regs;

	int debug_event;
} hfc_card;

static inline u8 hfc_inb(struct hfc_card *card, int offset)
{
 return readb(card->io_mem + offset);
}

static inline void hfc_outb(struct hfc_card *card, int offset, u8 value)
{
 writeb(value, card->io_mem + offset);
}

static inline u16 hfc_inw(struct hfc_card *card, int offset)
{
 return readw(card->io_mem + offset);
}

static inline void hfc_outw(struct hfc_card *card, int offset, u16 value)
{
 writew(value, card->io_mem + offset);
}

static inline u32 hfc_inl(struct hfc_card *card, int offset)
{
 return readl(card->io_mem + offset);
}

static inline void hfc_outl(struct hfc_card *card, int offset, u32 value)
{
 writel(value, card->io_mem + offset);
}

extern int debug_level;


#define hfc_R_CIRM		0x00
#define hfc_R_CTRL		0x01
#define hfc_R_BRG_PCM_CFG	0x02
#define hfc_R_RAM_ADDR0		0x08
#define hfc_R_RAM_ADDR1		0x09
#define hfc_R_RAM_ADDR2		0x0A
#define hfc_R_FIRST_FIFO	0x0B
#define hfc_R_RAM_MISC		0x0C
#define hfc_R_FIFO_MD		0x0D
#define hfc_A_INC_RES_FIFO	0x0E
#define hfc_R_FSM_IDX		0x0F
#define hfc_R_FIFO		0x0F
#define hfc_R_SLOT		0x10
#define hfc_R_IRQMSK_MISC	0x11
#define hfc_R_SCI_MSK		0x12
#define hfc_R_IRQ_CTRL		0x13
#define hfc_R_PCM_MD0		0x14
#define hfc_R_PCM_MD1		0x15
#define hfc_R_PCM_MD2		0x15
#define hfc_R_SH0H		0x15
#define hfc_R_SH1H		0x15
#define hfc_R_SH0L		0x15
#define hfc_R_SH1L		0x15
#define hfc_R_SL_SEL0		0x15
#define hfc_R_SL_SEL1		0x15
#define hfc_R_SL_SEL2		0x15
#define hfc_R_SL_SEL3		0x15
#define hfc_R_SL_SEL4		0x15
#define hfc_R_SL_SEL5		0x15
#define hfc_R_SL_SEL6		0x15
#define hfc_R_SL_SEL7		0x15
#define hfc_R_ST_SEL		0x16
#define hfc_R_ST_SYNC		0x17
#define hfc_R_CONF_EN		0x18
#define hfc_R_TI_WD		0x1A
#define hfc_R_BERT_WD_MD	0x1B
#define hfc_R_DTMF		0x1C
#define hfc_R_DTMF_N		0x1D
#define hfc_A_ST_WR_STA		0x30
#define hfc_A_ST_CTRL0		0x31
#define hfc_A_ST_CTRL1		0x32
#define hfc_A_ST_CTRL2		0x33
#define hfc_A_ST_SQ_WR		0x34
#define hfc_A_ST_CLK_DLY	0x37

#define hfc_R_PWM0		0x38
#define hfc_R_PWM1		0x39
#define hfc_A_ST_B1_TX		0x3C
#define hfc_A_ST_B2_TX		0x3D
#define hfc_A_ST_D_TX		0x3E
#define hfc_R_GPIO_OUT0		0x40
#define hfc_R_GPIO_OUT1		0x41
#define hfc_R_GPIO_EN0		0x42
#define hfc_R_GPIO_EN1		0x43
#define hfc_R_GPIO_SEL		0x44
#define hfc_R_PWM_MD		0x46
#define hfc_A_SL_CFG		0xD0
#define hfc_A_CONF		0xD1
#define hfc_A_CH_MSK		0xF4
#define hfc_A_CON_HDLC		0xFA
#define hfc_A_SUBCH_CFG		0xFB
#define hfc_A_CHANNEL		0xFC
#define hfc_A_FIFO_SEQ		0xFD
#define hfc_A_IRQ_MSK		0xFF

#define hfc_A_Z12		0x04
#define hfc_A_Z1L		0x04
#define hfc_A_Z1		0x04
#define hfc_A_Z1H		0x05
#define hfc_A_Z2L		0x06
#define hfc_A_Z2		0x06
#define hfc_A_Z2H		0x07
#define hfc_A_F1		0x0C
#define hfc_A_F12		0x0C
#define hfc_A_F2		0x0D
#define hfc_R_IRQ_OVIEW		0x10
#define hfc_R_IRQ_MISC		0x11
#define hfc_R_SCI		0x12
#define hfc_R_CONF_OFLOW	0x14
#define hfc_R_RAM_USE		0x15
#define hfc_R_CHIP_ID		0x16
#define hfc_R_BERT_STA		0x17
#define hfc_R_F0_CNTL		0x18
#define hfc_R_F0_CNTH		0x19
#define hfc_R_BERT_ECL		0x1A

#define hfc_R_BERT_ECH		0x1B
#define hfc_R_STATUS		0x1C
#define hfc_R_CHIP_RV		0x1F
#define hfc_A_ST_RD_STA		0x30
#define hfc_A_ST_SQ_RD		0x34
#define hfc_A_ST_B1_RX		0x3C
#define hfc_A_ST_B2_RX		0x3D
#define hfc_A_ST_D_RX		0x3E
#define hfc_A_ST_E_RX		0x3F
#define hfc_R_GPIO_IN0		0x40
#define hfc_R_GPIO_IN1		0x41
#define hfc_R_GPI_IN0		0x44
#define hfc_R_GPI_IN1		0x45
#define hfc_R_GPI_IN2		0x46
#define hfc_R_GPI_IN3		0x47
#define hfc_R_INT_DATA		0x88
#define hfc_R_IRQ_FIFO_BL0	0xC8
#define hfc_R_IRQ_FIFO_BL1	0xC9
#define hfc_R_IRQ_FIFO_BL2	0xCA
#define hfc_R_IRQ_FIFO_BL3	0xCB
#define hfc_R_IRQ_FIFO_BL4	0xCC
#define hfc_R_IRQ_FIFO_BL5	0xCD
#define hfc_R_IRQ_FIFO_BL6	0xCE
#define hfc_R_IRQ_FIFO_BL7	0xCF

#define hfc_A_FIFO_DATA2	0x80
#define hfc_A_FIFO_DATA0	0x80
#define hfc_A_FIFO_DATA1	0x80
#define hfc_A_FIFO_DATA2_NOINC	0x84
#define hfc_A_FIFO_DATA0_NOINC	0x84
#define hfc_A_FIFO_DATA1_NOINC	0x84
#define hfc_R_RAM_DATA		0xC0


// R_CIRM
#define hfc_R_CIRM_V_IRQ_SEL_NONE	(0 << 0)
#define hfc_R_CIRM_V_IRQ_SEL_IRQ0	(1 << 0)
#define hfc_R_CIRM_V_IRQ_SEL_IRQ1	(2 << 0)
#define hfc_R_CIRM_V_IRQ_SEL_IRQ2	(3 << 0)
#define hfc_R_CIRM_V_IRQ_SEL_IRQ3	(4 << 0)
#define hfc_R_CIRM_V_IRQ_SEL_IRQ4	(5 << 0)
#define hfc_R_CIRM_V_IRQ_SEL_IRQ5	(6 << 0)
#define hfc_R_CIRM_V_IRQ_SEL_IRQ6	(7 << 0)
#define hfc_R_CIRM_V_SRES		(1 << 3)
#define hfc_R_CIRM_V_HFC_RES		(1 << 4)
#define hfc_R_CIRM_V_PCM_RES		(1 << 5)
#define hfc_R_CIRM_V_ST_RES		(1 << 6)
#define hfc_R_CIRM_V_RLD_EPR		(1 << 7)

// R_STATUS
#define hfc_R_STATUS_V_BUSY		(1 << 0)
#define hfc_R_STATUS_V_PROC		(1 << 1)
#define hfc_R_STATUS_V_DTMF_IRQSTA	(1 << 2)
#define hfc_R_STATUS_V_LOST_STA		(1 << 3)
#define hfc_R_STATUS_V_SYNC_IN		(1 << 4)
#define hfc_R_STATUS_V_EXT_IRQSTA	(1 << 5)
#define hfc_R_STATUS_V_MISC_IRQSTA	(1 << 6)
#define hfc_R_STATUS_V_FR_IRQSTA	(1 << 7)

// R_IRQ_OVIEW
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL0	(1 << 0)
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL1	(1 << 1)
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL2	(1 << 2)
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL3	(1 << 3)
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL4	(1 << 4)
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL5	(1 << 5)
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL6	(1 << 6)
#define hfc_R_IRQ_OVIEW_V_IRQ_FIFO_BL7	(1 << 7)

//R_IRQ_MISC
#define hfc_R_IRQ_MISC_V_TI_IRQ		(1 << 1)
#define hfc_R_IRQ_MISC_V_IRQ_PROC	(1 << 2)
#define hfc_R_IRQ_MISC_V_DTMF_IRQ	(1 << 3)
#define hfc_R_IRQ_MISC_V_EXT_IRQ	(1 << 5)

// R_PWM_MD
#define hfc_R_PWM_MD_V_EXT_IRQ_EN		(1 << 3)
#define hfc_R_PWM_MD_V_PWM0_MD_DISABLE		(0 << 4)
#define hfc_R_PWM_MD_V_PWM0_MD_PUSH_PULL	(1 << 4)
#define hfc_R_PWM_MD_V_PWM0_MD_PUSH		(2 << 4)
#define hfc_R_PWM_MD_V_PWM0_MD_PULL		(3 << 4)
#define hfc_R_PWM_MD_V_PWM1_MD_DISABLE		(0 << 6)
#define hfc_R_PWM_MD_V_PWM1_MD_PUSH_PULL	(1 << 6)
#define hfc_R_PWM_MD_V_PWM1_MD_PUSH		(2 << 6)
#define hfc_R_PWM_MD_V_PWM1_MD_PULL		(3 << 6)

// R_TI_WD
#define hfc_R_TI_WD_V_EV_TS_250_US	(0x0 << 0)
#define hfc_R_TI_WD_V_EV_TS_500_US	(0x1 << 0)
#define hfc_R_TI_WD_V_EV_TS_1_MS	(0x2 << 0)
#define hfc_R_TI_WD_V_EV_TS_2_MS	(0x3 << 0)
#define hfc_R_TI_WD_V_EV_TS_4_MS	(0x4 << 0)
#define hfc_R_TI_WD_V_EV_TS_8_MS	(0x5 << 0)
#define hfc_R_TI_WD_V_EV_TS_16_MS	(0x6 << 0)
#define hfc_R_TI_WD_V_EV_TS_32_MS	(0x7 << 0)
#define hfc_R_TI_WD_V_EV_TS_64_MS	(0x8 << 0)
#define hfc_R_TI_WD_V_EV_TS_128_MS	(0x9 << 0)
#define hfc_R_TI_WD_V_EV_TS_256_MS	(0xA << 0)
#define hfc_R_TI_WD_V_EV_TS_512_MS	(0xB << 0)
#define hfc_R_TI_WD_V_EV_TS_1_024_S	(0xC << 0)
#define hfc_R_TI_WD_V_EV_TS_2_048_S	(0xD << 0)
#define hfc_R_TI_WD_V_EV_TS_4_096_S	(0xE << 0)
#define hfc_R_TI_WD_V_EV_TS_8_192_S	(0xF << 0)

#define hfc_R_TI_WD_V_WD_TS_2_MS	(0x0 << 4)
#define hfc_R_TI_WD_V_WD_TS_4_MS	(0x1 << 4)
#define hfc_R_TI_WD_V_WD_TS_8_MS	(0x2 << 4)
#define hfc_R_TI_WD_V_WD_TS_16_MS	(0x3 << 4)
#define hfc_R_TI_WD_V_WD_TS_32_MS	(0x4 << 4)
#define hfc_R_TI_WD_V_WD_TS_64_MS	(0x5 << 4)
#define hfc_R_TI_WD_V_WD_TS_128_MS	(0x6 << 4)
#define hfc_R_TI_WD_V_WD_TS_256_MS	(0x7 << 4)
#define hfc_R_TI_WD_V_WD_TS_512_MS	(0x8 << 4)
#define hfc_R_TI_WD_V_WD_TS_1_024_S	(0x9 << 4)
#define hfc_R_TI_WD_V_WD_TS_2_048_S	(0xA << 4)
#define hfc_R_TI_WD_V_WD_TS_4_096_S	(0xB << 4)
#define hfc_R_TI_WD_V_WD_TS_8_192_S	(0xC << 4)
#define hfc_R_TI_WD_V_WD_TS_16_384_S	(0xD << 4)
#define hfc_R_TI_WD_V_WD_TS_32_768_S	(0xE << 4)
#define hfc_R_TI_WD_V_WD_TS_65_536_S	(0xF << 4)

// R_IRQMSK_MISC
#define hfc_R_IRQMSK_MISC_V_TI_IRQMSK	(1 << 1)
#define hfc_R_IRQMSK_MISC_V_PROC_IRQMSK	(1 << 2)
#define hfc_R_IRQMSK_MISC_V_DTMF_IRQMSK	(1 << 3)
#define hfc_R_IRQMSK_MISC_V_EXT_IRQMSK	(1 << 5)

// R_PCM_MD0
#define hfc_R_PCM_MD0_V_PCM_MD			(1 << 0)
#define hfc_R_PCM_MD0_V_C4_POL			(1 << 1)
#define hfc_R_PCM_MD0_V_F0_NEG			(1 << 2)
#define hfc_R_PCM_MD0_V_F0_LEN			(1 << 3)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL0	(0x0 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL1	(0x1 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL2	(0x2 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL3	(0x3 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL4	(0x4 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL5	(0x5 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL6	(0x6 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SL_SEL7	(0x7 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_PCM_MD1	(0x9 << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_PCM_MD2	(0xA << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SH0L		(0xC << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SH0H		(0xD << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SH1L		(0xE << 4)
#define hfc_R_PCM_MD0_V_PCM_IDX_R_SH1H		(0xF << 4)

// R_SCI
#define hfc_R_SCI_V_SCI_ST0		(1 << 0)
#define hfc_R_SCI_V_SCI_ST1		(1 << 1)
#define hfc_R_SCI_V_SCI_ST2		(1 << 2)
#define hfc_R_SCI_V_SCI_ST3		(1 << 3)
#define hfc_R_SCI_V_SCI_ST4		(1 << 4)
#define hfc_R_SCI_V_SCI_ST5		(1 << 5)
#define hfc_R_SCI_V_SCI_ST6		(1 << 6)
#define hfc_R_SCI_V_SCI_ST7		(1 << 7)

// R_SCI_MSK
#define hfc_R_SCI_MSK_V_SCI_MSK_ST0	(1 << 0)
#define hfc_R_SCI_MSK_V_SCI_MSK_ST1	(1 << 1)
#define hfc_R_SCI_MSK_V_SCI_MSK_ST2	(1 << 2)
#define hfc_R_SCI_MSK_V_SCI_MSK_ST3	(1 << 3)
#define hfc_R_SCI_MSK_V_SCI_MSK_ST4	(1 << 4)
#define hfc_R_SCI_MSK_V_SCI_MSK_ST5	(1 << 5)
#define hfc_R_SCI_MSK_V_SCI_MSK_ST6	(1 << 6)
#define hfc_R_SCI_MSK_V_SCI_MSK_ST7	(1 << 7)

// R_FIFO_MD
#define hfc_R_FIFO_MD_V_FIFO_MD_00	(0x0 << 0)
#define hfc_R_FIFO_MD_V_FIFO_MD_10	(0x1 << 0)
#define hfc_R_FIFO_MD_V_FIFO_MD_11	(0x2 << 0)
#define hfc_R_FIFO_MD_V_DF_MD_SM	(0x0 << 2)
#define hfc_R_FIFO_MD_V_DF_MD_CSM	(0x1 << 2)
#define hfc_R_FIFO_MD_V_DF_MD_FSM	(0x3 << 2)
#define hfc_R_FIFO_MD_V_FIFO_SZ_00	(0x0 << 4)
#define hfc_R_FIFO_MD_V_FIFO_SZ_01	(0x1 << 4)
#define hfc_R_FIFO_MD_V_FIFO_SZ_10	(0x2 << 4)
#define hfc_R_FIFO_MD_V_FIFO_SZ_11	(0x3 << 4)

// R_BRG_PCM_CFG
#define hfc_R_BRG_PCM_CFG_V_PCM_CLK_DIV_1_5	(0 << 5)
#define hfc_R_BRG_PCM_CFG_V_PCM_CLK_DIV_3_0	(1 << 5)
#define hfc_R_BRG_PCM_CFG_V_ADDR_WRDLY_3NS	(0x0 << 6)
#define hfc_R_BRG_PCM_CFG_V_ADDR_WRDLY_5NS	(0x1 << 6)
#define hfc_R_BRG_PCM_CFG_V_ADDR_WRDLY_7NS	(0x2 << 6)
#define hfc_R_BRG_PCM_CFG_V_ADDR_WRDLY_9NS	(0x3 << 6)

// R_CTRL
#define hfc_R_CTRL_V_FIFO_LPRIO	(1 << 1)
#define hfc_R_CTRL_V_SLOW_RD		(1 << 2)
#define hfc_R_CTRL_V_EXT_RAM		(1 << 3)
#define hfc_R_CTRL_V_CLK_OFF		(1 << 5)
#define hfc_R_CTRL_V_ST_CLK_DIV4	(0x0 << 6)
#define hfc_R_CTRL_V_ST_CLK_DIV8	(0x1 << 6)
#define hfc_R_CTRL_V_ST_CLK_DIV1	(0x2 << 6)
#define hfc_R_CTRL_V_ST_CLK_DIV2	(0x3 << 6)

// R_IRQ_CTRL
#define hfc_R_V_FIFO_IRQ	(1 << 0)
#define hfc_R_V_GLOB_IRQ_EN	(1 << 3)
#define hfc_R_V_IRQ_POL_LOW	(0 << 4)
#define hfc_R_V_IRQ_POL_HIGH	(1 << 4)

// R_FIFO
#define hfc_R_FIFO_V_FIFO_DIR_TX	(0 << 0)
#define hfc_R_FIFO_V_FIFO_DIR_RX	(1 << 0)
#define hfc_R_FIFO_V_FIFO_NUM(num)	((num) << 1)
#define hfc_R_FIFO_V_REV_NORMAL		(0 << 7)
#define hfc_R_FIFO_V_REV_REVERSE	(1 << 7)

// A_INC_RES_FIFO
#define hfc_A_INC_RES_FIFO_V_INC_F	(1 << 0)
#define hfc_A_INC_RES_FIFO_V_RES_F	(1 << 1)
#define hfc_A_INC_RES_FIFO_V_RES_LOST	(1 << 2)

// A_CON_HDCL
#define hfc_A_CON_HDCL_V_IFF			(1 << 0)
#define hfc_A_CON_HDCL_V_HDLC_TRP_HDLC		(0 << 1)
#define hfc_A_CON_HDCL_V_HDLC_TRP_TRP		(1 << 1)
#define hfc_A_CON_HDCL_V_TRP_IRQ_DISABLED	(0x0 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_64		(0x1 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_128		(0x2 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_256		(0x3 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_512		(0x4 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_1024		(0x5 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_2048		(0x6 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_4096		(0x7 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_DISABLED	(0x0 << 2)
#define hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED	(0x1 << 2)
#define hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_to_ST_FIFO_to_PCM	(0x0 << 5)
#define hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_to_PCM			(0x2 << 5)
#define hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_to_ST_ST_to_PCM		(0x4 << 5)
#define hfc_A_CON_HDCL_V_DATA_FLOW_ST_to_PCM			(0x6 << 5)
#define hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST			(0x0 << 5)
#define hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_PCM		(0x1 << 5)
#define hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST_ST_from_PCM	(0x2 << 5)
#define hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_PCM_ST_from_PCM	(0x3 << 5)

// A_SUBCH_CFG
#define hfc_A_SUBCH_CFG_V_BIT_CNT_8		(0x0 << 0)
#define hfc_A_SUBCH_CFG_V_BIT_CNT_1		(0x1 << 0)
#define hfc_A_SUBCH_CFG_V_BIT_CNT_2		(0x2 << 0)
#define hfc_A_SUBCH_CFG_V_BIT_CNT_3		(0x3 << 0)
#define hfc_A_SUBCH_CFG_V_BIT_CNT_4		(0x4 << 0)
#define hfc_A_SUBCH_CFG_V_BIT_CNT_5		(0x5 << 0)
#define hfc_A_SUBCH_CFG_V_BIT_CNT_6		(0x6 << 0)
#define hfc_A_SUBCH_CFG_V_BIT_CNT_7		(0x7 << 0)
#define hfc_A_SUBCH_CFG_V_START_BIT(num)	((num) << 3)
#define hfc_A_SUBCH_CFG_V_LOOP_FIFO		(1 << 6)
#define hfc_A_SUBCH_CFG_V_INV_DATA		(1 << 7)

// A_IRQ_MSK
#define hfc_A_IRQ_MSK_V_IRQ		(1 << 0)
#define hfc_A_IRQ_MSK_V_BERT_EN		(1 << 1)
#define hfc_A_IRQ_MSK_V_MIX_IRQ		(1 << 2)

// R_ST_SEL
#define hfc_R_ST_SEL_V_ST_SEL(num)	((num) << 0)
#define hfc_R_ST_SEL_V_MULT_SEL		(1 << 3)

// A_ST_CTRL0
#define hfc_A_ST_CTRL0_V_B1_EN		(1 << 0)
#define hfc_A_ST_CTRL0_V_B2_EN		(1 << 1)
#define hfc_A_ST_CTRL0_V_ST_MD_TE	(0 << 2)
#define hfc_A_ST_CTRL0_V_ST_MD_NT	(1 << 2)
#define hfc_A_ST_CTRL0_V_D_PRIO		(1 << 3)
#define hfc_A_ST_CTRL0_V_SQ_EN		(1 << 4)
#define hfc_A_ST_CTRL0_V_96_KHZ		(1 << 5)
#define hfc_A_ST_CTRL0_V_TX_LI		(1 << 6)
#define hfc_A_ST_CTRL0_V_ST_STOP	(1 << 7)

// A_ST_CTRL1
#define hfc_A_ST_CTRL1_V_G2_G3_EN	(1 << 0)
#define hfc_A_ST_CTRL1_V_D_HI		(1 << 2)
#define hfc_A_ST_CTRL1_V_E_IGNO		(1 << 3)
#define hfc_A_ST_CTRL1_V_E_LO		(1 << 4)
#define hfc_A_ST_CTRL1_V_B12_SWAP	(1 << 7)

// A_ST_CTRL2
#define hfc_A_ST_CTRL2_V_B1_RX_EN	(1 << 0)
#define hfc_A_ST_CTRL2_V_B2_RX_EN	(1 << 1)
#define hfc_A_ST_CTRL2_V_ST_TRI		(1 << 6)

// A_ST_CLK_DLY
#define hfc_A_ST_CLK_DLY_V_ST_CLK_DLY(num)	((num) << 0)
#define hfc_A_ST_CLK_DLY_V_ST_SMPL(num)		((num) << 4)

// A_ST_WR_STA
#define hfc_A_ST_WR_STA_V_ST_SET_STA(num)	((num) << 0)
#define hfc_A_ST_WR_STA_V_ST_LD_STA		(1 << 4)
#define hfc_A_ST_WR_STA_V_ST_ACT_DEACTIVATION	(0x2 << 5)
#define hfc_A_ST_WR_STA_V_ST_ACT_ACTIVATION	(0x3 << 5)
#define hfc_A_ST_WR_STA_V_SET_G2_G3		(1 << 7)

// A_ST_RD_STA
#define hfc_A_ST_RD_STA_V_ST_STA(val)		((val) & 0xF)
#define hfc_A_ST_RD_STA_V_FR_SYNC		(1 << 4)
#define hfc_A_ST_RD_STA_V_T2_EXP		(1 << 5)
#define hfc_A_ST_RD_STA_V_INFO0			(1 << 6)
#define hfc_A_ST_RD_STA_V_G2_G3			(1 << 7)

// A_ST_SQ_RD
#define hfc_A_ST_SQ_RD_V_ST_SQ_RD(val)		((val) & 0xF)
#define hfc_A_ST_SQ_RD_V_MF_RX_RDY		(1 << 4)
#define hfc_A_ST_SQ_RD_V_MF_TX_RDY		(1 << 7)

// R_ST_SYNC
#define hfc_R_ST_SYNC_V_SYNC_SEL(num)		((num) << 0)
#define hfc_R_ST_SYNC_V_AUTO_SYNC		(1 << 3)

// R_IRQ_FIFO_BL0
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO0_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO0_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO1_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO1_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO2_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO2_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO3_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL0_V_IRQ_FIFO3_RX	(1 << 7)

// R_IRQ_FIFO_BL1
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO4_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO4_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO5_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO5_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO6_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO6_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO7_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL1_V_IRQ_FIFO7_RX	(1 << 7)

// R_IRQ_FIFO_BL2
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO8_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO8_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO9_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO9_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO10_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO10_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO11_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL2_V_IRQ_FIFO11_RX	(1 << 7)

// R_IRQ_FIFO_BL3
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO12_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO12_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO13_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO13_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO14_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO14_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO15_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL3_V_IRQ_FIFO15_RX	(1 << 7)

// R_IRQ_FIFO_BL4
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO16_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO16_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO17_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO17_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO18_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO18_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO19_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL4_V_IRQ_FIFO19_RX	(1 << 7)


// R_IRQ_FIFO_BL5
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO20_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO20_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO21_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO21_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO22_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO22_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO23_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL5_V_IRQ_FIFO23_RX	(1 << 7)

// R_IRQ_FIFO_BL6
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO24_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO24_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO25_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO25_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO26_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO26_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO27_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL6_V_IRQ_FIFO27_RX	(1 << 7)

// R_IRQ_FIFO_BL7
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO28_TX	(1 << 0)
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO28_RX	(1 << 1)
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO29_TX	(1 << 2)
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO29_RX	(1 << 3)
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO30_TX	(1 << 4)
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO30_RX	(1 << 5)
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO31_TX	(1 << 6)
#define hfc_R_IRQ_FIFO_BL7_V_IRQ_FIFO31_RX	(1 << 7)

#endif
