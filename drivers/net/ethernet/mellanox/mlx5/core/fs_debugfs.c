/* Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/module.h>
#include "fs_core.h"
#include <linux/debugfs.h>
#include <linux/in.h>
#include <linux/cache.h>

static int fs_debugfs_add_fte(struct fs_base *base);
static int fs_debugfs_add_fg(struct fs_base *base);
static int fs_debugfs_add_ft(struct fs_base *base);
static int fs_debugfs_add_prio(struct fs_base *base);
static int fs_debugfs_add_ns(struct fs_base *base);
static int fs_debugfs_add_dst(struct fs_base *base);

static int fs_open(struct inode *inode, struct file *filp)
{
	int ret;
	struct fs_base *base;

	ret = simple_open(inode, filp);
	if (ret)
		return ret;

	if (!filp->private_data)
		return -ENOENT;

	base = filp->private_data;
	kref_get(&base->refcount);

	return ret;
}

static int fs_match_open(struct inode *inode, struct file *filp)
{
	int ret;
	struct fs_base *base;
	struct fs_debugfs_match_header_ctx *ctx;

	ret = simple_open(inode, filp);
	if (ret)
		return ret;

	if (!filp->private_data)
		return -ENOENT;

	ctx = filp->private_data;
	base = ctx->base;
	kref_get(&base->refcount);

	return ret;
}

void fs_debugfs_remove(struct fs_base *base)
{
	debugfs_remove_recursive(base->debugfs.dir);
}

static int _fs_release(void *ptr)
{
	struct fs_base *base;

	if (!ptr)
		return 0;

	base = ptr;
	kref_put(&base->refcount, _fs_remove_node);

	return 0;
}

static int fs_release(struct inode *inode, struct file *filp)
{
	return _fs_release(filp->private_data);
}

static int fs_match_release(struct inode *inode, struct file *filp)
{
	struct fs_base *base;
	struct fs_debugfs_match_header_ctx *ctx;

	if (!filp->private_data)
		return 0;

	ctx = filp->private_data;
	base = ctx->base;
	kref_put(&base->refcount, _fs_remove_node);

	return 0;
}

static ssize_t type_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *pos)
{
	struct fs_base *base = filp->private_data;

	switch (base->type) {
	case FS_TYPE_FLOW_ENTRY:
		return simple_read_from_buffer(buf, count, pos, "flow entry\n",
					       sizeof("flow entry\n") - 1);
	case FS_TYPE_FLOW_DEST:
		return simple_read_from_buffer(buf, count, pos, "flow dest\n",
					       sizeof("flow dest\n") - 1);
	case FS_TYPE_NAMESPACE:
		return simple_read_from_buffer(buf, count, pos, "namespace\n",
					       sizeof("namespace\n") - 1);
	case FS_TYPE_FLOW_TABLE:
		return simple_read_from_buffer(buf, count, pos, "flow table\n",
					       sizeof("flow table\n") - 1);
	case FS_TYPE_PRIO:
		return simple_read_from_buffer(buf, count, pos, "priority\n",
					       sizeof("priority\n") - 1);
	case FS_TYPE_FLOW_GROUP:
		return simple_read_from_buffer(buf, count, pos, "flow group\n",
					       sizeof("flow group\n") - 1);
	}

	return 0;
}

static const struct file_operations fops_type_read = {
	.owner	= THIS_MODULE,
	.open	= fs_open,
	.read	= type_read,
	.release = fs_release,
};

static int add_obj_debugfs(struct fs_base *base)
{
	int err = 0;

	mutex_lock(&base->lock);
	switch (base->type) {
	case FS_TYPE_FLOW_TABLE:
		err = fs_debugfs_add_ft(base);
		break;
	case FS_TYPE_FLOW_GROUP:
		err = fs_debugfs_add_fg(base);
		break;
	case FS_TYPE_PRIO:
		err = fs_debugfs_add_prio(base);
		break;
	case FS_TYPE_FLOW_ENTRY:
		err = fs_debugfs_add_fte(base);
		break;
	case FS_TYPE_NAMESPACE:
		err = fs_debugfs_add_ns(base);
		break;
	case FS_TYPE_FLOW_DEST:
		err = fs_debugfs_add_dst(base);
		break;
	}

	if (err)
		debugfs_remove_recursive(base->debugfs.dir);

	mutex_unlock(&base->lock);

	return err;
}

static struct dentry *get_debugfs_parent(struct fs_base *base)
{
	if (!base->parent)
		return ((struct mlx5_flow_root_namespace *)base)->dev->priv.dbg_root;

	switch (base->type) {
	case FS_TYPE_NAMESPACE: {
		struct fs_prio *prio;

		fs_get_parent(prio, container_of(base,
						 struct mlx5_flow_namespace,
						 base));
		return prio->debugfs.ns;
	}
	case FS_TYPE_FLOW_TABLE: {
		struct fs_prio *prio;

		fs_get_parent(prio, container_of(base, struct mlx5_flow_table,
						 base));
		return prio->debugfs.fts;
	}
	case FS_TYPE_FLOW_GROUP: {
		struct mlx5_flow_table *ft;

		fs_get_parent(ft, container_of(base, struct mlx5_flow_group,
					       base));
		return ft->debugfs.fgs;
	}
	case FS_TYPE_FLOW_ENTRY: {
		struct mlx5_flow_group *fg;

		fs_get_parent(fg, container_of(base, struct fs_fte,
					       base));
		return fg->debugfs.ftes;
	}
	case FS_TYPE_FLOW_DEST: {
		struct fs_fte *fte;

		fs_get_parent(fte, container_of(base, struct mlx5_flow_rule,
						base));
		return fte->debugfs.dests;
	}
	case FS_TYPE_PRIO: {
		struct mlx5_flow_namespace *ns;

		fs_get_parent(ns, container_of(base, struct fs_prio,
					       base));
		return ns->debugfs.prios;
	}
	}

	WARN_ON(true);
	return ERR_PTR(-EINVAL);
}

#define FS_DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __fmt)		\
static int __fops ## _open(struct inode *inode, struct file *filp)	\
{									\
	int ret;							\
	struct fs_base *base;						\
									\
	ret = simple_attr_open(inode, filp, __get, NULL, __fmt);	\
	if (ret)							\
		return ret;						\
	base = inode->i_private;					\
	kref_get(&base->refcount);					\
	return ret;							\
}									\
static int __fops ## _release(struct inode *inode, struct file *file)	\
{									\
	_fs_release(inode->i_private);					\
	simple_attr_release(inode, file);				\
	return 0;							\
}									\
static const struct file_operations __fops = {				\
	.owner	 = THIS_MODULE,						\
	.open	 = __fops ## _open,					\
	.release = __fops ##_release,					\
	.read	 = simple_attr_read,					\
	.write	 = simple_attr_write,					\
	.llseek	 = generic_file_llseek,					\
}

/*Read user refcount attribute*/
static int fs_debugfs_read_users_refcount(void *attr, u64 *data)
{
	struct fs_base *base = attr;

	*data = atomic_read(&base->users_refcount);
	return 0;
}

/*Read priority ttributes*/
static int fs_debugfs_read_priority(void *attr, u64 *data)
{
	struct fs_prio *prio = attr;

	*data = prio->prio;
	return 0;
}

/*READ Flow table attributes*/
static int fs_debugfs_read_ft_level(void *attr, u64 *data)
{
	struct mlx5_flow_table *ft = attr;

	*data = ft->level;
	return 0;
}

static int fs_debugfs_read_ft_id(void *attr, u64 *data)
{
	struct mlx5_flow_table *ft = attr;

	*data = ft->id;
	return 0;
}

static int fs_debugfs_read_ft_max_fte(void *attr, u64 *data)
{
	struct mlx5_flow_table *ft = attr;

	*data = ft->max_fte;
	return 0;
}

static int fs_debugfs_read_ft_autogroup_max_types(void *attr, u64 *data)
{
	struct mlx5_flow_table *ft = attr;

	*data = ft->autogroup.max_types;
	return 0;
}

static int fs_debugfs_read_ft_autogroup_num_types(void *attr, u64 *data)
{
	struct mlx5_flow_table *ft = attr;

	*data = ft->autogroup.num_types;
	return 0;
}

/*Read flow group attributes*/
static int fs_debugfs_read_fg_max_ftes(void *attr, u64 *data)
{
	struct mlx5_flow_group *fg = attr;

	*data = fg->max_ftes;
	return 0;
}

static int fs_debugfs_read_fg_num_ftes(void *attr, u64 *data)
{
	struct mlx5_flow_group *fg = attr;

	*data = fg->num_ftes;
	return 0;
}

static int fs_debugfs_read_fg_start_index(void *attr, u64 *data)
{
	struct mlx5_flow_group *fg = attr;

	*data = fg->start_index;
	return 0;
}

static int fs_debugfs_read_fg_id(void *attr, u64 *data)
{
	struct mlx5_flow_group *fg = attr;

	*data = fg->id;
	return 0;
}

static int fs_debugfs_read_fg_match_criteria_enable(void *attr, u64 *data)
{
	struct mlx5_flow_group *fg = attr;

	*data = fg->mask.match_criteria_enable;
	return 0;
}

/*Read flow table entry attributes*/
static int fs_debugfs_read_fte_index(void *attr, u64 *data)
{
	struct fs_fte *fte = attr;

	*data = fte->index;
	return 0;
}

static int fs_debugfs_read_fte_flow_tag(void *attr, u64 *data)
{
	struct fs_fte *fte = attr;

	*data = fte->flow_tag;
	return 0;
}

static int fs_debugfs_read_fte_dests_size(void *attr, u64 *data)
{
	struct fs_fte *fte = attr;

	*data = fte->dests_size;
	return 0;
}

/*Read destination(tir) attributes*/
static int fs_debugfs_read_dst_tir(void *attr, u64 *data)
{
	struct mlx5_flow_rule *dst = attr;

	*data = dst->dest_attr.tir_num;
	return 0;
}

/*Define base attributes*/
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_base_users_refcount,
			   fs_debugfs_read_users_refcount, "%u\n");
/*Define priority attributes*/
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_prio, fs_debugfs_read_priority, "%u\n");
/*Define flow table attributes*/
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_ft_level, fs_debugfs_read_ft_level, "%u\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_ft_id, fs_debugfs_read_ft_id, "0x%x\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_ft_max_fte, fs_debugfs_read_ft_max_fte, "%u\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_ft_autogroup_max_types,
			   fs_debugfs_read_ft_autogroup_max_types, "%u\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_ft_autogroup_num_types,
			   fs_debugfs_read_ft_autogroup_num_types, "%u\n");
/*Define flow group attributes*/
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fg_max_ftes, fs_debugfs_read_fg_max_ftes,
			   "%u\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fg_num_ftes, fs_debugfs_read_fg_num_ftes,
			   "%u\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fg_start_index, fs_debugfs_read_fg_start_index,
			   "%u\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fg_id, fs_debugfs_read_fg_id, "0x%x\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fg_match_criteria_enable,
			   fs_debugfs_read_fg_match_criteria_enable, "%u\n");
/*Define flow table entry attributes*/
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fte_index, fs_debugfs_read_fte_index,
			   "0x%x\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fte_flow_tag, fs_debugfs_read_fte_flow_tag,
			   "0x%x\n");
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_fte_dests_size, fs_debugfs_read_fte_dests_size,
			   "%u\n");

/*Define destination attributes*/
FS_DEFINE_SIMPLE_ATTRIBUTE(fs_dst_tir, fs_debugfs_read_dst_tir, "0x%x\n");


void update_debugfs_dir_name(struct fs_base *base, const char *name)
{
	struct dentry *parent;

	parent = get_debugfs_parent(base);
	debugfs_rename(parent, base->debugfs.dir, parent, name);
}

/* Add objects to debugfs.
 * This function is called after the child was added and
 * the parent can't be freed.
 */
static int fs_debugfs_add_base(struct fs_base *base)
{
	struct dentry *parent;

	parent = get_debugfs_parent(base);

	base->debugfs.dir = debugfs_create_dir(base->name, parent);
	if (!base->debugfs.dir)
		return -ENOMEM;

	base->debugfs.type = debugfs_create_file("type", 0400,
						 base->debugfs.dir,
						 base,
						 &fops_type_read);
	if (!base->debugfs.type)
		return -ENOMEM;

	base->debugfs.users_refcount =
		debugfs_create_file("users_refcount", 0400, base->debugfs.dir,
				    base, &fs_base_users_refcount);
	if (!base->debugfs.users_refcount)
		return -ENOMEM;

	return 0;
}

int fs_debugfs_add(struct fs_base *base)
{
	int err = fs_debugfs_add_base(base);

	if (err)
		return err;

	return add_obj_debugfs(base);
}

int fs_debugfs_add_ns(struct fs_base *base)
{
	struct mlx5_flow_namespace *ns;

	fs_get_obj(ns, base);
	ns->debugfs.prios = debugfs_create_dir("priorities",
					       base->debugfs.dir);
	if (!ns->debugfs.prios)
		return -ENOMEM;

	return 0;
}

int fs_debugfs_add_prio(struct fs_base *base)
{
	struct fs_prio *prio;

	fs_get_obj(prio, base);

	prio->debugfs.prio = debugfs_create_file("priority", 0400,
						 base->debugfs.dir, prio,
						 &fs_prio);
	if (!prio->debugfs.prio)
		return -ENOMEM;

	prio->debugfs.fts = debugfs_create_dir("flow_tables",
					       base->debugfs.dir);
	if (!prio->debugfs.fts)
		return -ENOMEM;

	prio->debugfs.ns = debugfs_create_dir("namespaces",
					      base->debugfs.dir);
	if (!prio->debugfs.ns)
		return -ENOMEM;

	return 0;
}

int fs_debugfs_add_ft(struct fs_base *base)
{
	struct mlx5_flow_table *ft;

	fs_get_obj(ft, base);

	ft->debugfs.level = debugfs_create_file("level", 0400,
						 base->debugfs.dir, ft,
						 &fs_ft_level);
	if (!ft->debugfs.level)
		return -ENOMEM;

	ft->debugfs.id = debugfs_create_file("id", 0400,
					     base->debugfs.dir, ft,
					     &fs_ft_id);
	if (!ft->debugfs.id)
		return -ENOMEM;

	ft->debugfs.max_fte = debugfs_create_file("max_fte", 0400,
						  base->debugfs.dir, ft,
						  &fs_ft_max_fte);
	if (!ft->debugfs.max_fte)
		return -ENOMEM;

	ft->debugfs.fgs = debugfs_create_dir("groups",
					     base->debugfs.dir);
	if (!ft->debugfs.fgs)
		return -ENOMEM;

	if (!ft->autogroup.active)
		return 0;

	ft->debugfs.autogroup.dir =
		debugfs_create_dir("autogroup",
				   base->debugfs.dir);

	if (!ft->debugfs.autogroup.dir)
		return -ENOMEM;

	ft->debugfs.autogroup.max_types =
		debugfs_create_file("max_types", 0400,
				    ft->debugfs.autogroup.dir,
				    ft,
				    &fs_ft_autogroup_max_types);
	if (!ft->debugfs.autogroup.max_types)
		return -ENOMEM;

	ft->debugfs.autogroup.num_types =
		debugfs_create_file("num_types", 0400,
				    ft->debugfs.autogroup.dir,
				    ft,
				    &fs_ft_autogroup_num_types);

	if (!ft->debugfs.autogroup.num_types)
		return -ENOMEM;

	return 0;
}

#define MAC_STR_LEN		19
#define VID_STR_LEN		5
#define PORT_STR_LEN		6
#define IPV4_STR_LEN		17
#define ETHERTYPE_STR_LEN	6
#define PROTOCOL_STR_LEN	4
/*Read flow context value:
 *FTE(Flow Table entry) context: Read flow context values(for e.g. dmac)
 *FG (Flow Group): Read flow context mask.
 */
static ssize_t smac_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	char mac[MAC_STR_LEN];
	int len = snprintf(mac, sizeof(mac), "%pM\n",
			   MLX5_ADDR_OF(fte_match_set_lyr_2_4,
					headers, smac_47_16));
	return simple_read_from_buffer(buf, count, pos, mac, len);
}

static ssize_t dmac_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	char mac[MAC_STR_LEN];
	int len = snprintf(mac, sizeof(mac), "%pM\n",
			   MLX5_ADDR_OF(fte_match_set_lyr_2_4,
					headers,
					dmac_47_16));
	return simple_read_from_buffer(buf, count, pos, mac, len);
}

static ssize_t vid_read(struct file *filp, char __user *buf, size_t count,
			loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[VID_STR_LEN];

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, first_vid);

	len = snprintf(tbuf, sizeof(tbuf), "0x%x\n", field);
	return simple_read_from_buffer(buf, count, pos, tbuf, len);
}

static ssize_t udp_dport_read(struct file *filp, char __user *buf, size_t count,
			      loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[PORT_STR_LEN];
	const char *fmt = ctx->base->type == FS_TYPE_FLOW_GROUP ?
		"0x%x\n" : "%u\n";

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, udp_dport);

	len = snprintf(tbuf, sizeof(tbuf), fmt, field);
	return simple_read_from_buffer(buf, count, pos, tbuf, len);
}

static ssize_t udp_sport_read(struct file *filp, char __user *buf, size_t count,
			      loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[PORT_STR_LEN];
	const char *fmt = ctx->base->type == FS_TYPE_FLOW_GROUP ?
		"0x%x\n" : "%u\n";

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, udp_sport);

	len = snprintf(tbuf, sizeof(tbuf), fmt, field);
	return simple_read_from_buffer(buf, count, pos, tbuf, len);
}

static ssize_t tcp_dport_read(struct file *filp, char __user *buf, size_t count,
			      loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[PORT_STR_LEN];
	const char *fmt = ctx->base->type == FS_TYPE_FLOW_GROUP ?
		"0x%x\n" : "%u\n";

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, tcp_dport);

	len = snprintf(tbuf, sizeof(tbuf), fmt, field);
	return simple_read_from_buffer(buf, count, pos, tbuf, len);
}

static ssize_t tcp_sport_read(struct file *filp, char __user *buf, size_t count,
			      loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[PORT_STR_LEN];
	const char *fmt = ctx->base->type == FS_TYPE_FLOW_GROUP ?
		"0x%x\n" : "%u\n";

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, tcp_sport);

	len = snprintf(tbuf, sizeof(tbuf), fmt, field);
	return simple_read_from_buffer(buf, count, pos, tbuf, len);
}

static ssize_t src_ip_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[IPV4_STR_LEN];

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, src_ip[0]);

	len = snprintf(tbuf, sizeof(tbuf), "%pI4\n", &field);
	return simple_read_from_buffer(buf, count, pos, tbuf, len);
}

static ssize_t dst_ip_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[IPV4_STR_LEN];

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, dst_ip[0]);

	len = snprintf(tbuf, sizeof(tbuf), "%pI4\n", &field);
	return simple_read_from_buffer(buf, count, pos, tbuf, len);
}

static ssize_t ethertype_read(struct file *filp, char __user *buf, size_t count,
			      loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[ETHERTYPE_STR_LEN];

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, ethertype);
	switch (field) {
	case ETH_P_IPV6:
		return simple_read_from_buffer(buf, count, pos, "IPV6\n",
					       sizeof("IPV6\n") - 1);
	case ETH_P_IP:
		return simple_read_from_buffer(buf, count, pos, "IPV4\n",
					       sizeof("IPV4\n") - 1);
	default:
		len = snprintf(tbuf, sizeof(tbuf), "0x%x\n", field);
		return simple_read_from_buffer(buf, count, pos, tbuf, len);
	}
}

static ssize_t ip_protocol_read(struct file *filp, char __user *buf,
				size_t count, loff_t *pos)
{
	struct fs_debugfs_match_header_ctx *ctx = filp->private_data;
	char *headers  = ctx->addr;
	u32 field;
	int len;
	char tbuf[PROTOCOL_STR_LEN];

	field = MLX5_GET(fte_match_set_lyr_2_4, headers, ip_protocol);
	switch (field) {
	case IPPROTO_IP:
		return simple_read_from_buffer(buf, count, pos, "IP\n",
					       sizeof("IP\n") - 1);
	case IPPROTO_ICMP:
		return simple_read_from_buffer(buf, count, pos, "ICMP\n",
					       sizeof("ICMP\n") - 1);
	case IPPROTO_IGMP:
		return simple_read_from_buffer(buf, count, pos, "IGMP\n",
					       sizeof("IGMP\n") - 1);
	case IPPROTO_TCP:
		return simple_read_from_buffer(buf, count, pos, "TCP\n",
					       sizeof("TCP\n") - 1);
	case IPPROTO_EGP:
		return simple_read_from_buffer(buf, count, pos, "EGP\n",
					       sizeof("EGP\n") - 1);
	case IPPROTO_IPIP:
		return simple_read_from_buffer(buf, count, pos, "IPIP\n",
					       sizeof("IPIP\n") - 1);
	case IPPROTO_PUP:
		return simple_read_from_buffer(buf, count, pos, "PUP\n",
					       sizeof("PUP\n") - 1);
	case IPPROTO_UDP:
		return simple_read_from_buffer(buf, count, pos, "UDP\n",
					       sizeof("UDP\n") - 1);
	case IPPROTO_IDP:
		return simple_read_from_buffer(buf, count, pos, "IDP\n",
					       sizeof("IDP\n") - 1);
	case IPPROTO_DCCP:
		return simple_read_from_buffer(buf, count, pos, "DCCP\n",
					       sizeof("DCCP\n") - 1);
	case IPPROTO_RSVP:
		return simple_read_from_buffer(buf, count, pos, "RSVP\n",
					       sizeof("RSVP\n") - 1);
	case IPPROTO_GRE:
		return simple_read_from_buffer(buf, count, pos, "GRE\n",
					       sizeof("GRE\n") - 1);
	case IPPROTO_IPV6:
		return simple_read_from_buffer(buf, count, pos, "IPV6\n",
					       sizeof("IPV6\n") - 1);
	case IPPROTO_ESP:
		return simple_read_from_buffer(buf, count, pos, "ESP\n",
					       sizeof("ESP\n") - 1);
	case IPPROTO_AH:
		return simple_read_from_buffer(buf, count, pos, "AH\n",
					       sizeof("AH\n") - 1);
	default:
		len = snprintf(tbuf, sizeof(tbuf), "0x%x\n", field);
		return simple_read_from_buffer(buf, count, pos, tbuf, len);
	}
}

static ssize_t action_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *pos)
{
	struct fs_fte *fte  = filp->private_data;

	switch (fte->action) {
	case MLX5_FLOW_CONTEXT_ACTION_ALLOW:
		return simple_read_from_buffer(buf, count, pos, "ALLOW\n",
					       sizeof("ALLOW\n") - 1);
	case MLX5_FLOW_CONTEXT_ACTION_DROP:
		return simple_read_from_buffer(buf, count, pos, "DROP\n",
					       sizeof("DROP\n") - 1);
	case MLX5_FLOW_CONTEXT_ACTION_FWD_DEST:
		return simple_read_from_buffer(buf, count, pos, "FORWARD\n",
					       sizeof("FORWARD\n") - 1);
	}

	return 0;
}

static ssize_t dst_type_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *pos)
{
	struct mlx5_flow_rule *dst  = filp->private_data;

	switch (dst->dest_attr.type) {
	case MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE:
		return simple_read_from_buffer(buf, count, pos,
					       "flow table\n",
					       sizeof("flow table\n") - 1);
	case MLX5_FLOW_DESTINATION_TYPE_TIR:
		return simple_read_from_buffer(buf, count, pos, "tir\n",
					       sizeof("tir\n") - 1);
	case MLX5_FLOW_DESTINATION_TYPE_VPORT:
		return simple_read_from_buffer(buf, count, pos, "vport\n",
					       sizeof("vport\n") - 1);
	}
	return 0;
}

#define FS_DEFINE_FOPS(name)				\
static const struct file_operations fops_##name = {		\
	.owner	 = THIS_MODULE,					\
	.open	 = fs_open,					\
	.release = fs_release,					\
	.read	 = name ##_read						\
}

#define FS_DEFINE_MATCH_FOPS(name)				\
static const struct file_operations fops_##name = {		\
	.owner	 = THIS_MODULE,					\
	.open	 = fs_match_open,					\
	.release = fs_match_release,					\
	.read	 = name ##_read						\
}

FS_DEFINE_FOPS(action);
FS_DEFINE_FOPS(dst_type);
FS_DEFINE_MATCH_FOPS(smac);
FS_DEFINE_MATCH_FOPS(dmac);
FS_DEFINE_MATCH_FOPS(vid);
FS_DEFINE_MATCH_FOPS(src_ip);
FS_DEFINE_MATCH_FOPS(dst_ip);
FS_DEFINE_MATCH_FOPS(udp_sport);
FS_DEFINE_MATCH_FOPS(udp_dport);
FS_DEFINE_MATCH_FOPS(tcp_sport);
FS_DEFINE_MATCH_FOPS(tcp_dport);
FS_DEFINE_MATCH_FOPS(ethertype);
FS_DEFINE_MATCH_FOPS(ip_protocol);

/*Size is in bits*/
static bool mask_field_no_zero(void *val, ssize_t size)
{
	if (find_first_bit((unsigned long *)val, size) ==
	    size)
		return false;
	else
		return true;
}

static int fs_debugfs_create_header_files(char *mask_headers,
					  struct fs_debugfs_match_header_ctx *ctx)
{
	void *addr;
	struct fs_debugfs_match_layer_2_4 *header_files = &ctx->header_files;

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, smac_47_16);
	if (mask_field_no_zero(addr, 48)) {
		header_files->smac = debugfs_create_file("smac", 0400,
							 header_files->dir,
							 ctx,
							 &fops_smac);
		if (!header_files->smac)
			return -ENOMEM;
	}
	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, dmac_47_16);
	if (mask_field_no_zero(addr, 48)) {
		header_files->dmac = debugfs_create_file("dmac", 0400,
							 header_files->dir,
							 ctx,
							 &fops_dmac);
		if (!header_files->dmac)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, first_vid);
	if (mask_field_no_zero(addr, 12)) {
		header_files->vid = debugfs_create_file("vlan_id", 0400,
							header_files->dir,
							ctx,
							&fops_vid);
		if (!header_files->vid)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, dst_ip);
	if (mask_field_no_zero(addr, 32)) {
		header_files->dst_ip = debugfs_create_file("dst_ip", 0400,
							   header_files->dir,
							   ctx,
							   &fops_dst_ip);
		if (!header_files->dst_ip)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, src_ip);
	if (mask_field_no_zero(addr, 32)) {
		header_files->src_ip = debugfs_create_file("src_ip", 0400,
							   header_files->dir,
							   ctx,
							   &fops_src_ip);
		if (!header_files->src_ip)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, udp_sport);
	if (mask_field_no_zero(addr, 16)) {
		header_files->udp_sport = debugfs_create_file("udp_sport", 0400,
							      header_files->dir,
							      ctx,
							      &fops_udp_sport);
		if (!header_files->udp_sport)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, udp_dport);
	if (mask_field_no_zero(addr, 16)) {
		header_files->udp_dport = debugfs_create_file("udp_dport", 0400,
							      header_files->dir,
							      ctx,
							      &fops_udp_dport);
		if (!header_files->udp_dport)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, tcp_sport);
	if (mask_field_no_zero(addr, 16)) {
		header_files->tcp_sport = debugfs_create_file("tcp_sport", 0400,
							      header_files->dir,
							      ctx,
							      &fops_tcp_sport);
		if (!header_files->tcp_sport)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, tcp_dport);
	if (mask_field_no_zero(addr, 16)) {
		header_files->tcp_dport = debugfs_create_file("tcp_dport", 0400,
							      header_files->dir,
							      ctx,
							      &fops_tcp_dport);
		if (!header_files->tcp_dport)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, ethertype);
	if (mask_field_no_zero(addr, 16)) {
		header_files->ethertype = debugfs_create_file("ethertype", 0400,
							      header_files->dir,
							      ctx,
							      &fops_ethertype);
		if (!header_files->ethertype)
			return -ENOMEM;
	}

	addr = MLX5_ADDR_OF(fte_match_set_lyr_2_4, mask_headers, ip_protocol);
	if (mask_field_no_zero(addr, 8)) {
		header_files->ip_protocol =
			debugfs_create_file("ip_protocol",
					    0400,
					    header_files->dir,
					    ctx,
					    &fops_ip_protocol);
		if (!header_files->ip_protocol)
			return -ENOMEM;
	}

	return 0;
}

static int fs_debugfs_add_mask_headers(struct fs_debugfs_match_criteria
				       *match_criteria,
				       struct fs_base *base)
{
	struct mlx5_flow_group *fg;
	int err;

	fs_get_obj(fg, base);

	if (fg->mask.match_criteria_enable &
	    1 << MLX5_CREATE_FLOW_GROUP_IN_MATCH_CRITERIA_ENABLE_OUTER_HEADERS) {
		char *outer_headers;

		outer_headers = MLX5_ADDR_OF(fte_match_param,
					     fg->mask.match_criteria,
					     outer_headers);

		match_criteria->outer_headers_ctx.base = base;
		match_criteria->outer_headers_ctx.addr = outer_headers;
		err = fs_debugfs_create_header_files(outer_headers,
						     &match_criteria->outer_headers_ctx);
		if (err)
			return err;
	}

	if (fg->mask.match_criteria_enable &
	    1 << MLX5_CREATE_FLOW_GROUP_IN_MATCH_CRITERIA_ENABLE_INNER_HEADERS) {
		char *inner_headers;

		inner_headers = MLX5_ADDR_OF(fte_match_param,
					     fg->mask.match_criteria,
					     inner_headers);

		match_criteria->inner_headers_ctx.base = base;
		match_criteria->inner_headers_ctx.addr = inner_headers;
		err = fs_debugfs_create_header_files(inner_headers,
						     &match_criteria->inner_headers_ctx);
		if (err)
			return err;
	}
	return 0;
}

static int fs_debugfs_add_val_headers(struct fs_debugfs_match_criteria *match_criteria,
				      struct fs_base *base)
{
	char *outer_headers, *inner_headers;
	struct fs_fte *fte;
	int err;
	struct mlx5_flow_group *fg;

	fs_get_obj(fte, base);
	fs_get_parent(fg, fte);
	outer_headers = MLX5_ADDR_OF(fte_match_param, fte->val, outer_headers);
	inner_headers = MLX5_ADDR_OF(fte_match_param, fte->val, inner_headers);

	if (fg->mask.match_criteria_enable &
	    1 << MLX5_CREATE_FLOW_GROUP_IN_MATCH_CRITERIA_ENABLE_OUTER_HEADERS) {
		char *mask_outer_headers;

		mask_outer_headers = MLX5_ADDR_OF(fte_match_param,
						  fg->mask.match_criteria,
						  outer_headers);

		match_criteria->outer_headers_ctx.base = base;
		match_criteria->outer_headers_ctx.addr = outer_headers;
		err = fs_debugfs_create_header_files(mask_outer_headers,
						     &match_criteria->outer_headers_ctx);
		if (err)
			return err;
	}
	if (fg->mask.match_criteria_enable &
	    1 << MLX5_CREATE_FLOW_GROUP_IN_MATCH_CRITERIA_ENABLE_INNER_HEADERS) {
		char *mask_inner_headers;

		mask_inner_headers = MLX5_ADDR_OF(fte_match_param,
						  fg->mask.match_criteria,
						  inner_headers);

		match_criteria->inner_headers_ctx.base = base;
		match_criteria->inner_headers_ctx.addr = inner_headers;
		err = fs_debugfs_create_header_files(mask_inner_headers,
						     &match_criteria->inner_headers_ctx);
		if (err)
			return err;
	}
	return 0;
}

static int fs_debugfs_add_headers(struct fs_debugfs_match_criteria *match_criteria,
				  struct fs_base *base)
{
	match_criteria->outer_headers_ctx.header_files.dir =
		debugfs_create_dir("outer_headers", match_criteria->dir);
	if (!match_criteria->outer_headers_ctx.header_files.dir)
		return -ENOMEM;

	match_criteria->inner_headers_ctx.header_files.dir =
		debugfs_create_dir("inner_headers", match_criteria->dir);
	if (!match_criteria->inner_headers_ctx.header_files.dir)
		return -ENOMEM;

	if (base->type == FS_TYPE_FLOW_ENTRY)
		return fs_debugfs_add_val_headers(match_criteria, base);
	else
		return fs_debugfs_add_mask_headers(match_criteria, base);
}

static int fs_debugfs_add_mask_misc(struct fs_debugfs_match_criteria *match_criteria,
				    struct fs_base *base)
{
	struct mlx5_flow_group *fg;

	fs_get_obj(fg, base);

	match_criteria->misc_params_ctx.misc_params.dir =
		debugfs_create_dir("misc_params", match_criteria->dir);
		if (!match_criteria->misc_params_ctx.misc_params.dir)
			return -ENOMEM;

	return 0;
}

static int fs_debugfs_add_mask(struct fs_base *base)
{
	int err = 0;
	struct mlx5_flow_group *fg;

	fs_get_obj(fg, base);

	fg->debugfs.mask.match_criteria_enable =
		debugfs_create_file("match_criteria_enable", 0400,
				    base->debugfs.dir, fg,
				    &fs_fg_match_criteria_enable);

	if (!fg->debugfs.mask.match_criteria_enable)
		return -ENOMEM;

	fg->debugfs.mask.match_criteria.dir =
		debugfs_create_dir("mask", base->debugfs.dir);
	if (!fg->debugfs.mask.match_criteria.dir)
		return -ENOMEM;

	err = fs_debugfs_add_headers(&fg->debugfs.mask.match_criteria, base);
	if (err)
		return err;

	return fs_debugfs_add_mask_misc(&fg->debugfs.mask.match_criteria, base);
}

static int fs_debugfs_add_val(struct fs_base *base)
{
	int err;
	struct fs_fte *fte;

	fs_get_obj(fte, base);

	fte->debugfs.match_criteria.dir = debugfs_create_dir("value",
							     base->debugfs.dir);
	if (!fte->debugfs.match_criteria.dir)
		return -ENOMEM;

	err = fs_debugfs_add_headers(&fte->debugfs.match_criteria, base);
	return err;
}

static int fs_debugfs_add_fg(struct fs_base *base)
{
	struct mlx5_flow_group *fg;

	fs_get_obj(fg, base);

	fg->debugfs.max_ftes = debugfs_create_file("max_ftes", 0400,
						   base->debugfs.dir, fg,
						   &fs_fg_max_ftes);
	if (!fg->debugfs.max_ftes)
		return -ENOMEM;

	fg->debugfs.id = debugfs_create_file("id", 0400,
					     base->debugfs.dir, fg,
					     &fs_fg_id);
	if (!fg->debugfs.id)
		return -ENOMEM;

	fg->debugfs.num_ftes = debugfs_create_file("num_ftes", 0400,
						   base->debugfs.dir, fg,
						   &fs_fg_num_ftes);
	if (!fg->debugfs.num_ftes)
		return -ENOMEM;

	fg->debugfs.start_index = debugfs_create_file("start_index", 0400,
						      base->debugfs.dir, fg,
						      &fs_fg_start_index);
	if (!fg->debugfs.start_index)
		return -ENOMEM;

	if (fs_debugfs_add_mask(base))
		return -ENOMEM;

	fg->debugfs.ftes = debugfs_create_dir("entries", base->debugfs.dir);
	if (!fg->debugfs.ftes)
		return -ENOMEM;

	return 0;
}

int fs_debugfs_add_fte(struct fs_base *base)
{
	struct fs_fte *fte;

	fs_get_obj(fte, base);

	fte->debugfs.index = debugfs_create_file("index", 0400,
						 base->debugfs.dir, fte,
						 &fs_fte_index);
	if (!fte->debugfs.index)
		return -ENOMEM;

	fte->debugfs.action = debugfs_create_file("action", 0400,
						   base->debugfs.dir, fte,
						   &fops_action);
	if (!fte->debugfs.action)
		return -ENOMEM;

	fte->debugfs.flow_tag = debugfs_create_file("flow_tag", 0400,
						    base->debugfs.dir, fte,
						    &fs_fte_flow_tag);
	if (!fte->debugfs.flow_tag)
		return -ENOMEM;

	fte->debugfs.dests_size = debugfs_create_file("dests_size", 0400,
						      base->debugfs.dir, fte,
						      &fs_fte_dests_size);
	if (!fte->debugfs.dests_size)
		return -ENOMEM;

	if (fs_debugfs_add_val(base))
		return -ENOMEM;

	fte->debugfs.dests = debugfs_create_dir("destinations",
						base->debugfs.dir);
	if (!fte->debugfs.dests)
		return -ENOMEM;

	return 0;
}

static int fs_debugfs_add_dst(struct fs_base *base)
{
	struct mlx5_flow_rule *dst;

	fs_get_obj(dst, base);

	dst->debugfs.type = debugfs_create_file("dest_type", 0400,
						base->debugfs.dir, dst,
						&fops_dst_type);
	if (!dst->debugfs.type)
		return -ENOMEM;

	if (dst->dest_attr.type ==
	    MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE) {
		char *path = kcalloc(1024, sizeof(char), GFP_KERNEL);

		if (!path)
			return -ENOMEM;

		/*We need the path to the pointed flow table,
		 *we need go back 7 directories:
		 *dest directory->destinations->entry->
		 *entries->group->groups->flow table->flow tables
		 */
		snprintf(path, 1024, "../../../../../../../%s",
			 dst->dest_attr.ft->base.debugfs.dir->d_name.name);
		dst->debugfs.ft = debugfs_create_symlink("flow_table",
							 base->debugfs.dir,
							 path);
		if (!dst->debugfs.ft) {
			kfree(path);
			return -ENOMEM;
		}
		kfree(path);
	} else if (dst->dest_attr.type ==
		   MLX5_FLOW_DESTINATION_TYPE_TIR) {
		dst->debugfs.tir = debugfs_create_file("tir", 0400,
							base->debugfs.dir, dst,
							&fs_dst_tir);
		if (!dst->debugfs.tir)
			return -ENOMEM;
	}
	return 0;
}
