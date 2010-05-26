/****************************************************************************
 *  flashcache.h
 *  FlashCache: Device mapper target for block-level disk caching
 *
 *  Copyright 2010 Facebook, Inc.
 *  Author: Mohan Srinivasan (mohan@facebook.com)
 *
 *  Based on DM-Cache:
 *   Copyright (C) International Business Machines Corp., 2006
 *   Author: Ming Zhao (mingzhao@ufl.edu)
 * 
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#ifndef FLASHCACHE_H
#define FLASHCACHE_H

#define FLASHCACHE_VERSION		1

#define DEV_PATHLEN	128

#ifdef __KERNEL__

/* Like ASSERT() but always compiled in */

#define VERIFY(x) do { \
	if (unlikely(!(x))) { \
		dump_stack(); \
		panic("VERIFY: assertion (%s) failed at %s (%d)\n", \
		      #x,  __FILE__ , __LINE__);		    \
	} \
} while(0)

#define DMC_DEBUG 0
#define DMC_DEBUG_LITE 0

#define DM_MSG_PREFIX "flashcache"
#define DMC_PREFIX "flashcache: "

#if DMC_DEBUG
#define DPRINTK( s, arg... ) printk(DMC_PREFIX s "\n", ##arg)
#else
#define DPRINTK( s, arg... )
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
#define flashcache_bio_endio(BIO, ERROR)	bio_endio((BIO), (BIO)->bi_size, (ERROR))
#else
#define flashcache_bio_endio(BIO, ERROR)	bio_endio((BIO), (ERROR))
#endif

/*
 * Block checksums :
 * Block checksums seem a good idea (especially for debugging, I found a couple 
 * of bugs with this), but in practice there are a number of issues with this
 * in production.
 * 1) If a flash write fails, there is no guarantee that the failure was atomic.
 * Some sectors may have been written to flash. If so, the checksum we have
 * is wrong. We could re-read the flash block and recompute the checksum, but
 * the read could fail too. 
 * 2) On a node crash, we could have crashed between the flash data write and the
 * flash metadata update (which updates the new checksum to flash metadata). When
 * we reboot, the checksum we read from metadata is wrong. This is worked around
 * by having the cache load recompute checksums after an unclean shutdown.
 * 3) Checksums require 4 or 8 more bytes per block in terms of metadata overhead.
 * Especially because the metadata is wired into memory.
 * 4) Checksums force us to do a flash metadata IO on a block re-dirty. If we 
 * didn't maintain checksums, we could avoid the metadata IO on a re-dirty.
 * Therefore in production we disable block checksums.
 */
#if 0
#define FLASHCACHE_DO_CHECKSUMS
#endif

#if DMC_DEBUG_LITE
#define DPRINTK_LITE( s, arg... ) printk(DMC_PREFIX s "\n", ##arg)
#else
#define DPRINTK_LITE( s, arg... )
#endif

/* Number of pages for I/O */
#define FLASHCACHE_COPY_PAGES (1024)

/* Default cache parameters */
#define DEFAULT_CACHE_SIZE	65536
#define DEFAULT_CACHE_ASSOC	512
#define DEFAULT_BLOCK_SIZE	32	/* 16 KB */
#define CONSECUTIVE_BLOCKS	512

#define FLASHCACHE_FIFO		0
#define FLASHCACHE_LRU		1

/* States of a cache block */
#define INVALID			0x0001
#define VALID			0x0002	/* Valid */
#define DISKREADINPROG		0x0004	/* Read from disk in progress */
#define DISKWRITEINPROG		0x0008	/* Write to disk in progress */
#define CACHEREADINPROG		0x0010	/* Read from cache in progress */
#define CACHEWRITEINPROG	0x0020	/* Write to cache in progress */
#define DIRTY			0x0040	/* Dirty, needs writeback to disk */

#define BLOCK_IO_INPROG	(DISKREADINPROG | DISKWRITEINPROG | CACHEREADINPROG | CACHEWRITEINPROG)

/*
 * The LRU pointers are maintained as set-relative offsets, instead of 
 * pointers. This enables us to store the LRU pointers per cacheblock
 * using 4 bytes instead of 16 bytes. The upshot of this is that we 
 * are required to clamp the associativity at an 8K max.
 */
#define FLASHCACHE_MAX_ASSOC	8192
#define FLASHCACHE_LRU_NULL	0xFFFF

/* Cache block metadata structure */
struct cacheblock {
	u_int16_t	cache_state;
	int16_t 	nr_queued;	/* jobs in pending queue */
	u_int16_t	lru_prev, lru_next;
	sector_t 	dbn;	/* Sector number of the cached block */
#ifdef FLASHCACHE_DO_CHECKSUMS
	u_int64_t 	checksum;
#endif
	struct pending_job *head;
};

struct cache_set {
	u_int32_t		set_fifo_next;
	u_int32_t		set_clean_next;
	u_int32_t		clean_inprog;
	u_int32_t		nr_dirty;
	u_int16_t		lru_head, lru_tail;
};

/*
 * Cache context
 */
struct cache_c {
	struct dm_target	*tgt;
	
	struct dm_dev 		*disk_dev;   /* Source device */
	struct dm_dev 		*cache_dev; /* Cache device */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
	struct kcopyd_client *kcp_client; /* Kcopyd client for writing back data */
#else
	struct dm_kcopyd_client *kcp_client; /* Kcopyd client for writing back data */
	struct dm_io_client *io_client; /* Client memory pool*/
#endif

	spinlock_t		cache_spin_lock;

	struct cacheblock	*cache;	/* Hash table for cache blocks */
	struct cache_set	*cache_sets;
	struct cache_md_sector_head *md_sectors_buf;
	
	sector_t size;			/* Cache size */
	unsigned int assoc;		/* Cache associativity */
	unsigned int block_size;	/* Cache block size */
	unsigned int block_shift;	/* Cache block size in bits */
	unsigned int block_mask;	/* Cache block mask */
	unsigned int consecutive_shift;	/* Consecutive blocks size in bits */

	wait_queue_head_t destroyq;	/* Wait queue for I/O completion */
	/* XXX - Updates of nr_jobs should happen inside the lock. But doing it outside
	   is OK since the filesystem is unmounted at this point */
	atomic_t nr_jobs;		/* Number of I/O jobs */
	atomic_t fast_remove_in_prog;

	int	dirty_thresh_set;	/* Per set dirty threshold to start cleaning */
	int	max_clean_ios_set;	/* Max cleaning IOs per set */
	int	max_clean_ios_total;	/* Total max cleaning IOs */
	int	clean_inprog;
	int	sync_index;
	int	nr_dirty;

	int	md_sectors;		/* Numbers of metadata sectors, including header */

	/* Stats */
	unsigned long reads;		/* Number of reads */
	unsigned long writes;		/* Number of writes */
	unsigned long read_hits;	/* Number of cache hits */
	unsigned long write_hits;	/* Number of write hits (includes dirty write hits) */
	unsigned long dirty_write_hits;	/* Number of "dirty" write hits */
	unsigned long replace;		/* Number of cache replacements */
	unsigned long wr_replace;
	unsigned long wr_invalidates;	/* Number of write invalidations */
	unsigned long rd_invalidates;	/* Number of read invalidations */
	unsigned long pending_inval;	/* Invalidations due to concurrent ios on same block */
	unsigned long cached_blocks;	/* Number of cached blocks */
#ifdef FLASHCACHE_DO_CHECKSUMS
	unsigned long checksum_store;
	unsigned long checksum_valid;
	unsigned long checksum_invalid;
#endif
	unsigned long enqueues;		/* enqueues on pending queue */
	unsigned long cleanings;
	unsigned long noroom;		/* No room in set */
	unsigned long md_write_dirty;	/* Metadata sector writes dirtying block */
	unsigned long md_write_clean;	/* Metadata sector writes cleaning block */
	unsigned long nc_pid_drops;
	unsigned long nc_pid_adds;
	unsigned long nc_pid_dels;
	unsigned long nc_expiry;
	unsigned long front_merge, back_merge;	/* Write Merging */

	unsigned long clean_set_calls;
	unsigned long clean_set_less_dirty;
	unsigned long clean_set_fails;
	unsigned long clean_set_ios;
	unsigned long set_limit_reached;
	unsigned long total_limit_reached;

	/* Errors */
	int	disk_read_errors;
	int	disk_write_errors;
	int	ssd_read_errors;
	int	ssd_write_errors;
	int	memory_alloc_errors;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	struct work_struct delayed_clean;
#else
	struct delayed_work delayed_clean;
#endif

	struct flashcache_non_cacheable_pid *nc_pid_list_head, *nc_pid_list_tail;
	int num_nc_pids;
	unsigned long nc_pid_expire_check;

	struct cache_c	*next_cache;

	char cache_devname[DEV_PATHLEN];
	char disk_devname[DEV_PATHLEN];
};

/* kcached/pending job states */
#define READCACHE	1
#define WRITECACHE	2
#define READDISK	3
#define WRITEDISK	4
#define READFILL	5	/* Read Cache Miss Fill */
#define INVALIDATE	6
#define WRITEDISK_SYNC	7

struct kcached_job {
	struct list_head list;
	struct cache_c *dmc;
	struct bio *bio;	/* Original bio */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
	struct io_region disk;
	struct io_region cache;
#else
	struct dm_io_region disk;
	struct dm_io_region cache;
#endif
	int    index;
	int    action;
	int 	error;
	struct flash_cacheblock *md_sector;
	struct bio_vec md_io_bvec;
	struct kcached_job *next;
};

struct pending_job {
	struct bio *bio;
	int    action;	
	struct pending_job *next;
};
#endif /* __KERNEL__ */

/* States of a cache block */
#define INVALID			0x0001
#define VALID			0x0002	/* Valid */
#define DISKREADINPROG		0x0004	/* Read from disk in progress */
#define DISKWRITEINPROG		0x0008	/* Write to disk in progress */
#define CACHEREADINPROG		0x0010	/* Read from cache in progress */
#define CACHEWRITEINPROG	0x0020	/* Write to cache in progress */
#define DIRTY			0x0040	/* Dirty, needs writeback to disk */

#define BLOCK_IO_INPROG	(DISKREADINPROG | DISKWRITEINPROG | CACHEREADINPROG | CACHEWRITEINPROG)

/* Cache metadata is read by Flashcache utilities */
#ifndef __KERNEL__
typedef u_int64_t sector_t;
#endif

/* On Flash (cache metadata) Structures */
#define CACHE_MD_STATE_DIRTY		0xdeadbeef
#define CACHE_MD_STATE_CLEAN		0xfacecafe
#define CACHE_MD_STATE_FASTCLEAN	0xcafefeed
#define CACHE_MD_STATE_UNSTABLE		0xc8249756

struct flash_superblock {
	sector_t size;		/* Cache size */
	u_int32_t block_size;	/* Cache block size */
	u_int32_t assoc;	/* Cache associativity */
	u_int32_t cache_sb_state;	/* Clean shutdown ? */
	char cache_devname[DEV_PATHLEN];
	sector_t cache_devsize;
	char disk_devname[DEV_PATHLEN];
	sector_t disk_devsize;
	u_int32_t cache_version;
};

/* 
 * We do metadata updates only when a block trasitions from DIRTY -> CLEAN
 * or from CLEAN -> DIRTY. Consequently, on an unclean shutdown, we only
 * pick up blocks that are marked (DIRTY | CLEAN), we clean these and stick
 * them in the cache.
 * On a clean shutdown, we will sync the state for every block, and we will
 * load every block back into cache on a restart.
 */
struct flash_cacheblock {
	sector_t 	dbn;	/* Sector number of the cached block */
#ifdef FLASHCACHE_DO_CHECKSUMS
	u_int64_t 	checksum;
#endif
	u_int32_t	cache_state; /* INVALID | VALID | DIRTY */
};

#define MD_BLOCKS_PER_SECTOR		(512 / (sizeof(struct flash_cacheblock)))
#define INDEX_TO_MD_SECTOR(INDEX)	((INDEX) / MD_BLOCKS_PER_SECTOR)
#define INDEX_TO_MD_SECTOR_OFFSET(INDEX)	((INDEX) % MD_BLOCKS_PER_SECTOR)

#ifdef __KERNEL__

/* Cache persistence */
#define CACHE_RELOAD		1
#define CACHE_CREATE		2
#define CACHE_FORCECREATE	3

/* 
 * We have one of these for *every* cache metadata sector, to keep track
 * of metadata ios in progress for blocks covered in this sector. Only
 * one metadata IO per sector can be in progress at any given point in 
 * time
 */
struct cache_md_sector_head {
	u_int32_t		nr_in_prog;
	struct kcached_job	*pending_jobs, *md_io_inprog;
};

#define MIN_JOBS 1024

enum {
	FLASHCACHE_WB_SYNC=1,
	FLASHCACHE_WB_DIRTY_THRESH=2,
	FLASHCACHE_WB_DEBUG=3,
	FLASHCACHE_WB_MAX_CLEAN_IOS_SET=4,
	FLASHCACHE_WB_MAX_CLEAN_IOS_TOTAL=5,
	FLASHCACHE_WB_MAX_NC_PIDS=6,
	FLASHCACHE_WB_MAX_NC_PID_EXPIRY=7,
	FLASHCACHE_WB_DO_NC_EXPIRY=8,
	FLASHCACHE_WB_RECLAIM_POLICY=9,
	FLASHCACHE_WB_WRITE_MERGE=10,
	FLASHCACHE_WB_ZERO_STATS=11,
	FLASHCACHE_WB_ERROR_INJECT=12,
	FLASHCACHE_WB_DO_FAST_REMOVE=13,
	FLASHCACHE_WB_STOP_SYNC=14,
};

#define DIRTY_THRESH_MIN	10
#define DIRTY_THRESH_MAX	90
#define DIRTY_THRESH_DEF	20

/* DM async IO mempool sizing */
#define FLASHCACHE_ASYNC_SIZE 1024

struct flashcache_non_cacheable_pid {
	pid_t					pid;
	struct flashcache_non_cacheable_pid	*next, *prev;
	unsigned long				expiry;
};

struct dbn_index_pair {
	sector_t	dbn;
	int		index;
};

/* Error injection flags */
#define READDISK_ERROR				0x00000001
#define READCACHE_ERROR				0x00000002
#define READFILL_ERROR				0x00000004
#define WRITECACHE_ERROR			0x00000008
#define WRITECACHE_MD_ERROR			0x00000010
#define WRITEDISK_MD_ERROR			0x00000020
#define KCOPYD_CALLBACK_ERROR			0x00000040
#define DIRTY_WRITEBACK_JOB_ALLOC_FAIL		0x00000080
#define READ_MISS_JOB_ALLOC_FAIL		0x00000100
#define READ_HIT_JOB_ALLOC_FAIL			0x00000200
#define READ_HIT_PENDING_JOB_ALLOC_FAIL		0x00000400
#define INVAL_PENDING_JOB_ALLOC_FAIL		0x00000800
#define WRITE_HIT_JOB_ALLOC_FAIL		0x00001000
#define WRITE_HIT_PENDING_JOB_ALLOC_FAIL	0x00002000
#define WRITE_MISS_JOB_ALLOC_FAIL		0x00004000
#define WRITES_LIST_ALLOC_FAIL			0x00008000
#define MD_ALLOC_SECTOR_ERROR			0x00010000

/* Inject a 5s delay between syncing blocks and metadata */
#define FLASHCACHE_SYNC_REMOVE_DELAY		5000

int flashcache_map(struct dm_target *ti, struct bio *bio,
		   union map_info *map_context);
int flashcache_ctr(struct dm_target *ti, unsigned int argc,
		   char **argv);
void flashcache_dtr(struct dm_target *ti);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,27)
int flashcache_ioctl(struct dm_target *ti, struct inode *inode,
		     struct file *filp, unsigned int cmd,
		     unsigned long arg);
#else
int flashcache_ioctl(struct dm_target *ti, unsigned int cmd,
 		     unsigned long arg);
#endif

int flashcache_status(struct dm_target *ti, status_type_t type,
		      char *result, unsigned int maxlen);
struct kcached_job *flashcache_alloc_cache_job(void);
void flashcache_free_cache_job(struct kcached_job *job);
struct pending_job *flashcache_alloc_pending_job(struct cache_c *dmc);
void flashcache_free_pending_job(struct pending_job *job);
#ifdef FLASHCACHE_DO_CHECKSUMS
u_int64_t flashcache_compute_checksum(struct bio *bio);
void flashcache_store_checksum(struct kcached_job *job);
int flashcache_validate_checksum(struct kcached_job *job);
int flashcache_read_compute_checksum(struct cache_c *dmc, int index, void *block);
#endif
struct kcached_job *pop(struct list_head *jobs);
void push(struct list_head *jobs, struct kcached_job *job);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
void do_work(void *unused);
#else
void do_work(struct work_struct *unused);
#endif
struct kcached_job *new_kcached_job(struct cache_c *dmc, struct bio* bio,
				    int index);
void push_pending(struct kcached_job *job);
void push_io(struct kcached_job *job);
void push_md_io(struct kcached_job *job);
void push_md_complete(struct kcached_job *job);
int flashcache_pending_empty(void);
int flashcache_io_empty(void);
int flashcache_md_io_empty(void);
int flashcache_md_complete_empty(void);
void flashcache_md_write_done(struct kcached_job *job);
void flashcache_do_pending(struct kcached_job *job);
void flashcache_md_write(struct kcached_job *job);
void flashcache_do_io(struct kcached_job *job);
void flashcache_clean_set(struct cache_c *dmc, int set);
void flashcache_sync_all(struct cache_c *dmc);
void flashcache_del_nc_all(struct cache_c *dmc);
void flashcache_reclaim_lru_movetail(struct cache_c *dmc, int index);
void flashcache_merge_writes(struct cache_c *dmc, 
			     struct dbn_index_pair *writes_list, 
			     int *nr_writes, int set);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
int flashcache_dm_io_sync_vm(struct io_region *where, int rw, void *data);
#else
int flashcache_dm_io_sync_vm(struct cache_c *dmc, struct dm_io_region *where, 
			     int rw, void *data);
#endif
void flashcache_update_sync_progress(struct cache_c *dmc);
void flashcache_unplug_device(struct block_device *bdev);
#endif /* __KERNEL__ */

#endif
