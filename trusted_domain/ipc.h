#ifndef QUARD_STAR_TRUSTED_IPC_H
#define QUARD_STAR_TRUSTED_IPC_H

#include <stddef.h>
#include "quard_star_ipc.h"

#define QUARD_STAR_IPC_ERR_EMPTY   (-1)
#define QUARD_STAR_IPC_ERR_NOSPC   (-2)
#define QUARD_STAR_IPC_ERR_INVAL   (-3)
#define QUARD_STAR_IPC_ERR_TOOLONG (-4)

void quard_star_ipc_init(void);
int quard_star_ipc_send(const char *buf, size_t len);
int quard_star_ipc_recv(char *buf, size_t cap);

#endif
