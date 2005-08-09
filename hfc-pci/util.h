#ifndef _HFC_UTIL_H
#define _HFC_UTIL_H

#ifdef DEBUG
#define hfc_debug(dbglevel, format, arg...)		\
	if (debug_level >= dbglevel)			\
		printk(KERN_DEBUG hfc_DRIVER_PREFIX format, ## arg)
#define hfc_debug_cont(dbglevel, format, arg...)	\
	if (debug_level >= dbglevel)			\
		printk(format, ## arg)
#else
#define hfc_debug(dbglevel, format, arg...) do {} while (0)
#define hfc_debug_cont(dbglevel, format, arg...) do {} while (0)
#endif


#define hfc_msg(level, format, arg...)	\
	printk(level hfc_DRIVER_PREFIX			\
		format, ## arg)

#ifndef intptr_t
#define intptr_t unsigned long
#endif

#ifndef uintptr_t
#define uintptr_t unsigned long
#endif

#ifndef PCI_DMA_32BIT
#define PCI_DMA_32BIT	0x00000000ffffffffULL
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef PCI_VENDOR_ID_SITECOM
#define PCI_VENDOR_ID_SITECOM 0x182D
#endif

#ifndef PCI_DEVICE_ID_SITECOM_3069
#define PCI_DEVICE_ID_SITECOM_3069 0x3069
#endif


extern int debug_level;

enum hfc_direction { RX = 0, TX = 1 };

#endif