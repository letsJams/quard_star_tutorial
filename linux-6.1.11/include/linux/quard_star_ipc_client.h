#ifndef _LINUX_QUARD_STAR_IPC_CLIENT_H
#define _LINUX_QUARD_STAR_IPC_CLIENT_H

#include <linux/types.h>

#define QUARD_STAR_IPC_DEFAULT_TIMEOUT_MS  1000U

struct quard_star_ipc_request {
	unsigned char service;
	unsigned char opcode;
	const void *tx_payload;
	unsigned int tx_len;
	void *rx_payload;
	unsigned int rx_len;
	unsigned int *rx_len_out;
	unsigned int timeout_ms;
};

int quard_star_ipc_call(const void *tx, size_t tx_len, void *rx, size_t *rx_len);
int quard_star_ipc_call_timeout(const void *tx, size_t tx_len,
				void *rx, size_t *rx_len,
				unsigned int timeout_ms);
int quard_star_ipc_request(struct quard_star_ipc_request *req);

#endif
