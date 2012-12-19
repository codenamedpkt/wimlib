/*
 * ntfs-apply.c
 *
 * Apply a WIM image to a NTFS volume.  Restore as much information as possible,
 * including security data, file attributes, DOS names, and alternate data
 * streams.
 */

/*
 * Copyright (C) 2012 Eric Biggers
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


#include <ntfs-3g/endians.h>
#include <ntfs-3g/types.h>

#include "wimlib_internal.h"
#include "dentry.h"
#include "lookup_table.h"
#include "buffer_io.h"
#include <ntfs-3g/layout.h>
#include <ntfs-3g/acls.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/security.h> /* security.h before xattrs.h */
#include <ntfs-3g/xattrs.h>
#include <ntfs-3g/reparse.h>
#include <stdlib.h>
#include <unistd.h>

static int extract_wim_chunk_to_ntfs_attr(const u8 *buf, size_t len,
					  u64 offset, void *arg)
{
	ntfs_attr *na = arg;
	if (ntfs_attr_pwrite(na, offset, len, buf) == len) {
		return 0;
	} else {
		ERROR_WITH_ERRNO("Error extracting WIM resource to NTFS attribute");
		return WIMLIB_ERR_WRITE;
	}
}

/*
 * Extracts a WIM resource to a NTFS attribute.
 */
static int
extract_wim_resource_to_ntfs_attr(const struct lookup_table_entry *lte,
			          ntfs_attr *na)
{
	return extract_wim_resource(lte, wim_resource_size(lte),
				    extract_wim_chunk_to_ntfs_attr, na);
}

/* Writes the data streams to a NTFS file
 *
 * @ni:	     The NTFS inode for the file.
 * @inode:   The WIM dentry that has an inode containing the streams.
 *
 * Returns 0 on success, nonzero on failure.
 */
static int write_ntfs_data_streams(ntfs_inode *ni, const struct dentry *dentry,
				   union wimlib_progress_info *progress_info)
{
	int ret = 0;
	unsigned stream_idx = 0;
	ntfschar *stream_name = AT_UNNAMED;
	u32 stream_name_len = 0;
	const struct inode *inode = dentry->d_inode;

	DEBUG("Writing %u NTFS data stream%s for `%s'",
	      inode->num_ads + 1,
	      (inode->num_ads == 0 ? "" : "s"),
	      dentry->full_path_utf8);

	while (1) {
		struct lookup_table_entry *lte;

		lte = inode_stream_lte_resolved(inode, stream_idx);

		if (stream_name_len) {
			/* Create an empty named stream. */
			ret = ntfs_attr_add(ni, AT_DATA, stream_name,
					    stream_name_len, NULL, 0);
			if (ret != 0) {
				ERROR_WITH_ERRNO("Failed to create name data "
						 "stream for extracted file "
						 "`%s'",
						 dentry->full_path_utf8);
				ret = WIMLIB_ERR_NTFS_3G;
				break;

			}
		}
		/* If there's no lookup table entry, it's an empty stream.
		 * Otherwise, we must open the attribute and extract the data.
		 * */
		if (lte) {
			ntfs_attr *na;

			na = ntfs_attr_open(ni, AT_DATA, stream_name, stream_name_len);
			if (!na) {
				ERROR_WITH_ERRNO("Failed to open a data stream of "
						 "extracted file `%s'",
						 dentry->full_path_utf8);
				ret = WIMLIB_ERR_NTFS_3G;
				break;
			}
			ret = ntfs_attr_truncate_solid(na, wim_resource_size(lte));
			if (ret != 0) {
				ntfs_attr_close(na);
				break;
			}

			ret = extract_wim_resource_to_ntfs_attr(lte, na);
			ntfs_attr_close(na);
			if (ret != 0)
				break;
			progress_info->extract.completed_bytes += wim_resource_size(lte);
		}
		if (stream_idx == inode->num_ads)
			break;
		stream_name = (ntfschar*)inode->ads_entries[stream_idx].stream_name;
		stream_name_len = inode->ads_entries[stream_idx].stream_name_len / 2;
		stream_idx++;
	}
	return ret;
}

/* Open the NTFS inode that corresponds to the parent of a WIM dentry. */
static ntfs_inode *dentry_open_parent_ni(const struct dentry *dentry,
					 ntfs_volume *vol)
{
	char *p;
	const char *dir_name;
	ntfs_inode *dir_ni;
	char orig;

	p = dentry->full_path_utf8 + dentry->full_path_utf8_len;
	do {
		p--;
	} while (*p != '/');

	orig = *p;
	*p = '\0';
	dir_name = dentry->full_path_utf8;
	dir_ni = ntfs_pathname_to_inode(vol, NULL, dir_name);
	if (!dir_ni) {
		ERROR_WITH_ERRNO("Could not find NTFS inode for `%s'",
				 dir_name);
	}
	*p = orig;
	return dir_ni;
}

/*
 * Makes a NTFS hard link
 *
 * It is named @from_dentry->file_name and is located under the directory
 * specified by @dir_ni, and it is made to point to the previously extracted
 * file located at @inode->extracted_file.
 *
 * Return 0 on success, nonzero on failure.
 */
static int apply_ntfs_hardlink(const struct dentry *from_dentry,
			       const struct inode *inode,
			       ntfs_inode **dir_ni_p)
{
	int ret;
	ntfs_inode *to_ni;
	ntfs_inode *dir_ni;
	ntfs_volume *vol;

	dir_ni = *dir_ni_p;
	vol = dir_ni->vol;
	ret = ntfs_inode_close(dir_ni);
	*dir_ni_p = NULL;
	if (ret != 0) {
		ERROR_WITH_ERRNO("Error closing directory");
		return WIMLIB_ERR_NTFS_3G;
	}

	DEBUG("Extracting NTFS hard link `%s' => `%s'",
	      from_dentry->full_path_utf8, inode->extracted_file);

	to_ni = ntfs_pathname_to_inode(vol, NULL, inode->extracted_file);
	if (!to_ni) {
		ERROR_WITH_ERRNO("Could not find NTFS inode for `%s'",
				 inode->extracted_file);
		return WIMLIB_ERR_NTFS_3G;
	}

	dir_ni = dentry_open_parent_ni(from_dentry, vol);
	if (!dir_ni) {
		ntfs_inode_close(to_ni);
		return WIMLIB_ERR_NTFS_3G;
	}

	*dir_ni_p = dir_ni;

	ret = ntfs_link(to_ni, dir_ni,
			(ntfschar*)from_dentry->file_name,
			from_dentry->file_name_len / 2);
	if (ntfs_inode_close_in_dir(to_ni, dir_ni) || ret != 0) {
		ERROR_WITH_ERRNO("Could not create hard link `%s' => `%s'",
				 from_dentry->full_path_utf8,
				 inode->extracted_file);
		ret = WIMLIB_ERR_NTFS_3G;
	}
	return ret;
}

static int
apply_file_attributes_and_security_data(ntfs_inode *ni,
					ntfs_inode *dir_ni,
					const struct dentry *dentry,
					const WIMStruct *w)
{
	DEBUG("Setting NTFS file attributes on `%s' to %#"PRIx32,
	      dentry->full_path_utf8, dentry->d_inode->attributes);
	int ret;
	struct SECURITY_CONTEXT ctx;
	u32 attributes_le32;
 	attributes_le32 = cpu_to_le32(dentry->d_inode->attributes);
	memset(&ctx, 0, sizeof(ctx));
	ctx.vol = ni->vol;
	ret = ntfs_xattr_system_setxattr(&ctx, XATTR_NTFS_ATTRIB,
					 ni, dir_ni,
					 (const char*)&attributes_le32,
					 sizeof(u32), 0);
	if (ret != 0) {
		ERROR("Failed to set NTFS file attributes on `%s'",
		       dentry->full_path_utf8);
		return WIMLIB_ERR_NTFS_3G;
	}
	if (dentry->d_inode->security_id != -1) {
		const struct wim_security_data *sd;
		const char *descriptor;

		sd = wim_const_security_data(w);
		wimlib_assert(dentry->d_inode->security_id < sd->num_entries);
		descriptor = (const char *)sd->descriptors[dentry->d_inode->security_id];
		DEBUG("Applying security descriptor %d to `%s'",
		      dentry->d_inode->security_id, dentry->full_path_utf8);

		ret = ntfs_xattr_system_setxattr(&ctx, XATTR_NTFS_ACL,
						 ni, dir_ni, descriptor,
					   	 sd->sizes[dentry->d_inode->security_id], 0);

		if (ret != 0) {
			ERROR_WITH_ERRNO("Failed to set security data on `%s'",
					dentry->full_path_utf8);
			return WIMLIB_ERR_NTFS_3G;
		}
	}
	return 0;
}

static int apply_reparse_data(ntfs_inode *ni, const struct dentry *dentry,
			      union wimlib_progress_info *progress_info)
{
	struct lookup_table_entry *lte;
	int ret = 0;

	lte = inode_unnamed_lte_resolved(dentry->d_inode);

	DEBUG("Applying reparse data to `%s'", dentry->full_path_utf8);

	if (!lte) {
		ERROR("Could not find reparse data for `%s'",
		      dentry->full_path_utf8);
		return WIMLIB_ERR_INVALID_DENTRY;
	}

	if (wim_resource_size(lte) >= 0xffff) {
		ERROR("Reparse data of `%s' is too long (%"PRIu64" bytes)",
		      dentry->full_path_utf8, wim_resource_size(lte));
		return WIMLIB_ERR_INVALID_DENTRY;
	}

	u8 reparse_data_buf[8 + wim_resource_size(lte)];
	u8 *p = reparse_data_buf;
	p = put_u32(p, dentry->d_inode->reparse_tag); /* ReparseTag */
	p = put_u16(p, wim_resource_size(lte)); /* ReparseDataLength */
	p = put_u16(p, 0); /* Reserved */

	ret = read_full_wim_resource(lte, p, 0);
	if (ret != 0)
		return ret;

	ret = ntfs_set_ntfs_reparse_data(ni, (char*)reparse_data_buf,
					 wim_resource_size(lte) + 8, 0);
	if (ret != 0) {
		ERROR_WITH_ERRNO("Failed to set NTFS reparse data on `%s'",
				 dentry->full_path_utf8);
		return WIMLIB_ERR_NTFS_3G;
	}
	progress_info->extract.completed_bytes += wim_resource_size(lte);
	return 0;
}

static int do_apply_dentry_ntfs(struct dentry *dentry, ntfs_inode *dir_ni,
				struct apply_args *args);

/*
 * If @dentry is part of a hard link group, search for hard-linked dentries in
 * the same directory that have a nonempty DOS (short) filename.  There should
 * be exactly 0 or 1 such dentries.  If there is 1, extract that dentry first,
 * so that the DOS name is correctly associated with the corresponding long name
 * in the Win32 namespace, and not any of the additional names in the POSIX
 * namespace created from hard links.
 */
static int preapply_dentry_with_dos_name(struct dentry *dentry,
				    	 ntfs_inode **dir_ni_p,
					 struct apply_args *args)
{
	struct dentry *other;
	struct dentry *dentry_with_dos_name;

	dentry_with_dos_name = NULL;
	inode_for_each_dentry(other, dentry->d_inode) {
		if (other != dentry && (dentry->parent == other->parent)
		    && other->short_name_len)
		{
			if (dentry_with_dos_name) {
				ERROR("Found multiple DOS names for file `%s' "
				      "in the same directory",
				      dentry_with_dos_name->full_path_utf8);
				return WIMLIB_ERR_INVALID_DENTRY;
			}
			dentry_with_dos_name = other;
		}
	}
	/* If there's a dentry with a DOS name, extract it first */
	if (dentry_with_dos_name && !dentry_with_dos_name->is_extracted) {
		char *p;
		const char *dir_name;
		char orig;
		int ret;
		ntfs_volume *vol = (*dir_ni_p)->vol;

		DEBUG("pre-applying DOS name `%s'",
		      dentry_with_dos_name->full_path_utf8);
		ret = do_apply_dentry_ntfs(dentry_with_dos_name,
					   *dir_ni_p, args);
		if (ret != 0)
			return ret;

		*dir_ni_p = dentry_open_parent_ni(dentry, vol);
		if (!*dir_ni_p)
			return WIMLIB_ERR_NTFS_3G;
	}
	return 0;
}

/*
 * Applies a WIM dentry to a NTFS filesystem.
 *
 * @dentry:  The WIM dentry to apply
 * @dir_ni:  The NTFS inode for the parent directory
 *
 * @return:  0 on success; nonzero on failure.
 */
static int do_apply_dentry_ntfs(struct dentry *dentry, ntfs_inode *dir_ni,
				struct apply_args *args)
{
	int ret = 0;
	mode_t type;
	ntfs_inode *ni = NULL;
	bool is_hardlink = false;
	ntfs_volume *vol = dir_ni->vol;
	struct inode *inode = dentry->d_inode;
	dentry->is_extracted = 1;

	if (inode->attributes & FILE_ATTRIBUTE_DIRECTORY) {
		type = S_IFDIR;
	} else {
		/* If this dentry is hard-linked to any other dentries in the
		 * same directory, make sure to apply the one (if any) with a
		 * DOS name first.  Otherwise, NTFS-3g might not assign the file
		 * names correctly. */
		if (dentry->short_name_len == 0) {
			ret = preapply_dentry_with_dos_name(dentry,
							    &dir_ni, args);
			if (ret != 0)
				return ret;
		}

		type = S_IFREG;

		if (inode->link_count > 1) {
			/* Already extracted another dentry in the hard link
			 * group.  Make a hard link instead of extracting the
			 * file data. */
			if (inode->extracted_file) {
				ret = apply_ntfs_hardlink(dentry, inode,
							  &dir_ni);
				is_hardlink = true;
				if (ret)
					goto out_close_dir_ni;
				else
					goto out_set_dos_name;
			}
			/* Can't make a hard link; extract the file itself */
			FREE(inode->extracted_file);
			inode->extracted_file = STRDUP(dentry->full_path_utf8);
			if (!inode->extracted_file) {
				ret = WIMLIB_ERR_NOMEM;
				goto out_close_dir_ni;
			}
		}
	}

	/*
	 * Create a directory or file.
	 *
	 * Note: For symbolic links that are not directory junctions, pass
	 * S_IFREG here, since we manually set the reparse data later.
	 */
	ni = ntfs_create(dir_ni, 0, (ntfschar*)dentry->file_name,
			 dentry->file_name_len / 2, type);

	if (!ni) {
		ERROR_WITH_ERRNO("Could not create NTFS object for `%s'",
				 dentry->full_path_utf8);
		ret = WIMLIB_ERR_NTFS_3G;
		goto out_close_dir_ni;
	}

	/* Write the data streams, unless this is a directory or reparse point
	 * */
	if (!(inode->attributes & (FILE_ATTRIBUTE_REPARSE_POINT |
				   FILE_ATTRIBUTE_DIRECTORY))) {
		ret = write_ntfs_data_streams(ni, dentry, &args->progress);
		if (ret != 0)
			goto out_close_dir_ni;
	}


	ret = apply_file_attributes_and_security_data(ni, dir_ni,
						      dentry, args->w);
	if (ret != 0)
		goto out_close_dir_ni;

	if (inode->attributes & FILE_ATTR_REPARSE_POINT) {
		ret = apply_reparse_data(ni, dentry, &args->progress);
		if (ret != 0)
			goto out_close_dir_ni;
	}

out_set_dos_name:
	/* Set DOS (short) name if given */
	if (dentry->short_name_len != 0) {

		char *short_name_utf8;
		size_t short_name_utf8_len;
		ret = utf16_to_utf8(dentry->short_name,
				    dentry->short_name_len,
				    &short_name_utf8,
				    &short_name_utf8_len);
		if (ret != 0)
			goto out_close_dir_ni;

		if (is_hardlink) {
			wimlib_assert(ni == NULL);
			ni = ntfs_pathname_to_inode(vol, dir_ni,
						    dentry->file_name_utf8);
			if (!ni) {
				ERROR_WITH_ERRNO("Could not find NTFS inode for `%s'",
						 dentry->full_path_utf8);
				ret = WIMLIB_ERR_NTFS_3G;
				goto out_close_dir_ni;
			}
		}

		wimlib_assert(ni != NULL);

		DEBUG("Setting short (DOS) name of `%s' to %s",
		      dentry->full_path_utf8, short_name_utf8);

		ret = ntfs_set_ntfs_dos_name(ni, dir_ni, short_name_utf8,
					     short_name_utf8_len, 0);
		FREE(short_name_utf8);
		if (ret != 0) {
			ERROR_WITH_ERRNO("Could not set DOS (short) name for `%s'",
					 dentry->full_path_utf8);
			ret = WIMLIB_ERR_NTFS_3G;
		}
		/* inodes have been closed by ntfs_set_ntfs_dos_name(). */
		return ret;
	}

out_close_dir_ni:
	if (dir_ni) {
		if (ni) {
			if (ntfs_inode_close_in_dir(ni, dir_ni)) {
				if (ret == 0)
					ret = WIMLIB_ERR_NTFS_3G;
				ERROR_WITH_ERRNO("Failed to close inode for `%s'",
						 dentry->full_path_utf8);
			}
		}
		if (ntfs_inode_close(dir_ni)) {
			if (ret == 0)
				ret = WIMLIB_ERR_NTFS_3G;
			ERROR_WITH_ERRNO("Failed to close directory inode");
		}
	} else {
		wimlib_assert(ni == NULL);
	}
	return ret;
}

static int apply_root_dentry_ntfs(const struct dentry *dentry,
				  ntfs_volume *vol, const WIMStruct *w)
{
	ntfs_inode *ni;
	int ret = 0;

	wimlib_assert(dentry_is_directory(dentry));
	ni = ntfs_pathname_to_inode(vol, NULL, "/");
	if (!ni) {
		ERROR_WITH_ERRNO("Could not find root NTFS inode");
		return WIMLIB_ERR_NTFS_3G;
	}
	ret = apply_file_attributes_and_security_data(ni, ni, dentry, w);
	if (ntfs_inode_close(ni) != 0) {
		ERROR_WITH_ERRNO("Failed to close NTFS inode for root "
				 "directory");
		ret = WIMLIB_ERR_NTFS_3G;
	}
	return ret;
}

/* Applies a WIM dentry to the NTFS volume */
int apply_dentry_ntfs(struct dentry *dentry, void *arg)
{
	struct apply_args *args = arg;
	ntfs_volume *vol = args->vol;
	WIMStruct *w = args->w;
	ntfs_inode *dir_ni;

	if (dentry_is_root(dentry))
		return apply_root_dentry_ntfs(dentry, vol, w);

	dir_ni = dentry_open_parent_ni(dentry, vol);
	if (dir_ni)
		return do_apply_dentry_ntfs(dentry, dir_ni, arg);
	else
		return WIMLIB_ERR_NTFS_3G;
}

int apply_dentry_timestamps_ntfs(struct dentry *dentry, void *arg)
{
	struct apply_args *args = arg;
	ntfs_volume *vol = args->vol;
	u8 *p;
	u8 buf[24];
	ntfs_inode *ni;
	int ret = 0;

	DEBUG("Setting timestamps on `%s'", dentry->full_path_utf8);

	ni = ntfs_pathname_to_inode(vol, NULL, dentry->full_path_utf8);
	if (!ni) {
		ERROR_WITH_ERRNO("Could not find NTFS inode for `%s'",
				 dentry->full_path_utf8);
		return WIMLIB_ERR_NTFS_3G;
	}

	p = buf;
	p = put_u64(p, dentry->d_inode->creation_time);
	p = put_u64(p, dentry->d_inode->last_write_time);
	p = put_u64(p, dentry->d_inode->last_access_time);
	ret = ntfs_inode_set_times(ni, (const char*)buf, 3 * sizeof(u64), 0);
	if (ret != 0) {
		ERROR_WITH_ERRNO("Failed to set NTFS timestamps on `%s'",
				 dentry->full_path_utf8);
		ret = WIMLIB_ERR_NTFS_3G;
	}

	if (ntfs_inode_close(ni) != 0) {
		if (ret == 0)
			ret = WIMLIB_ERR_NTFS_3G;
		ERROR_WITH_ERRNO("Failed to close NTFS inode for `%s'",
				 dentry->full_path_utf8);
	}
	return ret;
}
