/*
 * write.c
 *
 * Support for writing WIM files; write a WIM file, overwrite a WIM file, write
 * compressed file resources, etc.
 */

/*
 * Copyright (C) 2012, 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#include "config.h"

#if defined(HAVE_SYS_FILE_H) && defined(HAVE_FLOCK)
/* On BSD, this should be included before "list.h" so that "list.h" can
 * overwrite the LIST_HEAD macro. */
#  include <sys/file.h>
#endif

#ifdef __WIN32__
#  include "win32.h"
#endif

#include "list.h"
#include "wimlib_internal.h"
#include "buffer_io.h"
#include "dentry.h"
#include "lookup_table.h"
#include "xml.h"
#include "lzx.h"
#include "xpress.h"


#ifdef ENABLE_MULTITHREADED_COMPRESSION
#  include <pthread.h>
#endif

#include <unistd.h>
#include <errno.h>

#ifdef WITH_NTFS_3G
#  include <time.h>
#  include <ntfs-3g/attrib.h>
#  include <ntfs-3g/inode.h>
#  include <ntfs-3g/dir.h>
#endif

#ifdef HAVE_ALLOCA_H
#  include <alloca.h>
#else
#  include <stdlib.h>
#endif

#include <limits.h>

#if defined(__WIN32__) && !defined(INVALID_HANDLE_VALUE)
#  define INVALID_HANDLE_VALUE ((HANDLE)(-1))
#endif

static int
fflush_and_ftruncate(FILE *fp, off_t size)
{
	int ret;

	ret = fflush(fp);
	if (ret != 0) {
		ERROR_WITH_ERRNO("Failed to flush data to output WIM file");
		return WIMLIB_ERR_WRITE;
	}
	ret = ftruncate(fileno(fp), size);
	if (ret != 0) {
		ERROR_WITH_ERRNO("Failed to truncate output WIM file to "
				 "%"PRIu64" bytes", size);
		return WIMLIB_ERR_WRITE;
	}
	return 0;
}

/* Chunk table that's located at the beginning of each compressed resource in
 * the WIM.  (This is not the on-disk format; the on-disk format just has an
 * array of offsets.) */
struct chunk_table {
	off_t file_offset;
	u64 num_chunks;
	u64 original_resource_size;
	u64 bytes_per_chunk_entry;
	u64 table_disk_size;
	u64 cur_offset;
	u64 *cur_offset_p;
	u64 offsets[0];
};

/*
 * Allocates and initializes a chunk table, and reserves space for it in the
 * output file.
 */
static int
begin_wim_resource_chunk_tab(const struct wim_lookup_table_entry *lte,
			     FILE *out_fp,
			     off_t file_offset,
			     struct chunk_table **chunk_tab_ret)
{
	u64 size = wim_resource_size(lte);
	u64 num_chunks = (size + WIM_CHUNK_SIZE - 1) / WIM_CHUNK_SIZE;
	size_t alloc_size = sizeof(struct chunk_table) + num_chunks * sizeof(u64);
	struct chunk_table *chunk_tab = CALLOC(1, alloc_size);
	int ret;

	if (!chunk_tab) {
		ERROR("Failed to allocate chunk table for %"PRIu64" byte "
		      "resource", size);
		ret = WIMLIB_ERR_NOMEM;
		goto out;
	}
	chunk_tab->file_offset = file_offset;
	chunk_tab->num_chunks = num_chunks;
	chunk_tab->original_resource_size = size;
	chunk_tab->bytes_per_chunk_entry = (size >= (1ULL << 32)) ? 8 : 4;
	chunk_tab->table_disk_size = chunk_tab->bytes_per_chunk_entry *
				     (num_chunks - 1);
	chunk_tab->cur_offset = 0;
	chunk_tab->cur_offset_p = chunk_tab->offsets;

	if (fwrite(chunk_tab, 1, chunk_tab->table_disk_size, out_fp) !=
		   chunk_tab->table_disk_size) {
		ERROR_WITH_ERRNO("Failed to write chunk table in compressed "
				 "file resource");
		ret = WIMLIB_ERR_WRITE;
		goto out;
	}

	ret = 0;
out:
	*chunk_tab_ret = chunk_tab;
	return ret;
}

/*
 * Pointer to function to compresses a chunk of a WIM resource.
 *
 * @chunk:		Uncompressed data of the chunk.
 * @chunk_size:		Size of the uncompressed chunk in bytes.
 * @compressed_chunk:	Pointer to output buffer of size at least
 * 				(@chunk_size - 1) bytes.
 * @compressed_chunk_len_ret:	Pointer to an unsigned int into which the size
 * 					of the compressed chunk will be
 * 					returned.
 *
 * Returns zero if compressed succeeded, and nonzero if the chunk could not be
 * compressed to any smaller than @chunk_size.  This function cannot fail for
 * any other reasons.
 */
typedef int (*compress_func_t)(const void *, unsigned, void *, unsigned *);

compress_func_t
get_compress_func(int out_ctype)
{
	if (out_ctype == WIMLIB_COMPRESSION_TYPE_LZX)
		return lzx_compress;
	else
		return xpress_compress;
}

/*
 * Writes a chunk of a WIM resource to an output file.
 *
 * @chunk:	  Uncompressed data of the chunk.
 * @chunk_size:	  Size of the chunk (<= WIM_CHUNK_SIZE)
 * @out_fp:	  FILE * to write tho chunk to.
 * @out_ctype:	  Compression type to use when writing the chunk (ignored if no
 * 			chunk table provided)
 * @chunk_tab:	  Pointer to chunk table being created.  It is updated with the
 * 			offset of the chunk we write.
 *
 * Returns 0 on success; nonzero on failure.
 */
static int
write_wim_resource_chunk(const u8 chunk[], unsigned chunk_size,
			 FILE *out_fp, compress_func_t compress,
			 struct chunk_table *chunk_tab)
{
	const u8 *out_chunk;
	unsigned out_chunk_size;
	if (chunk_tab) {
		u8 *compressed_chunk = alloca(chunk_size);
		int ret;

		ret = compress(chunk, chunk_size, compressed_chunk,
			       &out_chunk_size);
		if (ret == 0) {
			out_chunk = compressed_chunk;
		} else {
			out_chunk = chunk;
			out_chunk_size = chunk_size;
		}
		*chunk_tab->cur_offset_p++ = chunk_tab->cur_offset;
		chunk_tab->cur_offset += out_chunk_size;
	} else {
		out_chunk = chunk;
		out_chunk_size = chunk_size;
	}
	if (fwrite(out_chunk, 1, out_chunk_size, out_fp) != out_chunk_size) {
		ERROR_WITH_ERRNO("Failed to write WIM resource chunk");
		return WIMLIB_ERR_WRITE;
	}
	return 0;
}

/*
 * Finishes a WIM chunk table and writes it to the output file at the correct
 * offset.
 *
 * The final size of the full compressed resource is returned in the
 * @compressed_size_p.
 */
static int
finish_wim_resource_chunk_tab(struct chunk_table *chunk_tab,
			      FILE *out_fp, u64 *compressed_size_p)
{
	size_t bytes_written;
	if (fseeko(out_fp, chunk_tab->file_offset, SEEK_SET) != 0) {
		ERROR_WITH_ERRNO("Failed to seek to byte %"PRIu64" of output "
				 "WIM file", chunk_tab->file_offset);
		return WIMLIB_ERR_WRITE;
	}

	if (chunk_tab->bytes_per_chunk_entry == 8) {
		array_cpu_to_le64(chunk_tab->offsets, chunk_tab->num_chunks);
	} else {
		for (u64 i = 0; i < chunk_tab->num_chunks; i++)
			((u32*)chunk_tab->offsets)[i] =
				cpu_to_le32(chunk_tab->offsets[i]);
	}
	bytes_written = fwrite((u8*)chunk_tab->offsets +
					chunk_tab->bytes_per_chunk_entry,
			       1, chunk_tab->table_disk_size, out_fp);
	if (bytes_written != chunk_tab->table_disk_size) {
		ERROR_WITH_ERRNO("Failed to write chunk table in compressed "
				 "file resource");
		return WIMLIB_ERR_WRITE;
	}
	if (fseeko(out_fp, 0, SEEK_END) != 0) {
		ERROR_WITH_ERRNO("Failed to seek to end of output WIM file");
		return WIMLIB_ERR_WRITE;
	}
	*compressed_size_p = chunk_tab->cur_offset + chunk_tab->table_disk_size;
	return 0;
}

/* Prepare for multiple reads to a resource by caching a FILE * or NTFS
 * attribute pointer in the lookup table entry. */
static int
prepare_resource_for_read(struct wim_lookup_table_entry *lte

				#ifdef WITH_NTFS_3G
				, ntfs_inode **ni_ret
				#endif
			)
{
	switch (lte->resource_location) {
	case RESOURCE_IN_FILE_ON_DISK:
		if (!lte->file_on_disk_fp) {
			lte->file_on_disk_fp = fopen(lte->file_on_disk, "rb");
			if (!lte->file_on_disk_fp) {
				ERROR_WITH_ERRNO("Failed to open the file "
						 "`%s'", lte->file_on_disk);
				return WIMLIB_ERR_OPEN;
			}
		}
		break;
#ifdef WITH_NTFS_3G
	case RESOURCE_IN_NTFS_VOLUME:
		if (!lte->attr) {
			struct ntfs_location *loc = lte->ntfs_loc;
			ntfs_inode *ni;
			wimlib_assert(loc);
			ni = ntfs_pathname_to_inode(*loc->ntfs_vol_p, NULL, loc->path);
			if (!ni) {
				ERROR_WITH_ERRNO("Failed to open inode `%s' in NTFS "
						 "volume", loc->path);
				return WIMLIB_ERR_NTFS_3G;
			}
			lte->attr = ntfs_attr_open(ni,
						   loc->is_reparse_point ? AT_REPARSE_POINT : AT_DATA,
						   loc->stream_name,
						   loc->stream_name_nchars);
			if (!lte->attr) {
				ERROR_WITH_ERRNO("Failed to open attribute of `%s' in "
						 "NTFS volume", loc->path);
				ntfs_inode_close(ni);
				return WIMLIB_ERR_NTFS_3G;
			}
			*ni_ret = ni;
		}
		break;
#endif
#ifdef __WIN32__
	case RESOURCE_WIN32:
		if (lte->win32_file_on_disk_fp == INVALID_HANDLE_VALUE) {
			lte->win32_file_on_disk_fp =
				win32_open_file_data_only(lte->win32_file_on_disk);
			if (lte->win32_file_on_disk_fp == INVALID_HANDLE_VALUE) {
				ERROR("Win32 API: Can't open %ls", lte->win32_file_on_disk);
				win32_error_last();
				return WIMLIB_ERR_OPEN;
			}
		}
		break;
#endif
	default:
		break;
	}
	return 0;
}

/* Undo prepare_resource_for_read() by closing the cached FILE * or NTFS
 * attribute. */
static void
end_wim_resource_read(struct wim_lookup_table_entry *lte
			#ifdef WITH_NTFS_3G
				, ntfs_inode *ni
			#endif
			)
{
	if (lte->resource_location == RESOURCE_IN_FILE_ON_DISK
	    && lte->file_on_disk_fp)
	{
		fclose(lte->file_on_disk_fp);
		lte->file_on_disk_fp = NULL;
	}
#ifdef WITH_NTFS_3G
	else if (lte->resource_location == RESOURCE_IN_NTFS_VOLUME) {
		if (lte->attr) {
			ntfs_attr_close(lte->attr);
			lte->attr = NULL;
		}
		if (ni)
			ntfs_inode_close(ni);
	}
#endif
#ifdef __WIN32__
	else if (lte->resource_location == RESOURCE_WIN32
		 && lte->win32_file_on_disk_fp != INVALID_HANDLE_VALUE)
	{
		win32_close_file(lte->win32_file_on_disk_fp);
		lte->win32_file_on_disk_fp = INVALID_HANDLE_VALUE;
	}
#endif
}

static int
write_uncompressed_resource_and_truncate(struct wim_lookup_table_entry *lte,
					 FILE *out_fp,
					 off_t file_offset,
					 struct resource_entry *out_res_entry)
{
	int ret;
	if (fseeko(out_fp, file_offset, SEEK_SET) != 0) {
		ERROR_WITH_ERRNO("Failed to seek to byte %"PRIu64" of "
				 "output WIM file", file_offset);
		return WIMLIB_ERR_WRITE;
	}
	ret = write_wim_resource(lte, out_fp, WIMLIB_COMPRESSION_TYPE_NONE,
				 out_res_entry, 0);
	if (ret != 0)
		return ret;

	return fflush_and_ftruncate(out_fp,
				    file_offset + wim_resource_size(lte));
}

/*
 * Writes a WIM resource to a FILE * opened for writing.  The resource may be
 * written uncompressed or compressed depending on the @out_ctype parameter.
 *
 * If by chance the resource compresses to more than the original size (this may
 * happen with random data or files than are pre-compressed), the resource is
 * instead written uncompressed (and this is reflected in the @out_res_entry by
 * removing the WIM_RESHDR_FLAG_COMPRESSED flag).
 *
 * @lte:	The lookup table entry for the WIM resource.
 * @out_fp:	The FILE * to write the resource to.
 * @out_ctype:  The compression type of the resource to write.  Note: if this is
 * 			the same as the compression type of the WIM resource we
 * 			need to read, we simply copy the data (i.e. we do not
 * 			uncompress it, then compress it again).
 * @out_res_entry:  If non-NULL, a resource entry that is filled in with the
 * 		    offset, original size, compressed size, and compression flag
 * 		    of the output resource.
 *
 * Returns 0 on success; nonzero on failure.
 */
int
write_wim_resource(struct wim_lookup_table_entry *lte,
		   FILE *out_fp, int out_ctype,
		   struct resource_entry *out_res_entry,
		   int flags)
{
	u64 bytes_remaining;
	u64 original_size;
	u64 old_compressed_size;
	u64 new_compressed_size;
	u64 offset;
	int ret;
	struct chunk_table *chunk_tab = NULL;
	bool raw;
	off_t file_offset;
	compress_func_t compress = NULL;
#ifdef WITH_NTFS_3G
	ntfs_inode *ni = NULL;
#endif

	wimlib_assert(lte);

	/* Original size of the resource */
 	original_size = wim_resource_size(lte);

	/* Compressed size of the resource (as it exists now) */
	old_compressed_size = wim_resource_compressed_size(lte);

	/* Current offset in output file */
	file_offset = ftello(out_fp);
	if (file_offset == -1) {
		ERROR_WITH_ERRNO("Failed to get offset in output "
				 "stream");
		return WIMLIB_ERR_WRITE;
	}

	/* Are the compression types the same?  If so, do a raw copy (copy
	 * without decompressing and recompressing the data). */
	raw = (wim_resource_compression_type(lte) == out_ctype
	       && out_ctype != WIMLIB_COMPRESSION_TYPE_NONE
	       && !(flags & WIMLIB_RESOURCE_FLAG_RECOMPRESS));

	if (raw) {
		flags |= WIMLIB_RESOURCE_FLAG_RAW;
		bytes_remaining = old_compressed_size;
	} else {
		flags &= ~WIMLIB_RESOURCE_FLAG_RAW;
		bytes_remaining = original_size;
	}

	/* Empty resource; nothing needs to be done, so just return success. */
	if (bytes_remaining == 0)
		return 0;

	/* Buffer for reading chunks for the resource */
	u8 buf[min(WIM_CHUNK_SIZE, bytes_remaining)];

	/* If we are writing a compressed resource and not doing a raw copy, we
	 * need to initialize the chunk table */
	if (out_ctype != WIMLIB_COMPRESSION_TYPE_NONE && !raw) {
		ret = begin_wim_resource_chunk_tab(lte, out_fp, file_offset,
						   &chunk_tab);
		if (ret != 0)
			goto out;
	}

	/* If the WIM resource is in an external file, open a FILE * to it so we
	 * don't have to open a temporary one in read_wim_resource() for each
	 * chunk. */
#ifdef WITH_NTFS_3G
	ret = prepare_resource_for_read(lte, &ni);
#else
	ret = prepare_resource_for_read(lte);
#endif
	if (ret != 0)
		goto out;

	/* If we aren't doing a raw copy, we will compute the SHA1 message
	 * digest of the resource as we read it, and verify it's the same as the
	 * hash given in the lookup table entry once we've finished reading the
	 * resource. */
	SHA_CTX ctx;
	if (!raw) {
		sha1_init(&ctx);
		compress = get_compress_func(out_ctype);
	}
	offset = 0;

	/* While there are still bytes remaining in the WIM resource, read a
	 * chunk of the resource, update SHA1, then write that chunk using the
	 * desired compression type. */
	do {
		u64 to_read = min(bytes_remaining, WIM_CHUNK_SIZE);
		ret = read_wim_resource(lte, buf, to_read, offset, flags);
		if (ret != 0)
			goto out_fclose;
		if (!raw)
			sha1_update(&ctx, buf, to_read);
		ret = write_wim_resource_chunk(buf, to_read, out_fp,
					       compress, chunk_tab);
		if (ret != 0)
			goto out_fclose;
		bytes_remaining -= to_read;
		offset += to_read;
	} while (bytes_remaining);

	/* Raw copy:  The new compressed size is the same as the old compressed
	 * size
	 *
	 * Using WIMLIB_COMPRESSION_TYPE_NONE:  The new compressed size is the
	 * original size
	 *
	 * Using a different compression type:  Call
	 * finish_wim_resource_chunk_tab() and it will provide the new
	 * compressed size.
	 */
	if (raw) {
		new_compressed_size = old_compressed_size;
	} else {
		if (out_ctype == WIMLIB_COMPRESSION_TYPE_NONE)
			new_compressed_size = original_size;
		else {
			ret = finish_wim_resource_chunk_tab(chunk_tab, out_fp,
							    &new_compressed_size);
			if (ret != 0)
				goto out_fclose;
		}
	}

	/* Verify SHA1 message digest of the resource, unless we are doing a raw
	 * write (in which case we never even saw the uncompressed data).  Or,
	 * if the hash we had before is all 0's, just re-set it to be the new
	 * hash. */
	if (!raw) {
		u8 md[SHA1_HASH_SIZE];
		sha1_final(md, &ctx);
		if (is_zero_hash(lte->hash)) {
			copy_hash(lte->hash, md);
		} else if (!hashes_equal(md, lte->hash)) {
			ERROR("WIM resource has incorrect hash!");
			if (lte->resource_location == RESOURCE_IN_FILE_ON_DISK) {
				ERROR("We were reading it from `%s'; maybe it changed "
				      "while we were reading it.",
				      lte->file_on_disk);
			}
			ret = WIMLIB_ERR_INVALID_RESOURCE_HASH;
			goto out_fclose;
		}
	}

	if (!raw && new_compressed_size >= original_size &&
	    out_ctype != WIMLIB_COMPRESSION_TYPE_NONE)
	{
		/* Oops!  We compressed the resource to larger than the original
		 * size.  Write the resource uncompressed instead. */
		ret = write_uncompressed_resource_and_truncate(lte,
							       out_fp,
							       file_offset,
							       out_res_entry);
		if (ret != 0)
			goto out_fclose;
	} else {
		if (out_res_entry) {
			out_res_entry->size          = new_compressed_size;
			out_res_entry->original_size = original_size;
			out_res_entry->offset        = file_offset;
			out_res_entry->flags         = lte->resource_entry.flags
							& ~WIM_RESHDR_FLAG_COMPRESSED;
			if (out_ctype != WIMLIB_COMPRESSION_TYPE_NONE)
				out_res_entry->flags |= WIM_RESHDR_FLAG_COMPRESSED;
		}
	}
	ret = 0;
out_fclose:
#ifdef WITH_NTFS_3G
	end_wim_resource_read(lte, ni);
#else
	end_wim_resource_read(lte);
#endif
out:
	FREE(chunk_tab);
	return ret;
}

#ifdef ENABLE_MULTITHREADED_COMPRESSION

/* Blocking shared queue (solves the producer-consumer problem) */
struct shared_queue {
	unsigned size;
	unsigned front;
	unsigned back;
	unsigned filled_slots;
	void **array;
	pthread_mutex_t lock;
	pthread_cond_t msg_avail_cond;
	pthread_cond_t space_avail_cond;
};

static int
shared_queue_init(struct shared_queue *q, unsigned size)
{
	wimlib_assert(size != 0);
	q->array = CALLOC(sizeof(q->array[0]), size);
	if (!q->array)
		return WIMLIB_ERR_NOMEM;
	q->filled_slots = 0;
	q->front = 0;
	q->back = size - 1;
	q->size = size;
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->msg_avail_cond, NULL);
	pthread_cond_init(&q->space_avail_cond, NULL);
	return 0;
}

static void
shared_queue_destroy(struct shared_queue *q)
{
	FREE(q->array);
	pthread_mutex_destroy(&q->lock);
	pthread_cond_destroy(&q->msg_avail_cond);
	pthread_cond_destroy(&q->space_avail_cond);
}

static void
shared_queue_put(struct shared_queue *q, void *obj)
{
	pthread_mutex_lock(&q->lock);
	while (q->filled_slots == q->size)
		pthread_cond_wait(&q->space_avail_cond, &q->lock);

	q->back = (q->back + 1) % q->size;
	q->array[q->back] = obj;
	q->filled_slots++;

	pthread_cond_broadcast(&q->msg_avail_cond);
	pthread_mutex_unlock(&q->lock);
}

static void *
shared_queue_get(struct shared_queue *q)
{
	void *obj;

	pthread_mutex_lock(&q->lock);
	while (q->filled_slots == 0)
		pthread_cond_wait(&q->msg_avail_cond, &q->lock);

	obj = q->array[q->front];
	q->array[q->front] = NULL;
	q->front = (q->front + 1) % q->size;
	q->filled_slots--;

	pthread_cond_broadcast(&q->space_avail_cond);
	pthread_mutex_unlock(&q->lock);
	return obj;
}

struct compressor_thread_params {
	struct shared_queue *res_to_compress_queue;
	struct shared_queue *compressed_res_queue;
	compress_func_t compress;
};

#define MAX_CHUNKS_PER_MSG 2

struct message {
	struct wim_lookup_table_entry *lte;
	u8 *uncompressed_chunks[MAX_CHUNKS_PER_MSG];
	u8 *out_compressed_chunks[MAX_CHUNKS_PER_MSG];
	u8 *compressed_chunks[MAX_CHUNKS_PER_MSG];
	unsigned uncompressed_chunk_sizes[MAX_CHUNKS_PER_MSG];
	unsigned compressed_chunk_sizes[MAX_CHUNKS_PER_MSG];
	unsigned num_chunks;
	struct list_head list;
	bool complete;
	u64 begin_chunk;
};

static void
compress_chunks(struct message *msg, compress_func_t compress)
{
	for (unsigned i = 0; i < msg->num_chunks; i++) {
		DEBUG2("compress chunk %u of %u", i, msg->num_chunks);
		int ret = compress(msg->uncompressed_chunks[i],
				   msg->uncompressed_chunk_sizes[i],
				   msg->compressed_chunks[i],
				   &msg->compressed_chunk_sizes[i]);
		if (ret == 0) {
			msg->out_compressed_chunks[i] = msg->compressed_chunks[i];
		} else {
			msg->out_compressed_chunks[i] = msg->uncompressed_chunks[i];
			msg->compressed_chunk_sizes[i] = msg->uncompressed_chunk_sizes[i];
		}
	}
}

/* Compressor thread routine.  This is a lot simpler than the main thread
 * routine: just repeatedly get a group of chunks from the
 * res_to_compress_queue, compress them, and put them in the
 * compressed_res_queue.  A NULL pointer indicates that the thread should stop.
 * */
static void *
compressor_thread_proc(void *arg)
{
	struct compressor_thread_params *params = arg;
	struct shared_queue *res_to_compress_queue = params->res_to_compress_queue;
	struct shared_queue *compressed_res_queue = params->compressed_res_queue;
	compress_func_t compress = params->compress;
	struct message *msg;

	DEBUG("Compressor thread ready");
	while ((msg = shared_queue_get(res_to_compress_queue)) != NULL) {
		compress_chunks(msg, compress);
		shared_queue_put(compressed_res_queue, msg);
	}
	DEBUG("Compressor thread terminating");
	return NULL;
}
#endif /* ENABLE_MULTITHREADED_COMPRESSION */

static int
do_write_stream_list(struct list_head *my_resources,
		     FILE *out_fp,
		     int out_ctype,
		     wimlib_progress_func_t progress_func,
		     union wimlib_progress_info *progress,
		     int write_resource_flags)
{
	int ret;
	struct wim_lookup_table_entry *lte, *tmp;

	list_for_each_entry_safe(lte, tmp, my_resources, staging_list) {
		ret = write_wim_resource(lte,
					 out_fp,
					 out_ctype,
					 &lte->output_resource_entry,
					 write_resource_flags);
		if (ret != 0)
			return ret;
		list_del(&lte->staging_list);
		progress->write_streams.completed_bytes +=
			wim_resource_size(lte);
		progress->write_streams.completed_streams++;
		if (progress_func) {
			progress_func(WIMLIB_PROGRESS_MSG_WRITE_STREAMS,
				      progress);
		}
	}
	return 0;
}

static int
write_stream_list_serial(struct list_head *stream_list,
			 FILE *out_fp,
			 int out_ctype,
			 int write_flags,
			 wimlib_progress_func_t progress_func,
			 union wimlib_progress_info *progress)
{
	int write_resource_flags;

	if (write_flags & WIMLIB_WRITE_FLAG_RECOMPRESS)
		write_resource_flags = WIMLIB_RESOURCE_FLAG_RECOMPRESS;
	else
		write_resource_flags = 0;
	progress->write_streams.num_threads = 1;
	if (progress_func)
		progress_func(WIMLIB_PROGRESS_MSG_WRITE_STREAMS, progress);
	return do_write_stream_list(stream_list, out_fp,
				    out_ctype, progress_func,
				    progress, write_resource_flags);
}

#ifdef ENABLE_MULTITHREADED_COMPRESSION
static int
write_wim_chunks(struct message *msg, FILE *out_fp,
		 struct chunk_table *chunk_tab)
{
	for (unsigned i = 0; i < msg->num_chunks; i++) {
		unsigned chunk_csize = msg->compressed_chunk_sizes[i];

		DEBUG2("Write wim chunk %u of %u (csize = %u)",
		      i, msg->num_chunks, chunk_csize);

		if (fwrite(msg->out_compressed_chunks[i], 1, chunk_csize, out_fp)
		    != chunk_csize)
		{
			ERROR_WITH_ERRNO("Failed to write WIM chunk");
			return WIMLIB_ERR_WRITE;
		}

		*chunk_tab->cur_offset_p++ = chunk_tab->cur_offset;
		chunk_tab->cur_offset += chunk_csize;
	}
	return 0;
}

/*
 * This function is executed by the main thread when the resources are being
 * compressed in parallel.  The main thread is in change of all reading of the
 * uncompressed data and writing of the compressed data.  The compressor threads
 * *only* do compression from/to in-memory buffers.
 *
 * Each unit of work given to a compressor thread is up to MAX_CHUNKS_PER_MSG
 * chunks of compressed data to compress, represented in a `struct message'.
 * Each message is passed from the main thread to a worker thread through the
 * res_to_compress_queue, and it is passed back through the
 * compressed_res_queue.
 */
static int
main_writer_thread_proc(struct list_head *stream_list,
			FILE *out_fp,
			int out_ctype,
			struct shared_queue *res_to_compress_queue,
			struct shared_queue *compressed_res_queue,
			size_t num_messages,
			int write_flags,
			wimlib_progress_func_t progress_func,
			union wimlib_progress_info *progress)
{
	int ret;
	struct chunk_table *cur_chunk_tab = NULL;
	struct message *msgs = CALLOC(num_messages, sizeof(struct message));
	struct wim_lookup_table_entry *next_lte = NULL;

	// Initially, all the messages are available to use.
	LIST_HEAD(available_msgs);

	if (!msgs) {
		ret = WIMLIB_ERR_NOMEM;
		goto out;
	}

	for (size_t i = 0; i < num_messages; i++)
		list_add(&msgs[i].list, &available_msgs);

	// outstanding_resources is the list of resources that currently have
	// had chunks sent off for compression.
	//
	// The first stream in outstanding_resources is the stream that is
	// currently being written (cur_lte).
	//
	// The last stream in outstanding_resources is the stream that is
	// currently being read and chunks fed to the compressor threads
	// (next_lte).
	//
	// Depending on the number of threads and the sizes of the resource,
	// the outstanding streams list may contain streams between cur_lte and
	// next_lte that have all their chunks compressed or being compressed,
	// but haven't been written yet.
	//
	LIST_HEAD(outstanding_resources);
	struct list_head *next_resource = stream_list->next;
	u64 next_chunk = 0;
	u64 next_num_chunks = 0;

	// As in write_wim_resource(), each resource we read is checksummed.
	SHA_CTX next_sha_ctx;
	u8 next_hash[SHA1_HASH_SIZE];

	// Resources that don't need any chunks compressed are added to this
	// list and written directly by the main thread.
	LIST_HEAD(my_resources);

	struct wim_lookup_table_entry *cur_lte = NULL;
	struct message *msg;

#ifdef WITH_NTFS_3G
	ntfs_inode *ni = NULL;
#endif

	DEBUG("Initializing buffers for uncompressed "
	      "and compressed data (%zu bytes needed)",
	      num_messages * MAX_CHUNKS_PER_MSG * WIM_CHUNK_SIZE * 2);

	// Pre-allocate all the buffers that will be needed to do the chunk
	// compression.
	for (size_t i = 0; i < num_messages; i++) {
		for (size_t j = 0; j < MAX_CHUNKS_PER_MSG; j++) {
			msgs[i].compressed_chunks[j] = MALLOC(WIM_CHUNK_SIZE);

			// The extra 8 bytes is because longest_match() in
			// lz77.c may read a little bit off the end of the
			// uncompressed data.  It doesn't need to be
			// initialized--- we really just need to avoid accessing
			// an unmapped page.
			msgs[i].uncompressed_chunks[j] = MALLOC(WIM_CHUNK_SIZE + 8);
			if (msgs[i].compressed_chunks[j] == NULL ||
			    msgs[i].uncompressed_chunks[j] == NULL)
			{
				ret = WIMLIB_ERR_NOMEM;
				goto out;
			}
		}
	}

	// This loop is executed until all resources have been written, except
	// possibly a few that have been added to the @my_resources list for
	// writing later.
	while (1) {
		// Send chunks to the compressor threads until either (a) there
		// are no more messages available since they were all sent off,
		// or (b) there are no more resources that need to be
		// compressed.
		while (!list_empty(&available_msgs)) {
			if (next_chunk == next_num_chunks) {
				// If next_chunk == next_num_chunks, there are
				// no more chunks to write in the current
				// stream.  So, check the SHA1 message digest of
				// the stream that was just finished (unless
				// next_lte == NULL, which is the case the very
				// first time this loop is entered, and also
				// near the very end of the compression when
				// there are no more streams.)  Then, advance to
				// the next stream (if there is one).
				if (next_lte != NULL) {
				#ifdef WITH_NTFS_3G
					end_wim_resource_read(next_lte, ni);
					ni = NULL;
				#else
					end_wim_resource_read(next_lte);
				#endif
					DEBUG2("Finalize SHA1 md (next_num_chunks=%zu)",
					       next_num_chunks);
					sha1_final(next_hash, &next_sha_ctx);
					if (!hashes_equal(next_lte->hash, next_hash)) {
						ERROR("WIM resource has incorrect hash!");
						if (next_lte->resource_location ==
						    RESOURCE_IN_FILE_ON_DISK)
						{
							ERROR("We were reading it from `%s'; "
							      "maybe it changed while we were "
							      "reading it.",
							      next_lte->file_on_disk);
						}
						ret = WIMLIB_ERR_INVALID_RESOURCE_HASH;
						goto out;
					}
				}

				// Advance to the next resource.
				//
				// If the next resource needs no compression, just write
				// it with this thread (not now though--- we could be in
				// the middle of writing another resource.)  Keep doing
				// this until we either get to the end of the resources
				// list, or we get to a resource that needs compression.
				while (1) {
					if (next_resource == stream_list) {
						// No more resources to send for
						// compression
						next_lte = NULL;
						break;
					}
					next_lte = container_of(next_resource,
								struct wim_lookup_table_entry,
								staging_list);
					next_resource = next_resource->next;
					if ((!(write_flags & WIMLIB_WRITE_FLAG_RECOMPRESS)
					       && wim_resource_compression_type(next_lte) == out_ctype)
					    || wim_resource_size(next_lte) == 0)
					{
						list_add_tail(&next_lte->staging_list,
							      &my_resources);
					} else {
						list_add_tail(&next_lte->staging_list,
							      &outstanding_resources);
						next_chunk = 0;
						next_num_chunks = wim_resource_chunks(next_lte);
						sha1_init(&next_sha_ctx);
						INIT_LIST_HEAD(&next_lte->msg_list);
					#ifdef WITH_NTFS_3G
						ret = prepare_resource_for_read(next_lte, &ni);
					#else
						ret = prepare_resource_for_read(next_lte);
					#endif

						if (ret != 0)
							goto out;
						if (cur_lte == NULL) {
							// Set cur_lte for the
							// first time
							cur_lte = next_lte;
						}
						break;
					}
				}
			}

			if (next_lte == NULL) {
				// No more resources to send for compression
				break;
			}

			// Get a message from the available messages
			// list
			msg = container_of(available_msgs.next,
					   struct message,
					   list);

			// ... and delete it from the available messages
			// list
			list_del(&msg->list);

			// Initialize the message with the chunks to
			// compress.
			msg->num_chunks = min(next_num_chunks - next_chunk,
					      MAX_CHUNKS_PER_MSG);
			msg->lte = next_lte;
			msg->complete = false;
			msg->begin_chunk = next_chunk;

			unsigned size = WIM_CHUNK_SIZE;
			for (unsigned i = 0; i < msg->num_chunks; i++) {

				// Read chunk @next_chunk of the stream into the
				// message so that a compressor thread can
				// compress it.

				if (next_chunk == next_num_chunks - 1) {
					size = MODULO_NONZERO(wim_resource_size(next_lte),
							      WIM_CHUNK_SIZE);
				}

				DEBUG2("Read resource (size=%u, offset=%zu)",
				      size, next_chunk * WIM_CHUNK_SIZE);

				msg->uncompressed_chunk_sizes[i] = size;

				ret = read_wim_resource(next_lte,
							msg->uncompressed_chunks[i],
							size,
							next_chunk * WIM_CHUNK_SIZE,
							0);
				if (ret != 0)
					goto out;
				sha1_update(&next_sha_ctx,
					    msg->uncompressed_chunks[i], size);
				next_chunk++;
			}

			// Send the compression request
			list_add_tail(&msg->list, &next_lte->msg_list);
			shared_queue_put(res_to_compress_queue, msg);
			DEBUG2("Compression request sent");
		}

		// If there are no outstanding resources, there are no more
		// resources that need to be written.
		if (list_empty(&outstanding_resources)) {
			ret = 0;
			goto out;
		}

		// Get the next message from the queue and process it.
		// The message will contain 1 or more data chunks that have been
		// compressed.
		msg = shared_queue_get(compressed_res_queue);
		msg->complete = true;

		// Is this the next chunk in the current resource?  If it's not
		// (i.e., an earlier chunk in a same or different resource
		// hasn't been compressed yet), do nothing, and keep this
		// message around until all earlier chunks are received.
		//
		// Otherwise, write all the chunks we can.
		while (cur_lte != NULL &&
		       !list_empty(&cur_lte->msg_list) &&
		       (msg = container_of(cur_lte->msg_list.next,
					   struct message,
					   list))->complete)
		{
			DEBUG2("Complete msg (begin_chunk=%"PRIu64")", msg->begin_chunk);
			if (msg->begin_chunk == 0) {
				DEBUG2("Begin chunk tab");

				// This is the first set of chunks.  Leave space
				// for the chunk table in the output file.
				off_t cur_offset = ftello(out_fp);
				if (cur_offset == -1) {
					ret = WIMLIB_ERR_WRITE;
					goto out;
				}
				ret = begin_wim_resource_chunk_tab(cur_lte,
								   out_fp,
								   cur_offset,
								   &cur_chunk_tab);
				if (ret != 0)
					goto out;
			}

			// Write the compressed chunks from the message.
			ret = write_wim_chunks(msg, out_fp, cur_chunk_tab);
			if (ret != 0)
				goto out;

			list_del(&msg->list);

			// This message is available to use for different chunks
			// now.
			list_add(&msg->list, &available_msgs);

			// Was this the last chunk of the stream?  If so, finish
			// it.
			if (list_empty(&cur_lte->msg_list) &&
			    msg->begin_chunk + msg->num_chunks == cur_chunk_tab->num_chunks)
			{
				DEBUG2("Finish wim chunk tab");
				u64 res_csize;
				ret = finish_wim_resource_chunk_tab(cur_chunk_tab,
								    out_fp,
								    &res_csize);
				if (ret != 0)
					goto out;

				if (res_csize >= wim_resource_size(cur_lte)) {
					/* Oops!  We compressed the resource to
					 * larger than the original size.  Write
					 * the resource uncompressed instead. */
					ret = write_uncompressed_resource_and_truncate(
							 cur_lte,
							 out_fp,
							 cur_chunk_tab->file_offset,
							 &cur_lte->output_resource_entry);
					if (ret != 0)
						goto out;
				} else {
					cur_lte->output_resource_entry.size =
						res_csize;

					cur_lte->output_resource_entry.original_size =
						cur_lte->resource_entry.original_size;

					cur_lte->output_resource_entry.offset =
						cur_chunk_tab->file_offset;

					cur_lte->output_resource_entry.flags =
						cur_lte->resource_entry.flags |
							WIM_RESHDR_FLAG_COMPRESSED;
				}

				progress->write_streams.completed_bytes +=
						wim_resource_size(cur_lte);
				progress->write_streams.completed_streams++;

				if (progress_func) {
					progress_func(WIMLIB_PROGRESS_MSG_WRITE_STREAMS,
						      progress);
				}

				FREE(cur_chunk_tab);
				cur_chunk_tab = NULL;

				struct list_head *next = cur_lte->staging_list.next;
				list_del(&cur_lte->staging_list);

				if (next == &outstanding_resources)
					cur_lte = NULL;
				else
					cur_lte = container_of(cur_lte->staging_list.next,
							       struct wim_lookup_table_entry,
							       staging_list);

				// Since we just finished writing a stream,
				// write any streams that have been added to the
				// my_resources list for direct writing by the
				// main thread (e.g. resources that don't need
				// to be compressed because the desired
				// compression type is the same as the previous
				// compression type).
				ret = do_write_stream_list(&my_resources,
							   out_fp,
							   out_ctype,
							   progress_func,
							   progress,
							   0);
				if (ret != 0)
					goto out;
			}
		}
	}

out:
	if (ret == WIMLIB_ERR_NOMEM) {
		ERROR("Could not allocate enough memory for "
		      "multi-threaded compression");
	}

	if (next_lte) {
#ifdef WITH_NTFS_3G
		end_wim_resource_read(next_lte, ni);
#else
		end_wim_resource_read(next_lte);
#endif
	}

	if (ret == 0) {
		ret = do_write_stream_list(&my_resources, out_fp,
					   out_ctype, progress_func,
					   progress, 0);
	} else {
		if (msgs) {
			size_t num_available_msgs = 0;
			struct list_head *cur;

			list_for_each(cur, &available_msgs) {
				num_available_msgs++;
			}

			while (num_available_msgs < num_messages) {
				shared_queue_get(compressed_res_queue);
				num_available_msgs++;
			}
		}
	}

	if (msgs) {
		for (size_t i = 0; i < num_messages; i++) {
			for (size_t j = 0; j < MAX_CHUNKS_PER_MSG; j++) {
				FREE(msgs[i].compressed_chunks[j]);
				FREE(msgs[i].uncompressed_chunks[j]);
			}
		}
		FREE(msgs);
	}

	FREE(cur_chunk_tab);
	return ret;
}

static long
get_default_num_threads()
{
#ifdef __WIN32__
	return win32_get_number_of_processors();
#else
	return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

static int
write_stream_list_parallel(struct list_head *stream_list,
			   FILE *out_fp,
			   int out_ctype,
			   int write_flags,
			   unsigned num_threads,
			   wimlib_progress_func_t progress_func,
			   union wimlib_progress_info *progress)
{
	int ret;
	struct shared_queue res_to_compress_queue;
	struct shared_queue compressed_res_queue;
	pthread_t *compressor_threads = NULL;

	if (num_threads == 0) {
		long nthreads = get_default_num_threads();
		if (nthreads < 1 || nthreads > UINT_MAX) {
			WARNING("Could not determine number of processors! Assuming 1");
			goto out_serial;
		} else {
			num_threads = nthreads;
		}
	}

	progress->write_streams.num_threads = num_threads;
	wimlib_assert(stream_list->next != stream_list);

	static const double MESSAGES_PER_THREAD = 2.0;
	size_t queue_size = (size_t)(num_threads * MESSAGES_PER_THREAD);

	DEBUG("Initializing shared queues (queue_size=%zu)", queue_size);

	ret = shared_queue_init(&res_to_compress_queue, queue_size);
	if (ret != 0)
		goto out_serial;

	ret = shared_queue_init(&compressed_res_queue, queue_size);
	if (ret != 0)
		goto out_destroy_res_to_compress_queue;

	struct compressor_thread_params params;
	params.res_to_compress_queue = &res_to_compress_queue;
	params.compressed_res_queue = &compressed_res_queue;
	params.compress = get_compress_func(out_ctype);

	compressor_threads = MALLOC(num_threads * sizeof(pthread_t));
	if (!compressor_threads) {
		ret = WIMLIB_ERR_NOMEM;
		goto out_destroy_compressed_res_queue;
	}

	for (unsigned i = 0; i < num_threads; i++) {
		DEBUG("pthread_create thread %u", i);
		ret = pthread_create(&compressor_threads[i], NULL,
				     compressor_thread_proc, &params);
		if (ret != 0) {
			ret = -1;
			ERROR_WITH_ERRNO("Failed to create compressor "
					 "thread %u", i);
			num_threads = i;
			goto out_join;
		}
	}

	if (progress_func)
		progress_func(WIMLIB_PROGRESS_MSG_WRITE_STREAMS, progress);

	ret = main_writer_thread_proc(stream_list,
				      out_fp,
				      out_ctype,
				      &res_to_compress_queue,
				      &compressed_res_queue,
				      queue_size,
				      write_flags,
				      progress_func,
				      progress);
out_join:
	for (unsigned i = 0; i < num_threads; i++)
		shared_queue_put(&res_to_compress_queue, NULL);

	for (unsigned i = 0; i < num_threads; i++) {
		if (pthread_join(compressor_threads[i], NULL)) {
			WARNING_WITH_ERRNO("Failed to join compressor "
					   "thread %u", i);
		}
	}
	FREE(compressor_threads);
out_destroy_compressed_res_queue:
	shared_queue_destroy(&compressed_res_queue);
out_destroy_res_to_compress_queue:
	shared_queue_destroy(&res_to_compress_queue);
	if (ret >= 0 && ret != WIMLIB_ERR_NOMEM)
		return ret;
out_serial:
	WARNING("Falling back to single-threaded compression");
	return write_stream_list_serial(stream_list,
					out_fp,
					out_ctype,
					write_flags,
					progress_func,
					progress);

}
#endif

/*
 * Write a list of streams to a WIM (@out_fp) using the compression type
 * @out_ctype and up to @num_threads compressor threads.
 */
static int
write_stream_list(struct list_head *stream_list, FILE *out_fp,
		  int out_ctype, int write_flags,
		  unsigned num_threads,
		  wimlib_progress_func_t progress_func)
{
	struct wim_lookup_table_entry *lte;
	size_t num_streams = 0;
	u64 total_bytes = 0;
	u64 total_compression_bytes = 0;
	union wimlib_progress_info progress;

	list_for_each_entry(lte, stream_list, staging_list) {
		num_streams++;
		total_bytes += wim_resource_size(lte);
		if (out_ctype != WIMLIB_COMPRESSION_TYPE_NONE
		       && (wim_resource_compression_type(lte) != out_ctype ||
			   (write_flags & WIMLIB_WRITE_FLAG_RECOMPRESS)))
		{
			total_compression_bytes += wim_resource_size(lte);
		}
	}
	progress.write_streams.total_bytes       = total_bytes;
	progress.write_streams.total_streams     = num_streams;
	progress.write_streams.completed_bytes   = 0;
	progress.write_streams.completed_streams = 0;
	progress.write_streams.num_threads       = num_threads;
	progress.write_streams.compression_type  = out_ctype;

#ifdef ENABLE_MULTITHREADED_COMPRESSION
	if (total_compression_bytes >= 1000000 && num_threads != 1)
		return write_stream_list_parallel(stream_list,
						  out_fp,
						  out_ctype,
						  write_flags,
						  num_threads,
						  progress_func,
						  &progress);
	else
#endif
		return write_stream_list_serial(stream_list,
						out_fp,
						out_ctype,
						write_flags,
						progress_func,
						&progress);
}

struct lte_overwrite_prepare_args {
	WIMStruct *wim;
	off_t end_offset;
	struct list_head *stream_list;
};

static int
lte_overwrite_prepare(struct wim_lookup_table_entry *lte, void *arg)
{
	struct lte_overwrite_prepare_args *args = arg;

	if (lte->resource_location == RESOURCE_IN_WIM &&
	    lte->wim == args->wim &&
	    lte->resource_entry.offset + lte->resource_entry.size > args->end_offset)
	{
	#ifdef ENABLE_ERROR_MESSAGES
		ERROR("The following resource is after the XML data:");
		print_lookup_table_entry(lte, stderr);
	#endif
		return WIMLIB_ERR_RESOURCE_ORDER;
	}

	lte->out_refcnt = lte->refcnt;
	memcpy(&lte->output_resource_entry, &lte->resource_entry,
	       sizeof(struct resource_entry));
	if (!(lte->resource_entry.flags & WIM_RESHDR_FLAG_METADATA)) {
		wimlib_assert(lte->resource_location != RESOURCE_NONEXISTENT);
		if (lte->resource_location != RESOURCE_IN_WIM || lte->wim != args->wim)
			list_add(&lte->staging_list, args->stream_list);
	}
	return 0;
}

static int
wim_find_new_streams(WIMStruct *wim, off_t end_offset,
		     struct list_head *stream_list)
{
	struct lte_overwrite_prepare_args args = {
		.wim         = wim,
		.end_offset  = end_offset,
		.stream_list = stream_list,
	};

	return for_lookup_table_entry(wim->lookup_table,
				      lte_overwrite_prepare, &args);
}

static int
inode_find_streams_to_write(struct wim_inode *inode,
			    struct wim_lookup_table *table,
			    struct list_head *stream_list)
{
	struct wim_lookup_table_entry *lte;
	for (unsigned i = 0; i <= inode->i_num_ads; i++) {
		lte = inode_stream_lte(inode, i, table);
		if (lte) {
			if (lte->out_refcnt == 0)
				list_add_tail(&lte->staging_list, stream_list);
			lte->out_refcnt += inode->i_nlink;
		}
	}
	return 0;
}

static int
image_find_streams_to_write(WIMStruct *w)
{
	struct wim_inode *inode;
	struct hlist_node *cur;
	struct hlist_head *inode_list;

	inode_list = &wim_get_current_image_metadata(w)->inode_list;
	hlist_for_each_entry(inode, cur, inode_list, i_hlist) {
		inode_find_streams_to_write(inode, w->lookup_table,
					    (struct list_head*)w->private);
	}
	return 0;
}

static int
write_wim_streams(WIMStruct *w, int image, int write_flags,
			     unsigned num_threads,
			     wimlib_progress_func_t progress_func)
{

	for_lookup_table_entry(w->lookup_table, lte_zero_out_refcnt, NULL);
	LIST_HEAD(stream_list);
	w->private = &stream_list;
	for_image(w, image, image_find_streams_to_write);
	return write_stream_list(&stream_list, w->out_fp,
				 wimlib_get_compression_type(w), write_flags,
				 num_threads, progress_func);
}

/*
 * Finish writing a WIM file: write the lookup table, xml data, and integrity
 * table (optional), then overwrite the WIM header.
 *
 * write_flags is a bitwise OR of the following:
 *
 * 	(public)  WIMLIB_WRITE_FLAG_CHECK_INTEGRITY:
 * 		Include an integrity table.
 *
 * 	(public)  WIMLIB_WRITE_FLAG_SHOW_PROGRESS:
 * 		Show progress information when (if) writing the integrity table.
 *
 * 	(private) WIMLIB_WRITE_FLAG_NO_LOOKUP_TABLE:
 * 		Don't write the lookup table.
 *
 * 	(private) WIMLIB_WRITE_FLAG_REUSE_INTEGRITY_TABLE:
 * 		When (if) writing the integrity table, re-use entries from the
 * 		existing integrity table, if possible.
 *
 * 	(private) WIMLIB_WRITE_FLAG_CHECKPOINT_AFTER_XML:
 * 		After writing the XML data but before writing the integrity
 * 		table, write a temporary WIM header and flush the stream so that
 * 		the WIM is less likely to become corrupted upon abrupt program
 * 		termination.
 *
 * 	(private) WIMLIB_WRITE_FLAG_FSYNC:
 * 		fsync() the output file before closing it.
 *
 */
int
finish_write(WIMStruct *w, int image, int write_flags,
	     wimlib_progress_func_t progress_func)
{
	int ret;
	struct wim_header hdr;
	FILE *out = w->out_fp;

	/* @hdr will be the header for the new WIM.  First copy all the data
	 * from the header in the WIMStruct; then set all the fields that may
	 * have changed, including the resource entries, boot index, and image
	 * count.  */
	memcpy(&hdr, &w->hdr, sizeof(struct wim_header));

	if (!(write_flags & WIMLIB_WRITE_FLAG_NO_LOOKUP_TABLE)) {
		ret = write_lookup_table(w->lookup_table, out, &hdr.lookup_table_res_entry);
		if (ret != 0)
			goto out;
	}

	ret = write_xml_data(w->wim_info, image, out,
			     (write_flags & WIMLIB_WRITE_FLAG_NO_LOOKUP_TABLE) ?
			      wim_info_get_total_bytes(w->wim_info) : 0,
			     &hdr.xml_res_entry);
	if (ret != 0)
		goto out;

	if (write_flags & WIMLIB_WRITE_FLAG_CHECK_INTEGRITY) {
		if (write_flags & WIMLIB_WRITE_FLAG_CHECKPOINT_AFTER_XML) {
			struct wim_header checkpoint_hdr;
			memcpy(&checkpoint_hdr, &hdr, sizeof(struct wim_header));
			memset(&checkpoint_hdr.integrity, 0, sizeof(struct resource_entry));
			if (fseeko(out, 0, SEEK_SET) != 0) {
				ERROR_WITH_ERRNO("Failed to seek to beginning "
						 "of WIM being written");
				ret = WIMLIB_ERR_WRITE;
				goto out;
			}
			ret = write_header(&checkpoint_hdr, out);
			if (ret != 0)
				goto out;

			if (fflush(out) != 0) {
				ERROR_WITH_ERRNO("Can't write data to WIM");
				ret = WIMLIB_ERR_WRITE;
				goto out;
			}

			if (fseeko(out, 0, SEEK_END) != 0) {
				ERROR_WITH_ERRNO("Failed to seek to end "
						 "of WIM being written");
				ret = WIMLIB_ERR_WRITE;
				goto out;
			}
		}

		off_t old_lookup_table_end;
		off_t new_lookup_table_end;
		if (write_flags & WIMLIB_WRITE_FLAG_REUSE_INTEGRITY_TABLE) {
			old_lookup_table_end = w->hdr.lookup_table_res_entry.offset +
					       w->hdr.lookup_table_res_entry.size;
		} else {
			old_lookup_table_end = 0;
		}
		new_lookup_table_end = hdr.lookup_table_res_entry.offset +
				       hdr.lookup_table_res_entry.size;

		ret = write_integrity_table(out,
					    &hdr.integrity,
					    new_lookup_table_end,
					    old_lookup_table_end,
					    progress_func);
		if (ret != 0)
			goto out;
	} else {
		memset(&hdr.integrity, 0, sizeof(struct resource_entry));
	}

	/*
	 * In the WIM header, there is room for the resource entry for a
	 * metadata resource labeled as the "boot metadata".  This entry should
	 * be zeroed out if there is no bootable image (boot_idx 0).  Otherwise,
	 * it should be a copy of the resource entry for the image that is
	 * marked as bootable.  This is not well documented...
	 */

	/* Set image count and boot index correctly for single image writes */
	if (image != WIMLIB_ALL_IMAGES) {
		hdr.image_count = 1;
		if (hdr.boot_idx == image)
			hdr.boot_idx = 1;
		else
			hdr.boot_idx = 0;
	}

	if (hdr.boot_idx == 0) {
		memset(&hdr.boot_metadata_res_entry, 0,
		       sizeof(struct resource_entry));
	} else {
		memcpy(&hdr.boot_metadata_res_entry,
		       &w->image_metadata[
			  hdr.boot_idx - 1].metadata_lte->output_resource_entry,
		       sizeof(struct resource_entry));
	}

	if (fseeko(out, 0, SEEK_SET) != 0) {
		ERROR_WITH_ERRNO("Failed to seek to beginning of WIM "
				 "being written");
		ret = WIMLIB_ERR_WRITE;
		goto out;
	}

	ret = write_header(&hdr, out);
	if (ret != 0)
		goto out;

	if (write_flags & WIMLIB_WRITE_FLAG_FSYNC) {
		if (fflush(out) != 0
		    || fsync(fileno(out)) != 0)
		{
			ERROR_WITH_ERRNO("Error flushing data to WIM file");
			ret = WIMLIB_ERR_WRITE;
		}
	}
out:
	if (fclose(out) != 0) {
		ERROR_WITH_ERRNO("Failed to close the WIM file");
		if (ret == 0)
			ret = WIMLIB_ERR_WRITE;
	}
	w->out_fp = NULL;
	return ret;
}

#if defined(HAVE_SYS_FILE_H) && defined(HAVE_FLOCK)
int
lock_wim(WIMStruct *w, FILE *fp)
{
	int ret = 0;
	if (fp && !w->wim_locked) {
		ret = flock(fileno(fp), LOCK_EX | LOCK_NB);
		if (ret != 0) {
			if (errno == EWOULDBLOCK) {
				ERROR("`%s' is already being modified or has been "
				      "mounted read-write\n"
				      "        by another process!", w->filename);
				ret = WIMLIB_ERR_ALREADY_LOCKED;
			} else {
				WARNING_WITH_ERRNO("Failed to lock `%s'",
						   w->filename);
				ret = 0;
			}
		} else {
			w->wim_locked = 1;
		}
	}
	return ret;
}
#endif

static int
open_wim_writable(WIMStruct *w, const mbchar *path,
		  bool trunc, bool readable)
{
	const char *mode;
	if (trunc)
		if (readable)
			mode = "w+b";
		else
			mode = "wb";
	else
		mode = "r+b";

	wimlib_assert(w->out_fp == NULL);
	w->out_fp = fopen(path, mode);
	if (w->out_fp) {
		return 0;
	} else {
		ERROR_WITH_ERRNO("Failed to open `%s' for writing", path);
		return WIMLIB_ERR_OPEN;
	}
}


void
close_wim_writable(WIMStruct *w)
{
	if (w->out_fp) {
		if (fclose(w->out_fp) != 0) {
			WARNING_WITH_ERRNO("Failed to close output WIM");
		}
		w->out_fp = NULL;
	}
}

/* Open file stream and write dummy header for WIM. */
int
begin_write(WIMStruct *w, const mbchar *path, int write_flags)
{
	int ret;
	ret = open_wim_writable(w, path, true,
				(write_flags & WIMLIB_WRITE_FLAG_CHECK_INTEGRITY) != 0);
	if (ret != 0)
		return ret;
	/* Write dummy header. It will be overwritten later. */
	return write_header(&w->hdr, w->out_fp);
}

/* Writes a stand-alone WIM to a file.  */
WIMLIBAPI int
wimlib_write(WIMStruct *w, const mbchar *path,
	     int image, int write_flags, unsigned num_threads,
	     wimlib_progress_func_t progress_func)
{
	int ret;

	if (!path)
		return WIMLIB_ERR_INVALID_PARAM;

	write_flags &= WIMLIB_WRITE_MASK_PUBLIC;

	if (image != WIMLIB_ALL_IMAGES &&
	     (image < 1 || image > w->hdr.image_count))
		return WIMLIB_ERR_INVALID_IMAGE;

	if (w->hdr.total_parts != 1) {
		ERROR("Cannot call wimlib_write() on part of a split WIM");
		return WIMLIB_ERR_SPLIT_UNSUPPORTED;
	}

	ret = begin_write(w, path, write_flags);
	if (ret != 0)
		goto out;

	ret = write_wim_streams(w, image, write_flags, num_threads,
				progress_func);
	if (ret != 0)
		goto out;

	if (progress_func)
		progress_func(WIMLIB_PROGRESS_MSG_WRITE_METADATA_BEGIN, NULL);

	ret = for_image(w, image, write_metadata_resource);
	if (ret != 0)
		goto out;

	if (progress_func)
		progress_func(WIMLIB_PROGRESS_MSG_WRITE_METADATA_END, NULL);

	ret = finish_write(w, image, write_flags, progress_func);
out:
	close_wim_writable(w);
	DEBUG("wimlib_write(path=%s) = %d", path, ret);
	return ret;
}

static bool
any_images_modified(WIMStruct *w)
{
	for (int i = 0; i < w->hdr.image_count; i++)
		if (w->image_metadata[i].modified)
			return true;
	return false;
}

/*
 * Overwrite a WIM, possibly appending streams to it.
 *
 * A WIM looks like (or is supposed to look like) the following:
 *
 *                   Header (212 bytes)
 *                   Streams and metadata resources (variable size)
 *                   Lookup table (variable size)
 *                   XML data (variable size)
 *                   Integrity table (optional) (variable size)
 *
 * If we are not adding any streams or metadata resources, the lookup table is
 * unchanged--- so we only need to overwrite the XML data, integrity table, and
 * header.  This operation is potentially unsafe if the program is abruptly
 * terminated while the XML data or integrity table are being overwritten, but
 * before the new header has been written.  To partially alleviate this problem,
 * a special flag (WIMLIB_WRITE_FLAG_CHECKPOINT_AFTER_XML) is passed to
 * finish_write() to cause a temporary WIM header to be written after the XML
 * data has been written.  This may prevent the WIM from becoming corrupted if
 * the program is terminated while the integrity table is being calculated (but
 * no guarantees, due to write re-ordering...).
 *
 * If we are adding new streams or images (metadata resources), the lookup table
 * needs to be changed, and those streams need to be written.  In this case, we
 * try to perform a safe update of the WIM file by writing the streams *after*
 * the end of the previous WIM, then writing the new lookup table, XML data, and
 * (optionally) integrity table following the new streams.  This will produce a
 * layout like the following:
 *
 *                   Header (212 bytes)
 *                   (OLD) Streams and metadata resources (variable size)
 *                   (OLD) Lookup table (variable size)
 *                   (OLD) XML data (variable size)
 *                   (OLD) Integrity table (optional) (variable size)
 *                   (NEW) Streams and metadata resources (variable size)
 *                   (NEW) Lookup table (variable size)
 *                   (NEW) XML data (variable size)
 *                   (NEW) Integrity table (optional) (variable size)
 *
 * At all points, the WIM is valid as nothing points to the new data yet.  Then,
 * the header is overwritten to point to the new lookup table, XML data, and
 * integrity table, to produce the following layout:
 *
 *                   Header (212 bytes)
 *                   Streams and metadata resources (variable size)
 *                   Nothing (variable size)
 *                   More Streams and metadata resources (variable size)
 *                   Lookup table (variable size)
 *                   XML data (variable size)
 *                   Integrity table (optional) (variable size)
 *
 * This method allows an image to be appended to a large WIM very quickly, and
 * is is crash-safe except in the case of write re-ordering, but the
 * disadvantage is that a small hole is left in the WIM where the old lookup
 * table, xml data, and integrity table were.  (These usually only take up a
 * small amount of space compared to the streams, however.)
 */
static int
overwrite_wim_inplace(WIMStruct *w, int write_flags,
		      unsigned num_threads,
		      wimlib_progress_func_t progress_func)
{
	int ret;
	struct list_head stream_list;
	off_t old_wim_end;
	bool found_modified_image;

	DEBUG("Overwriting `%s' in-place", w->filename);

	/* Make sure that the integrity table (if present) is after the XML
	 * data, and that there are no stream resources, metadata resources, or
	 * lookup tables after the XML data.  Otherwise, these data would be
	 * overwritten. */
	if (w->hdr.integrity.offset != 0 &&
	    w->hdr.integrity.offset < w->hdr.xml_res_entry.offset) {
		ERROR("Didn't expect the integrity table to be before the XML data");
		return WIMLIB_ERR_RESOURCE_ORDER;
	}

	if (w->hdr.lookup_table_res_entry.offset > w->hdr.xml_res_entry.offset) {
		ERROR("Didn't expect the lookup table to be after the XML data");
		return WIMLIB_ERR_RESOURCE_ORDER;
	}


	if (w->hdr.integrity.offset)
		old_wim_end = w->hdr.integrity.offset + w->hdr.integrity.size;
	else
		old_wim_end = w->hdr.xml_res_entry.offset + w->hdr.xml_res_entry.size;

	if (!w->deletion_occurred && !any_images_modified(w)) {
		/* If no images have been modified and no images have been
		 * deleted, a new lookup table does not need to be written. */
		old_wim_end = w->hdr.lookup_table_res_entry.offset +
			      w->hdr.lookup_table_res_entry.size;
		write_flags |= WIMLIB_WRITE_FLAG_NO_LOOKUP_TABLE |
			       WIMLIB_WRITE_FLAG_CHECKPOINT_AFTER_XML;
	}
	INIT_LIST_HEAD(&stream_list);
	ret = wim_find_new_streams(w, old_wim_end, &stream_list);
	if (ret != 0)
		return ret;

	ret = open_wim_writable(w, w->filename, false,
				(write_flags & WIMLIB_WRITE_FLAG_CHECK_INTEGRITY) != 0);
	if (ret != 0)
		return ret;

	ret = lock_wim(w, w->out_fp);
	if (ret != 0) {
		fclose(w->out_fp);
		w->out_fp = NULL;
		return ret;
	}

	if (fseeko(w->out_fp, old_wim_end, SEEK_SET) != 0) {
		ERROR_WITH_ERRNO("Can't seek to end of WIM");
		fclose(w->out_fp);
		w->out_fp = NULL;
		w->wim_locked = 0;
		return WIMLIB_ERR_WRITE;
	}

	if (!list_empty(&stream_list)) {
		DEBUG("Writing newly added streams (offset = %"PRIu64")",
		      old_wim_end);
		ret = write_stream_list(&stream_list, w->out_fp,
					wimlib_get_compression_type(w),
					write_flags, num_threads,
					progress_func);
		if (ret != 0)
			goto out_ftruncate;
	} else {
		DEBUG("No new streams were added");
	}

	found_modified_image = false;
	for (int i = 0; i < w->hdr.image_count; i++) {
		if (!found_modified_image)
			found_modified_image = w->image_metadata[i].modified;
		if (found_modified_image) {
			select_wim_image(w, i + 1);
			ret = write_metadata_resource(w);
			if (ret != 0)
				goto out_ftruncate;
		}
	}
	write_flags |= WIMLIB_WRITE_FLAG_REUSE_INTEGRITY_TABLE;
	ret = finish_write(w, WIMLIB_ALL_IMAGES, write_flags,
			   progress_func);
out_ftruncate:
	close_wim_writable(w);
	if (ret != 0 && !(write_flags & WIMLIB_WRITE_FLAG_NO_LOOKUP_TABLE)) {
		WARNING("Truncating `%s' to its original size (%"PRIu64" bytes)",
			w->filename, old_wim_end);
		/* Return value of truncate() is ignored because this is already
		 * an error path. */
		(void)truncate(w->filename, old_wim_end);
	}
	w->wim_locked = 0;
	return ret;
}

static int
overwrite_wim_via_tmpfile(WIMStruct *w, int write_flags,
			  unsigned num_threads,
			  wimlib_progress_func_t progress_func)
{
	size_t wim_name_len;
	int ret;

	DEBUG("Overwriting `%s' via a temporary file", w->filename);

	/* Write the WIM to a temporary file in the same directory as the
	 * original WIM. */
	wim_name_len = strlen(w->filename);
	mbchar tmpfile[wim_name_len + 10];
	memcpy(tmpfile, w->filename, wim_name_len);
	randomize_char_array_with_alnum(tmpfile + wim_name_len, 9);
	tmpfile[wim_name_len + 9] = '\0';

	ret = wimlib_write(w, tmpfile, WIMLIB_ALL_IMAGES,
			   write_flags | WIMLIB_WRITE_FLAG_FSYNC,
			   num_threads, progress_func);
	if (ret != 0) {
		ERROR("Failed to write the WIM file `%s'", tmpfile);
		goto err;
	}

	DEBUG("Renaming `%s' to `%s'", tmpfile, w->filename);

#ifdef __WIN32__
	/* Windows won't let you delete open files unless FILE_SHARE_DELETE was
	 * specified to CreateFile().  The WIM was opened with fopen(), which
	 * didn't provided this flag to CreateFile, so the handle must be closed
	 * before executing the rename(). */
	if (w->fp != NULL) {
		fclose(w->fp);
		w->fp = NULL;
	}
#endif

	/* Rename the new file to the old file .*/
	if (rename(tmpfile, w->filename) != 0) {
		ERROR_WITH_ERRNO("Failed to rename `%s' to `%s'",
				 tmpfile, w->filename);
		ret = WIMLIB_ERR_RENAME;
		goto err;
	}

	if (progress_func) {
		union wimlib_progress_info progress;
		progress.rename.from = tmpfile;
		progress.rename.to = w->filename;
		progress_func(WIMLIB_PROGRESS_MSG_RENAME, &progress);
	}

	/* Close the original WIM file that was opened for reading. */
	if (w->fp != NULL) {
		fclose(w->fp);
		w->fp = NULL;
	}

	/* Re-open the WIM read-only. */
	w->fp = fopen(w->filename, "rb");
	if (w->fp == NULL) {
		ret = WIMLIB_ERR_REOPEN;
		WARNING_WITH_ERRNO("Failed to re-open `%s' read-only",
				   w->filename);
		FREE(w->filename);
		w->filename = NULL;
	}
	return ret;
err:
	/* Remove temporary file. */
	if (unlink(tmpfile) != 0)
		WARNING_WITH_ERRNO("Failed to remove `%s'", tmpfile);
	return ret;
}

/*
 * Writes a WIM file to the original file that it was read from, overwriting it.
 */
WIMLIBAPI int
wimlib_overwrite(WIMStruct *w, int write_flags,
		 unsigned num_threads,
		 wimlib_progress_func_t progress_func)
{
	write_flags &= WIMLIB_WRITE_MASK_PUBLIC;

	if (!w->filename)
		return WIMLIB_ERR_NO_FILENAME;

	if (w->hdr.total_parts != 1) {
		ERROR("Cannot modify a split WIM");
		return WIMLIB_ERR_SPLIT_UNSUPPORTED;
	}

	if ((!w->deletion_occurred || (write_flags & WIMLIB_WRITE_FLAG_SOFT_DELETE))
	    && !(write_flags & WIMLIB_WRITE_FLAG_REBUILD))
	{
		int ret;
		ret = overwrite_wim_inplace(w, write_flags, num_threads,
					    progress_func);
		if (ret == WIMLIB_ERR_RESOURCE_ORDER)
			WARNING("Falling back to re-building entire WIM");
		else
			return ret;
	}
	return overwrite_wim_via_tmpfile(w, write_flags, num_threads,
					 progress_func);
}
