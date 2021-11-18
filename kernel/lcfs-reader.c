/*
 * composefs
 *
 * Copyright (C) 2021 Giuseppe Scrivano
 *
 * This file is released under the GPL.
 */

#include "lcfs.h"
#include "lcfs-reader.h"

#ifdef FUZZING
# define GFP_KERNEL 0
# include <stdio.h>
# include <errno.h>
# include <stdlib.h>
# include <string.h>
# include <fcntl.h>
# define kfree free
# define vfree free
# define min(a, b) ((a)<(b)?(a):(b))
# define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)

void *kzalloc(size_t len, int ignored)
{
	return malloc(len);
}

#else
# include <linux/string.h>
# include <linux/kernel_read_file.h>
# include <linux/vmalloc.h>
# include <linux/slab.h>
# include <linux/bsearch.h>
# include <linux/overflow.h>
#endif

struct lcfs_context_s {
	struct lcfs_header_s header;

#ifdef FUZZING
	char *descriptor;
#else
	struct file *descriptor;
#endif

	size_t descriptor_len;
};

#ifdef FUZZING
struct lcfs_context_s *lcfs_create_ctx_from_memory(char *blob, size_t size)
{
	struct lcfs_context_s *ctx;
	struct lcfs_header_s *h;
	int ret;

	if (size < sizeof(struct lcfs_header_s) + sizeof(struct lcfs_inode_s))
		goto fail_einval;

	h = (struct lcfs_header_s *)blob;

	if (h->version != LCFS_VERSION)
		goto fail_einval;

	if (h->inode_len != sizeof(struct lcfs_inode_s))
		goto fail_einval;

	if (h->inode_data_len != sizeof(struct lcfs_inode_data_s))
		goto fail_einval;

	ctx = kzalloc(sizeof(struct lcfs_context_s), GFP_KERNEL);
	ret = -ENOMEM;
	if (ctx == NULL)
		goto fail;

	ctx->header = *h;
	ctx->descriptor = blob;
	ctx->descriptor_len = size;

	return ctx;
fail_einval:
	ret = -EINVAL;
fail:
	return ERR_PTR(ret);
}

#else

struct lcfs_context_s *lcfs_create_ctx(char *descriptor_path)
{
	struct lcfs_context_s *ctx;
	struct file *descriptor;
	loff_t i_size;

	if (sizeof(void *) != sizeof(lcfs_off_t)) {
		return ERR_PTR(-ENOTSUPP);
	}

	descriptor = filp_open(descriptor_path, O_RDONLY, 0);
	if (IS_ERR(descriptor))
		return ERR_CAST(descriptor);

	i_size = i_size_read(file_inode(descriptor));
	if (i_size <= 0 || i_size > SIZE_MAX) {
		fput(descriptor);
		return ERR_PTR(-EINVAL);
	}

	ctx = kzalloc(sizeof(struct lcfs_context_s), GFP_KERNEL);
	if (ctx == NULL) {
		fput(descriptor);
		return ERR_PTR(-ENOMEM);
	}

	ctx->descriptor = descriptor;
	ctx->descriptor_len = (size_t) i_size;

	return ctx;
}

#endif

void lcfs_destroy_ctx(struct lcfs_context_s *ctx)
{
	if (!ctx)
		return;
#ifdef FUZZING
	vfree(ctx->descriptor);
#else
	fput(ctx->descriptor);
#endif
	kfree(ctx);
}

struct lcfs_dentry_s *lcfs_get_dentry(struct lcfs_context_s *ctx, size_t index,
				      struct lcfs_dentry_s *buffer)
{
	struct lcfs_vdata_s vdata = {
		.off = index,
		.len = sizeof(struct lcfs_dentry_s),
	};
	return lcfs_get_vdata(ctx, vdata, buffer);
}

void *lcfs_get_vdata(struct lcfs_context_s *ctx,
		     const struct lcfs_vdata_s vdata,
		     void *dest)
{
#ifdef FUZZING
	size_t off = vdata.off;
	size_t len = vdata.len;
	size_t index_end;
	size_t index;

	/* Verify that both ends are contained inside the blob data.  */
	if (check_add_overflow(sizeof(struct lcfs_header_s), off, &index))
		return ERR_PTR(-EFSCORRUPTED);

	if (index >= ctx->descriptor_len)
		return ERR_PTR(-EFSCORRUPTED);

	if (check_add_overflow(index, len, &index_end))
		return ERR_PTR(-EFSCORRUPTED);

	if (index_end > ctx->descriptor_len)
		return ERR_PTR(-EFSCORRUPTED);

	if (!dest)
		return NULL;

	memcpy(dest, ctx->descriptor + index, len);
#else
	size_t copied;
	loff_t pos = vdata.off + sizeof(struct lcfs_header_s);

	if (!dest)
		return NULL;

	copied = 0;
	while (copied < vdata.len) {
		ssize_t bytes;

		bytes = kernel_read(ctx->descriptor, dest + copied,
				    vdata.len - copied, &pos);
		if (bytes < 0)
			return ERR_PTR(bytes);
		if (bytes == 0)
			return ERR_PTR(-EINVAL);

		copied += bytes;
	}

	if (copied != vdata.len)
		return ERR_PTR(-EFSCORRUPTED);
#endif
	return dest;
}

char *lcfs_c_string(struct lcfs_context_s *ctx, struct lcfs_vdata_s vdata, size_t *len,
		    char *buf, size_t max)
{
	char *cstr;

	if (vdata.len == 0)
		return "";

	if (vdata.len > max)
		return ERR_PTR(-EINVAL);

	cstr = lcfs_get_vdata(ctx, vdata, buf);
	if (IS_ERR(cstr))
		return ERR_CAST(cstr);

	/* Make sure the string is NUL terminated.  */
	if (cstr[vdata.len - 1] != '\0')
		return ERR_PTR(-EFSCORRUPTED);

	if (len)
		*len = vdata.len;

	return cstr;
}

struct lcfs_inode_s *lcfs_get_ino_index(struct lcfs_context_s *ctx,
					lcfs_off_t index,
					struct lcfs_inode_s *buffer)
{
	const struct lcfs_vdata_s vdata = {
		.off = index,
		.len = sizeof(struct lcfs_inode_s),
	};
	return lcfs_get_vdata(ctx, vdata, buffer);
}

lcfs_off_t lcfs_get_root_index(struct lcfs_context_s *ctx)
{
	lcfs_off_t payload_len;

	payload_len = ctx->descriptor_len - sizeof(struct lcfs_header_s);
	return payload_len - sizeof(struct lcfs_inode_s);
}

struct lcfs_inode_s *lcfs_dentry_inode(struct lcfs_context_s *ctx,
				       struct lcfs_dentry_s *node,
				       struct lcfs_inode_s *buffer)
{
	return lcfs_get_ino_index(ctx, node->inode_index, buffer);
}

struct lcfs_inode_data_s *lcfs_inode_data(struct lcfs_context_s *ctx,
					  struct lcfs_inode_s *ino,
					  struct lcfs_inode_data_s *buffer)
{
	const struct lcfs_vdata_s vdata = {
		.off = ino->inode_data_index,
		.len = sizeof(struct lcfs_inode_data_s),
	};
	return lcfs_get_vdata(ctx, vdata, buffer);
}

ssize_t lcfs_list_xattrs(struct lcfs_context_s *ctx, struct lcfs_inode_s *ino, char *names, size_t size)
{
	const struct lcfs_xattr_header_s *xattrs;
	size_t n_xattrs = 0, i;
	ssize_t copied = 0;

	if (ino->xattrs.len == 0)
		return 0;

	/* Check for overflows.  */
	xattrs = lcfs_get_vdata(ctx, ino->xattrs, NULL);
	if (IS_ERR(xattrs))
		return PTR_ERR(xattrs);

	n_xattrs = ino->xattrs.len / sizeof(struct lcfs_xattr_header_s);

	for (i = 0; i < n_xattrs; i++) {
		struct lcfs_xattr_header_s h_buf;
		struct lcfs_xattr_header_s *h;
		char xattr_buf[XATTR_NAME_MAX];
		struct lcfs_vdata_s vdata;
		const void *xattr;

		vdata.off = ino->xattrs.off + i * sizeof (*h);
		vdata.len = sizeof(*h);

		/* Read the xattr header.  */
		h = lcfs_get_vdata(ctx, vdata, &h_buf);
		if (IS_ERR(h))
			return PTR_ERR(h);

		if (h->key.len > XATTR_NAME_MAX)
			return -EFSCORRUPTED;

		xattr = lcfs_get_vdata(ctx, h->key, xattr_buf);
		if (IS_ERR(xattr))
			return PTR_ERR(xattr);

		if (size) {
			if (size - copied < h->key.len + 1)
				return -E2BIG;

			memcpy(names + copied, xattr, h->key.len);
			names[copied + h->key.len] = '\0';
		}
		copied += h->key.len + 1;
	}
	return copied;
}

int lcfs_get_xattr(struct lcfs_context_s *ctx, struct lcfs_inode_s *ino, const char *name, void *value, size_t size)
{
	const struct lcfs_xattr_header_s *xattrs;
	size_t name_len = strlen(name);
	size_t n_xattrs = 0, i;
	ssize_t copied = 0;

	if (ino->xattrs.len == 0)
		return 0;

	if (name_len > XATTR_NAME_MAX)
		return 0;

	/* Check for overflows.  */
	xattrs = lcfs_get_vdata(ctx, ino->xattrs, NULL);
	if (IS_ERR(xattrs))
		return PTR_ERR(xattrs);

	n_xattrs = ino->xattrs.len / sizeof(struct lcfs_xattr_header_s);

	for (i = 0; i < n_xattrs; i++) {
		struct lcfs_xattr_header_s h_buf;
		struct lcfs_xattr_header_s *h;
		char xattr_buf[XATTR_NAME_MAX];
		struct lcfs_vdata_s vdata;
		const void *v, *xattr;

		vdata.off = ino->xattrs.off + i * sizeof (*h);
		vdata.len = sizeof(*h);

		/* Read the xattr header.  */
		h = lcfs_get_vdata(ctx, vdata, &h_buf);
		if (IS_ERR(h))
			return PTR_ERR(h);

		if (h->key.len != name_len)
			continue;

		/* Read the name.  */
		xattr = lcfs_get_vdata(ctx, h->key, xattr_buf);
		if (IS_ERR(xattr))
			return PTR_ERR(xattr);

		if (memcmp(xattr, name, name_len) != 0)
			continue;

		if (size == 0)
			return h->value.len;

		if (size - copied < h->value.len + 1)
			return -E2BIG;

		/* Read its value directly into VALUE.  */
		v = lcfs_get_vdata(ctx, h->value, value);
		if (IS_ERR(v))
			return PTR_ERR(v);

		return h->key.len;
	}
	return copied;
}

int lcfs_iterate_dir(struct lcfs_context_s *ctx, loff_t first, struct lcfs_inode_s *dir_ino, lcfs_dir_iter_cb cb, void *private)
{
	size_t i, entries;
	void *check;

	/* Check for overflows.  */
	check = lcfs_get_vdata(ctx, dir_ino->u.dir, NULL);
	if (IS_ERR(check))
		return PTR_ERR(check);

	entries = dir_ino->u.dir.len / sizeof(struct lcfs_dentry_s);

	for (i = first; i < entries; i++) {
		struct lcfs_inode_data_s ino_data_buf;
		struct lcfs_inode_data_s *ino_data;
		struct lcfs_dentry_s dentry_buf;
		struct lcfs_dentry_s *dentry;
		struct lcfs_inode_s ino_buf;
		struct lcfs_inode_s *ino;
		char name_buf[NAME_MAX];
		size_t name_len = 0;
		const char *name;
		size_t nd;

		nd = i * sizeof(struct lcfs_dentry_s);

		dentry = lcfs_get_dentry(ctx, dir_ino->u.dir.off + nd,
					 &dentry_buf);
		if (IS_ERR(dentry))
			return PTR_ERR(dentry);

		name = lcfs_c_string(ctx, dentry->name, &name_len,
				     name_buf, NAME_MAX);
		if (IS_ERR(name))
			return PTR_ERR(name);

		ino = lcfs_dentry_inode(ctx, dentry, &ino_buf);
		if (IS_ERR(ino))
			return PTR_ERR(ino);

		ino_data = lcfs_inode_data(ctx, ino, &ino_data_buf);
		if (IS_ERR(ino_data))
			return PTR_ERR(ino_data);

		if (!cb(private, name, name_len, lcfs_dentry_ino(dentry),
			      ino_data->st_mode & S_IFMT))
			break;
	}
	return 0;
}

struct bsearch_key_s {
	const char *name;
	struct lcfs_context_s *ctx;
	int err;
};

/* The first argument is the KEY, so take advantage to pass additional data.  */
static int compare_names(const void *a, const void *b)
{
	struct bsearch_key_s *key = (struct bsearch_key_s *) a;
	const struct lcfs_dentry_s *dentry;
	struct lcfs_dentry_s dentry_buf;
	char buf[NAME_MAX];
	const char *name;

	dentry = lcfs_get_dentry(key->ctx, (size_t) b, &dentry_buf);
	if (IS_ERR(dentry)) {
		key->err = PTR_ERR(dentry);
		return 0;
	}

	name = lcfs_c_string(key->ctx, dentry->name, NULL, buf, NAME_MAX);
	if (IS_ERR(name)) {
		key->err = PTR_ERR(name);
		return 0;
	}
	return strcmp(key->name, name);
}

int lcfs_lookup(struct lcfs_context_s *ctx, struct lcfs_inode_s *dir, const char *name, lcfs_off_t *index)
{
	struct lcfs_dentry_s *found;
	struct bsearch_key_s key = {
		.name = name,
		.ctx = ctx,
		.err = 0,
	};
	size_t size, n_entries;
	void *check;

	/* Check the entire directory is in the blob.  */
	check = lcfs_get_vdata(ctx, dir->u.dir, NULL);
	if (check)
		return PTR_ERR(check);

	size = sizeof(struct lcfs_dentry_s);
	n_entries = dir->u.dir.len / size;

	found = bsearch(&key, NULL + dir->u.dir.off, n_entries,
			size, compare_names);
	if (found == NULL || key.err)
		return 0;

	*index = (lcfs_off_t) found;

	return 1;
}

const char *lcfs_get_payload(struct lcfs_context_s *ctx, struct lcfs_inode_s *ino, void *buf)
{
	const char *real_path;

	if (ino->u.file.payload.len == 0)
		return ERR_PTR(-EINVAL);

	real_path = lcfs_c_string(ctx, ino->u.file.payload, NULL, buf, PATH_MAX);
	if (real_path == NULL)
		return ERR_PTR(-EIO);

	return real_path;
}
