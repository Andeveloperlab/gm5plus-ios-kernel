#pragma once
#include <stdint.h>

/*
 * Qualcomm MSM8952 (Snapdragon 617) ChipIdea High-Speed USB 2.0 OTG Controller
 * Physical Base: 0x078db000
 * Synopsys 28nm Femto PHY CSR Base: 0x0006c000
 */

#define MSM8952_USB_BASE          0x078db000UL
#define MSM8952_USB_PHY_CSR_BASE  0x0006c000UL

/* Core Registers (relative to MSM8952_USB_BASE) */
#define CI_ID                     0x0000
#define CI_ABS_AHBBURST           0x0090
#define CI_ABS_AHBMODE            0x0098
#define CI_MSM_GENCONFIG          0x009C
#define CI_MSM_GENCONFIG_2        0x00A0
#define GENCONFIG_TXFIFO_IDLE_DIS (1U << 4)
#define GENCONFIG_2_SESS_VLD_EN   (1U << 7)
#define GENCONFIG_2_TX_CLR_FIX    (1U << 19)
#define CI_CAPLENGTH              0x0100
#define CI_HCCPARAMS              0x0108
#define CI_DCCPARAMS              0x0124

/* Operational Registers (relative to cap_base = base + 0x0140) */
#define CI_USBCMD                 0x0000
#define CI_USBSTS                 0x0004
#define CI_USBINTR                0x0008
#define CI_DEVICEADDR             0x0014
#define CI_ENDPTLISTADDR          0x0018
#define CI_ULPI_VIEWPORT          0x0030
#define CI_PORTSC                 0x0044

/* When HCCPARAMS.LEN (bit 17) is 1 (LPM enabled on MSM8952) */
#define CI_LPM_DEVLC              0x0084
#define CI_LPM_USBMODE            0x00C8
#define CI_LPM_ENDPTSETUPSTAT     0x00D8
#define CI_LPM_ENDPTPRIME         0x00DC
#define CI_LPM_ENDPTFLUSH         0x00E0
#define CI_LPM_ENDPTSTAT          0x00E4
#define CI_LPM_ENDPTCOMPLETE      0x00E8
#define CI_LPM_ENDPTCTRL(n)       (0x00EC + 4 * (n))

/* When HCCPARAMS.LEN is 0 (non-LPM layout) */
#define CI_STD_USBMODE            0x0068
#define CI_STD_ENDPTSETUPSTAT     0x006C
#define CI_STD_ENDPTPRIME         0x0070
#define CI_STD_ENDPTFLUSH         0x0074
#define CI_STD_ENDPTSTAT          0x0078
#define CI_STD_ENDPTCOMPLETE      0x007C
#define CI_STD_ENDPTCTRL(n)       (0x0080 + 4 * (n))

/* USBCMD Bits */
#define USBCMD_RS                 (1U << 0)   /* Run / Stop */
#define USBCMD_RST                (1U << 1)   /* Controller Soft Reset */
#define USBCMD_SUTW               (1U << 13)  /* Setup TripWire */
#define USBCMD_ITC(n)             (((n) & 0xFF) << 16) /* Interrupt Threshold */
#define USBCMD_SESS_VLD_CTRL      (1U << 25)  /* Qualcomm Session Valid / VBUS Override */

/* USBSTS Bits */
#define USBSTS_UI                 (1U << 0)   /* USB Interrupt (transfer complete) */
#define USBSTS_UEI                (1U << 1)   /* USB Error Interrupt */
#define USBSTS_PCI                (1U << 2)   /* Port Change Detect */
#define USBSTS_URI                (1U << 6)   /* USB Reset Received */
#define USBSTS_SRI                (1U << 7)   /* SOF Received */
#define USBSTS_SLI                (1U << 8)   /* DCSuspend */

/* USBINTR Bits */
#define USBINTR_UIE               (1U << 0)
#define USBINTR_UEIE              (1U << 1)
#define USBINTR_PCIE              (1U << 2)
#define USBINTR_URE               (1U << 6)
#define USBINTR_SLE               (1U << 8)

/* DEVICEADDR Bits */
#define DEVICEADDR_USBADRA        (1U << 24)  /* Advance Device Address */
#define DEVICEADDR_USBADR(a)      (((uint32_t)(a) & 0x7F) << 25)

/* PORTSC Bits */
#define PORTSC_PTS_ULPI           (1U << 31)  /* Parallel Transceiver Select: ULPI */
#define PORTSC_PFSC               (1U << 24)  /* Port Force Full Speed Connect */
#define PORTSC_PR                 (1U << 8)   /* Port Reset */
#define PORTSC_HSP                (1U << 9)   /* High Speed Port */
#define PORTSC_CCS                (1U << 0)   /* Current Connect Status */

/* USBMODE Bits */
#define USBMODE_CM_IDLE           0x00
#define USBMODE_CM_DEVICE         0x02        /* Controller Mode: Device */
#define USBMODE_CM_HOST           0x03
#define USBMODE_SLOM              (1U << 3)   /* Setup Lockout Mode Off */
#define USBMODE_SDIS              (1U << 4)   /* Stream Disable Mode */

/* ENDPTCTRL Bits */
#define ENDPTCTRL_RXS             (1U << 0)   /* RX Stall */
#define ENDPTCTRL_RXT_CTRL        (0U << 2)   /* Control Endpoint */
#define ENDPTCTRL_RXT_BULK        (2U << 2)   /* Bulk Endpoint */
#define ENDPTCTRL_RXT_INT         (3U << 2)   /* Interrupt Endpoint */
#define ENDPTCTRL_RXR             (1U << 6)   /* RX Data Toggle Reset */
#define ENDPTCTRL_RXE             (1U << 7)   /* RX Endpoint Enable */

#define ENDPTCTRL_TXS             (1U << 16)  /* TX Stall */
#define ENDPTCTRL_TXT_CTRL        (0U << 18)  /* Control Endpoint */
#define ENDPTCTRL_TXT_BULK        (2U << 18)  /* Bulk Endpoint */
#define ENDPTCTRL_TXT_INT         (3U << 18)  /* Interrupt Endpoint */
#define ENDPTCTRL_TXR             (1U << 22)  /* TX Data Toggle Reset */
#define ENDPTCTRL_TXE             (1U << 23)  /* TX Endpoint Enable */

/* ULPI Viewport Bits */
#define ULPI_WAKEUP               (1U << 31)  /* Wake up ULPI PHY before transaction */
#define ULPI_RUN                  (1U << 30)
#define ULPI_WRITE                (1U << 29)
#define ULPI_SYNC_STATE           (1U << 27)
#define ULPI_ADDR(a)              (((uint32_t)(a) & 0xFF) << 16)
#define ULPI_DATA_WR(d)           ((uint32_t)(d) & 0xFF)
#define ULPI_DATA_RD(v)           (((uint32_t)(v) >> 8) & 0xFF)

/* Synopsys 28nm Femto PHY CSR Registers */
#define USB_PHY_CSR_PHY_CTRL_COMMON0  0x0078
#define PHY_CTRL_SIDDQ            (1U << 2)   /* Retention / Power Down */

/* dTD and dQH flags */
#define TD_TERMINATE              (1U << 0)
#define TD_STATUS_ACTIVE          (1U << 7)
#define TD_STATUS_HALTED          (1U << 6)
#define TD_STATUS_DATA_BUFF_ERR   (1U << 5)
#define TD_STATUS_TRANSACTION_ERR (1U << 3)
#define TD_IOC                    (1U << 15)

#define QH_IOS                    (1U << 15)  /* Interrupt on Setup */
#define QH_MAX_PKT(len)           (((uint32_t)(len) & 0x7FF) << 16)
#define QH_ZLT                    (1U << 29)  /* Zero-Length Termination */
