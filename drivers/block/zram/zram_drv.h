/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ZRAM_DRV_H_
#define _ZRAM_DRV_H_

#include <linux/rwsem.h>
#include <linux/zsmalloc.h>
#include <linux/crypto.h>
#include <linux/mm.h>

#include "zcomp.h"

#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define ZRAM_LOGICAL_BLOCK_SHIFT 12
#define ZRAM_LOGICAL_BLOCK_SIZE	(1 << ZRAM_LOGICAL_BLOCK_SHIFT)
#define ZRAM_SECTOR_PER_LOGICAL_BLOCK	\
	(1 << (ZRAM_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))


/*
 * ZRAM is mainly used for memory efficiency so we want to keep memory
 * footprint small and thus squeeze size and zram pageflags into a flags
 * member. The lower ZRAM_FLAG_SHIFT bits is for object size (excluding
 * header), which cannot be larger than PAGE_SIZE (requiring PAGE_SHIFT
 * bits), the higher bits are for zram_pageflags.
 *
 * We use BUILD_BUG_ON() to make sure that zram pageflags don't overflow.
 */
#define ZRAM_FLAG_SHIFT (PAGE_SHIFT + 1)

/* Flags for zram pages (table[page_no].flags) */
enum zram_pageflags {
	/* zram slot is locked */
	ZRAM_LOCK = ZRAM_FLAG_SHIFT,
	ZRAM_SAME,	/* Page consists the same element */
	ZRAM_WB,	/* page is stored on backing_device */
	ZRAM_UNDER_WB,	/* page is under writeback */
	ZRAM_HUGE,	/* Incompressible page */
	ZRAM_IDLE,	/* not accessed page since last idle marking */
#ifdef CONFIG_ZRAM_RAMPLUS
	ZRAM_EXPIRE,
	ZRAM_PPR,
	ZRAM_LRU,
#endif

	__NR_ZRAM_PAGEFLAGS,
};

/*-- Data structures */

/* Allocated for each disk page */
struct zram_table_entry {
	union {
		unsigned long handle;
		unsigned long element;
	};
	unsigned long flags;
#ifdef CONFIG_ZRAM_MEMORY_TRACKING
	ktime_t ac_time;
#endif
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	struct list_head list;
#endif
};

#ifdef CONFIG_ZRAM_PERF_STAT
#define NR_IO_TYPES 2

struct zram_perf_stat {
	ktime_t start;
	atomic64_t nr_io;
	atomic64_t nr_pages;
	atomic64_t time;
	atomic64_t cnt;
};
#endif

struct zram_stats {
	atomic64_t compr_data_size;	/* compressed size of pages stored */
	atomic64_t num_reads;	/* failed + successful */
	atomic64_t num_writes;	/* --do-- */
	atomic64_t failed_reads;	/* can happen when memory is too low */
	atomic64_t failed_writes;	/* can happen when memory is too low */
	atomic64_t invalid_io;	/* non-page-aligned I/O requests */
	atomic64_t notify_free;	/* no. of swap slot free notifications */
	atomic64_t same_pages;		/* no. of same element filled pages */
	atomic64_t huge_pages;		/* no. of huge pages */
	atomic64_t huge_pages_since;	/* no. of huge pages since zram set up */
	atomic64_t pages_stored;	/* no. of pages currently stored */
	atomic_long_t max_used_pages;	/* no. of maximum pages stored */
	atomic64_t writestall;		/* no. of write slow paths */
	atomic64_t miss_free;		/* no. of missed free */
#ifdef	CONFIG_ZRAM_WRITEBACK
	atomic64_t bd_count;		/* no. of pages in backing device */
	atomic64_t bd_reads;		/* no. of reads from backing device */
	atomic64_t bd_writes;		/* no. of writes from backing device */
#endif
#ifdef CONFIG_ZRAM_RAMPLUS
	atomic64_t bd_expire;
	atomic64_t bd_objcnt;
	atomic64_t bd_size;
	atomic64_t bd_max_count;
	atomic64_t bd_max_size;
	atomic64_t bd_ppr_count;
	atomic64_t bd_ppr_reads;
	atomic64_t bd_ppr_writes;
	atomic64_t bd_ppr_objcnt;
	atomic64_t bd_ppr_size;
	atomic64_t bd_ppr_max_count;
	atomic64_t bd_ppr_max_size;
	atomic64_t bd_objreads;
	atomic64_t bd_objwrites;
	atomic64_t lru_pages;
#endif
#ifdef CONFIG_ZRAM_PERF_STAT
	struct zram_perf_stat perf_stat[NR_IO_TYPES];
#endif
};

#ifdef CONFIG_ZRAM_RAMPLUS
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
#define LRU_LIMIT_RATIO 3
#endif
#define ZRAM_WB_THRESHOLD 32
#define NR_ZWBS 64
#define NR_FALLOC_PAGES 512
#define FALLOC_ALIGN_MASK (~(NR_FALLOC_PAGES - 1))
#define ZWBS_ALIGN_MASK (~(NR_ZWBS - 1))
#define IDX_SHIFT (PAGE_SHIFT * 2)
#define MAX_REQ_IDX 2042
#define MIN_NR_POOL 8
#define MAX_NR_POOL 64

enum ramplus_type {
	PREFETCH,
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	LRU_WRITEBACK,
#endif
	WRITEBACK,
	POOL,
	NR_RAMPLUS_TYPES
};

struct zram_wb_header {
	u32 index;
	u32 size;
};

struct zram_wb_work {
	struct work_struct work;
	struct page *src_page[NR_ZWBS];
	struct page *dst_page;
	struct bio *bio;
	struct bio *bio_chain;
	struct zram_writeback_buffer *buf;
	struct zram *zram;
	struct list_head list;
	unsigned long handle;
	int nr_pages;
};

struct zram_wb_entry {
	unsigned long index;
	unsigned int offset;
	unsigned int size;
};

struct zwbs {
	struct zram_wb_entry entry[ZRAM_WB_THRESHOLD];
	struct page *page;
	u32 cnt;
	u32 off;
};

struct zram_writeback_buffer {
	struct zwbs *zwbs[NR_ZWBS];
	int idx;
};

/* 8kB */
struct zram_request {
	struct list_head list;
	int first;
	int last;
	u32 index[MAX_REQ_IDX];
};

struct zram_ramplus {
	struct task_struct *task;
	struct work_struct work;
	struct list_head list;
	wait_queue_head_t wait;
	spinlock_t lock;
	atomic_t nr;
	bool running;
};

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
static void zram_entry_move_list(struct zram *zram,
			struct list_head *list, u32 index);
static void try_wakeup_zram_lru_writebackd(struct zram *zram);
static void zram_reset_lru_entry(struct zram *zram, u32 index);
static void init_lru_writeback(struct zram *zram, u64 disksize);
#else
static void zram_entry_move_list(struct zram *zram,
			struct list_head *list, u32 index) {}
static void try_wakeup_zram_lru_writebackd(struct zram *zram) {}
static void zram_reset_lru_entry(struct zram *zram, u32 index) {}
static void init_lru_writeback(struct zram *zram, u64 disksize) {}
#endif
static void deinit_ramplus(struct zram *zram);
static int init_ramplus(struct zram *zram, unsigned long nr_pages);
#endif

struct zram {
	struct zram_table_entry *table;
	struct zs_pool *mem_pool;
	struct zcomp *comp;
	struct gendisk *disk;
	/* Prevent concurrent execution of device init */
	struct rw_semaphore init_lock;
	/*
	 * the number of pages zram can consume for storing compressed data
	 */
	unsigned long limit_pages;

	struct zram_stats stats;
	/*
	 * This is the limit on amount of *uncompressed* worth of data
	 * we can store in a disk.
	 */
	u64 disksize;	/* bytes */
	char compressor[CRYPTO_MAX_ALG_NAME];
	/*
	 * zram is claimed so open request will be failed
	 */
	bool claim; /* Protected by disk->open_mutex */
#ifdef CONFIG_ZRAM_WRITEBACK
	struct file *backing_dev;
	spinlock_t wb_limit_lock;
	bool wb_limit_enable;
	u64 bd_wb_limit;
	struct block_device *bdev;
	unsigned long *bitmap;
	unsigned long nr_pages;
#endif
#ifdef CONFIG_ZRAM_MEMORY_TRACKING
	struct dentry *debugfs_dir;
#endif
#ifdef CONFIG_ZRAM_RAMPLUS
	struct zram_ramplus ramplus[NR_RAMPLUS_TYPES];
	struct mutex blk_bitmap_lock;
	spinlock_t wb_table_lock;
	spinlock_t bitmap_lock;
	unsigned long *blk_bitmap;
	unsigned long *chunk_bitmap;
	unsigned long *read_req_bitmap;
	unsigned long nr_lru_pages;
	u16 *wb_table;
#endif
#ifdef CONFIG_ZRAM_PERF_STAT
	bool perf_stat_enabled;
#endif
};
#endif
