#ifndef QUARD_STAR_RPMSG_H
#define QUARD_STAR_RPMSG_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 quard_star_rpmsg_u32;
typedef u8 quard_star_rpmsg_u8;
typedef s32 quard_star_rpmsg_s32;
#define QUARD_STAR_RPMSG_PACKED __packed
#else
#include <stdint.h>
typedef uint32_t quard_star_rpmsg_u32;
typedef uint8_t quard_star_rpmsg_u8;
typedef int32_t quard_star_rpmsg_s32;
#define QUARD_STAR_RPMSG_PACKED __attribute__((packed))
#endif

#define QUARD_STAR_RPMSG_SHM_SIZE          0x10000U
#define QUARD_STAR_RPMSG_RSC_OFFSET        0x0000U
#define QUARD_STAR_RPMSG_VRING0_OFFSET     0x1000U
#define QUARD_STAR_RPMSG_VRING1_OFFSET     0x3000U
#define QUARD_STAR_RPMSG_BUF_OFFSET        0x5000U
#define QUARD_STAR_RPMSG_BUF_SIZE          \
	(QUARD_STAR_RPMSG_SHM_SIZE - QUARD_STAR_RPMSG_BUF_OFFSET)

#define QUARD_STAR_RPMSG_VRING_COUNT       2U
#define QUARD_STAR_RPMSG_VRING_NUM         16U
#define QUARD_STAR_RPMSG_VRING_ALIGN       4096U

#define QUARD_STAR_RPMSG_VDEV_NOTIFYID     0U
#define QUARD_STAR_RPMSG_VRING0_NOTIFYID   1U
#define QUARD_STAR_RPMSG_VRING1_NOTIFYID   2U

#define QUARD_STAR_RPMSG_RESOURCE_VDEV     3U
#define QUARD_STAR_RPMSG_VIRTIO_ID_RPMSG   7U
#define QUARD_STAR_RPMSG_F_NS              0U

#define QUARD_STAR_RPMSG_RAW_SERVICE       "rpmsg-raw"
#define QUARD_STAR_RPMSG_LED_SERVICE       "qs-led"

struct quard_star_rpmsg_rsc_vring {
	quard_star_rpmsg_u32 da;
	quard_star_rpmsg_u32 align;
	quard_star_rpmsg_u32 num;
	quard_star_rpmsg_u32 notifyid;
	quard_star_rpmsg_u32 reserved;
} QUARD_STAR_RPMSG_PACKED;

struct quard_star_rpmsg_rsc_vdev {
	quard_star_rpmsg_u32 type;
	quard_star_rpmsg_u32 id;
	quard_star_rpmsg_u32 notifyid;
	quard_star_rpmsg_u32 dfeatures;
	quard_star_rpmsg_u32 gfeatures;
	quard_star_rpmsg_u32 config_len;
	quard_star_rpmsg_u8 status;
	quard_star_rpmsg_u8 num_of_vrings;
	quard_star_rpmsg_u8 reserved[2];
	struct quard_star_rpmsg_rsc_vring vring[QUARD_STAR_RPMSG_VRING_COUNT];
} QUARD_STAR_RPMSG_PACKED;

enum quard_star_rpmsg_led_cmd {
	QUARD_STAR_RPMSG_LED_GET = 1,
	QUARD_STAR_RPMSG_LED_SET = 2,
};

struct quard_star_rpmsg_led_msg {
	quard_star_rpmsg_u32 cmd;
	quard_star_rpmsg_u32 led;
	quard_star_rpmsg_u32 value;
	quard_star_rpmsg_s32 status;
} QUARD_STAR_RPMSG_PACKED;

#endif
