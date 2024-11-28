#include "mlfs/mlfs_user.h"
#include "coalesce.h"
#include "loghdr.h"
#include "compress.h"
#include "storage/storage.h" // nic_slab
#include "limit_rate.h"

#ifdef SHORT_PATH
#include "copy_log.h"
#endif

threadpool thpool_coalesce;

// Thread pools of the next pipeline stages.
threadpool thpool_loghdr_build;
threadpool thpool_compress;

TL_EVENT_TIMER(evt_coalesce);
TL_EVENT_TIMER(evt_coalesce_rate_limit);
TL_EVENT_TIMER(evt_coalesce_build_loghdrs_arg);
TL_EVENT_TIMER(evt_coalesce_compress_arg);

typedef struct rm_bd_arg {
	void *arg;
} rm_bd_arg_t;

typedef struct rm_cp_arg {
	void *arg;
} rm_cp_arg_t;

int post_bd_req(void *arg)
{
	rm_bd_arg_t *bd_arg = (rm_bd_arg_t *)arg;
	thpool_add_work(thpool_loghdr_build, build_loghdr_list, (void *)bd_arg->arg);
	return 0;
}

int post_cp_req(void *arg)
{
	rm_cp_arg_t *cp_arg = (rm_cp_arg_t *)arg;
	thpool_add_work(thpool_copy_to_last_replica, copy_log_to_last_replica_bg, (void *)cp_arg->arg);
	return 0;
}

threadpool init_coalesce_thpool(void)
{
	int th_num = mlfs_conf.thread_num_coalesce;
	char th_name[] = "coales";

	thpool_coalesce = thpool_init(th_num, th_name);

	print_thread_init(th_num, th_name);

	// Init threadpool of the next pipeline stage.
	thpool_loghdr_build = init_loghdr_build_thpool();
	thpool_compress = init_compress_thpool();

#ifdef REQUEST_MANAGER
	register_rm_func(rm_handle, post_bd_req, RM_BD_REQ);
	register_rm_func(rm_handle, post_cp_req, RM_CP_REQ);
#endif

	return thpool_coalesce;
}

static void validate_log(void)
{
	// TODO
	;
}

// I'm primary. Limit my request rate.
// TODO Better to move to the end of the compression stage.
static void limit_my_request_rate(int libfs_id, uint64_t seqn)
{
	struct timespec start, end;
	START_TL_TIMER(evt_coalesce_rate_limit);

	if (*primary_rate_limit_flag) {
		printf(ANSI_COLOR_RED "Limit rate of sending reqeust to "
				      "Replica 1 libfs_id=%d "
				      "pending_seqn=%lu" ANSI_COLOR_RESET "\n",
		       libfs_id, seqn);
	}

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (*primary_rate_limit_flag) {
		clock_gettime(CLOCK_MONOTONIC, &end);
		if (get_duration(&start, &end) > 1.0) {
			mlfs_printf(ANSI_COLOR_RED "Limit rate of sending "
						   "reqeust to Replica 1 "
						   "libfs_id=%d "
						   "pending_seqn=%lu"
						   ANSI_COLOR_RESET "\n",
				    libfs_id, seqn);
			clock_gettime(CLOCK_MONOTONIC, &start);
		}

		cpu_relax();
		usleep(10); // TODO do we need it?
	}

	END_TL_TIMER(evt_coalesce_rate_limit);
}

void coalesce_log(void *arg)
{
	START_TL_TIMER(evt_coalesce);

	coalesce_arg *c_arg = (coalesce_arg *)arg;
	struct replication_context *rctx = c_arg->rctx;
	uint64_t *fetch_log_done_p;
	uint32_t n_coalesced_loghdrs;
	uint64_t coalesced_log_size;

#ifdef SEQN_REORDER_ADVANCED
	unsigned long newest = atomic_load(&rctx->coalesce_newest);
	unsigned long expect = atomic_load(&rctx->coalesce_expect);
	unsigned long seqn = c_arg->seqn;
	if (seqn > newest) {
		newest = seqn;
		atomic_store(&rctx->coalesce_newest, newest);
	}
	if (seqn == expect + 1) atomic_store(&rctx->coalesce_expect, newest);
#endif

	print_coalesce_arg(c_arg);

	validate_log(); // TODO

#ifdef PIPELINE_RATE_LIMIT
	// Limit request rate.
	// Otherwise, replica 1 gets OOM.
	limit_my_request_rate(c_arg->rctx->peer->id, c_arg->seqn);
#endif

	// TODO do coalescing. Build replay_list.
	//
	n_coalesced_loghdrs = c_arg->n_loghdrs;
	coalesced_log_size = c_arg->log_size;

	// Allocate log_buf flag to free buffer asynchronously.
#ifndef SETTLED_LOG_BUF
#ifndef NO_MEM_FREE
	fetch_log_done_p = (uint64_t *)nic_slab_alloc_in_byte(sizeof(uint64_t));
#else
	fetch_log_done_p = (uint64_t *)mlfs_alloc(sizeof(uint64_t));
#endif
#else
	fetch_log_done_p = alloc_settled_log_buf_flag();
#endif
	*fetch_log_done_p = 0;

	// Next pipeline stage.
	// 1. Build loghdr list.
	START_TL_TIMER(evt_coalesce_build_loghdrs_arg);

	build_loghdrs_arg *bl_arg =
		(build_loghdrs_arg *)mlfs_alloc(sizeof(build_loghdrs_arg));

	bl_arg->seqn = c_arg->seqn;
	bl_arg->rctx = rctx;
	bl_arg->log_buf = c_arg->log_buf;
	bl_arg->log_size = coalesced_log_size;
	bl_arg->n_loghdrs = n_coalesced_loghdrs; // TODO coalesced
	bl_arg->fetch_start_blknr = c_arg->fetch_start_blknr;
	bl_arg->fetch_log_done_p = fetch_log_done_p;
	bl_arg->n_orig_loghdrs = c_arg->n_loghdrs;
	bl_arg->n_orig_blks = (c_arg->log_size >> g_block_size_shift);
	bl_arg->reset_meta = c_arg->reset_meta;

	END_TL_TIMER(evt_coalesce_build_loghdrs_arg);

#ifndef NO_PIPELINING
#ifndef NO_HDR_ALL
#ifndef REQUEST_MANAGER
	thpool_add_work(thpool_loghdr_build, build_loghdr_list, (void *)bl_arg);
#else
	rm_req_t *rm_req = (rm_req_t *)mlfs_zalloc(sizeof(rm_req_t) + sizeof(rm_bd_arg_t));
	rm_req->key = bl_arg->log_buf;
	rm_bd_arg_t *bd_arg = (rm_bd_arg_t *)rm_req->arg;
	bd_arg->arg = bl_arg;
	post_rm_req(rm_handle, rm_req, RM_BD_REQ);
#endif
#endif
#endif

#ifndef SHORT_PATH
	// 2. Compress.
	START_TL_TIMER(evt_coalesce_compress_arg);

	compress_arg *cp_arg = (compress_arg *)mlfs_alloc(sizeof(compress_arg));
	cp_arg->rctx = rctx;
	cp_arg->seqn = c_arg->seqn;
	cp_arg->log_buf = c_arg->log_buf;
	cp_arg->log_size = c_arg->log_size;
	cp_arg->fetch_start_blknr = c_arg->fetch_start_blknr;
	cp_arg->fetch_log_done_p = fetch_log_done_p;
	cp_arg->fsync = c_arg->fsync;
	cp_arg->fsync_ack_addr = c_arg->fsync_ack_addr;

	END_TL_TIMER(evt_coalesce_compress_arg);

	if (c_arg->fsync) {
		END_TL_TIMER(evt_coalesce);
		compress_log_bg((void *)cp_arg);
#ifdef NO_PIPELINING
		// On fsync, replicate log first.
		build_loghdr_list((void *)bl_arg);
#endif
	} else {
		thpool_add_work(thpool_compress, compress_log_bg,
				(void *)cp_arg);
		END_TL_TIMER(evt_coalesce);

#ifdef NO_PIPELINING
		// If it is not fsync, a replication path is created with a new
		// thread.
		build_loghdr_list((void *)bl_arg);
#endif
	}
#else
	// If there are only two replicas, we don't need to compress.
	// Just copy original log to the next (and last) replica.

	int libfs_id = rctx->peer->id;
	copy_to_last_replica_arg *cr_arg =
		(copy_to_last_replica_arg *)mlfs_alloc(
			sizeof(copy_to_last_replica_arg));
	cr_arg->rctx = g_sync_ctx[libfs_id];
	cr_arg->libfs_id = libfs_id;
	cr_arg->seqn = c_arg->seqn;
	cr_arg->log_buf = c_arg->log_buf;
	cr_arg->log_size = c_arg->log_size;
	cr_arg->orig_log_size = c_arg->log_size;
	cr_arg->start_blknr = c_arg->fetch_start_blknr;
	cr_arg->copy_log_done_p = (atomic_bool *) fetch_log_done_p;
	cr_arg->fsync = c_arg->fsync;
	cr_arg->fsync_ack_addr = c_arg->fsync_ack_addr;

	if (c_arg->fsync) {
		END_TL_TIMER(evt_coalesce);
		copy_log_to_last_replica_bg((void *)cr_arg);
#ifdef NO_PIPELINING
		// On fsync, replicate log first.
		build_loghdr_list((void *)bl_arg);
#endif
	} else {
		// NOTE We don't send ack to libfs on fsync assuming that local copy
		// finishes earlier than remote copy.
#ifndef REQUEST_MANAGER
		thpool_add_work(thpool_copy_to_last_replica,
				copy_log_to_last_replica_bg, (void *)cr_arg);
#else
		rm_req_t *rm_req = (rm_req_t *)mlfs_zalloc(sizeof(rm_req_t) + sizeof(rm_cp_arg_t));
		rm_req->key = cr_arg->log_buf;
		rm_cp_arg_t *cp_arg = (rm_cp_arg_t *)rm_req->arg;
		cp_arg->arg = cr_arg;
		post_rm_req(rm_handle, rm_req, RM_CP_REQ);
#endif
		END_TL_TIMER(evt_coalesce);

#ifdef NO_PIPELINING
		// If it is not fsync, a replication path is created with a new
		// thread.
		build_loghdr_list((void *)bl_arg);
#endif
	}
#endif  // SHORT_PATH

	mlfs_free(arg);
}

static void print_coalesce_arg(coalesce_arg *ar)
{
	pr_pipe("ARG %-30s libfs_id=%d seqn=%lu log_buf=%p log_size=%lu "
		"n_loghdrs=%u fetch_start_blknr=%lu reset_meta=%d fsync=%d "
		"fsync_ack_addr=%lu",
		"coalesce", ar->rctx->peer->id, ar->seqn, ar->log_buf,
		ar->log_size, ar->n_loghdrs, ar->fetch_start_blknr,
		ar->reset_meta, ar->fsync, ar->fsync_ack_addr);
}

void print_coalesce_thpool_stat(void)
{
#ifdef PROFILE_THPOOL
	print_profile_result(thpool_coalesce);
#endif
}

/** Print functions registered to each thread. **/
void print_coalesce_stat(void *arg)
{
	PRINT_TL_TIMER(evt_coalesce, arg);
	// PRINT_TL_TIMER(evt_coalesce_build_loghdrs_arg, arg);
	// PRINT_TL_TIMER(evt_coalesce_compress_arg, arg);

	RESET_TL_TIMER(evt_coalesce);
}

// PRINT_ALL_PIPELINE_STAT_FUNC(thpool_coalesce)
PRINT_ALL_PIPELINE_STAT_FUNC(coalesce)
