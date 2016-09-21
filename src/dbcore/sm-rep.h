#pragma once

#include <iostream>
#include <thread>
#include <vector>

#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "rdma.h"
#include "tcp.h"
#include "../macros.h"

#include "sm-config.h"
#include "sm-file.h"
#include "sm-log.h"
#include "sm-log-offset.h"

struct write_record_t;

namespace rep {

// Common functions
void start_as_primary();
void start_as_backup();
void primary_ship_log_buffer_all(const char *buf, uint32_t size);
void redo_daemon();

/* Ship a log file (on-disk) to a backup server via TCP using sendfile().
 * For primary server only. So far the only user is when the
 * primary server starts and has loaded the database.
 */
void primary_ship_log_file(int backup_fd, const char* log_fname, int log_fd);

/* RDMA message states, for log shipping using RDMA only */
static const uint64_t RDMA_READY_TO_RECEIVE = 1UL;
static const uint64_t RDMA_WAITING = 2UL;

// RDMA-specific functions
void init_rdma();
void backup_daemon_rdma();
void primary_ship_log_buffer_rdma(const char *buf, uint32_t size);
void update_oid_on_backup_rdma(write_record_t* w);

// TCP-specific functions
void backup_daemon_tcp(tcp::client_context *cctx);

/* Send a chunk of log records (still in memory log buffer) to a backup via TCP.
 */
void primary_ship_log_buffer_tcp(
  int backup_sockfd, const char* buf, uint32_t size);
};  // namespace rep
