#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/ratelimit.h>
#include <linux/uaccess.h>

#include "objsec.h"
#include "selinux/selinux.h"
#include "infra/btf_parser.h"
#include "klog.h"

#if defined(MODULE) && defined(CONFIG_DEBUG_INFO_BTF)
#include <uapi/linux/btf.h>

static u32 inode_blob_offset __ro_after_init;

int __init ksu_selinux_inode_init(void)
{
	struct ksu_btf_field blob_field, inode_field, sid_field;
	s32 blob_offset;
	int ret;

	ret = ksu_btf_field("lsm_blob_sizes", "lbs_inode", BTF_KIND_INT,
			    sizeof(int), &blob_field);
	if (ret)
		goto fail;
	ret = ksu_btf_field("inode_security_struct", "inode", BTF_KIND_PTR,
			    sizeof(void *), &inode_field);
	if (ret)
		goto fail;
	ret = ksu_btf_field("inode_security_struct", "sid", BTF_KIND_INT,
			    sizeof(u32), &sid_field);
	if (ret)
		goto fail;
	if (inode_field.offset !=
		offsetof(struct inode_security_struct, inode) ||
	    sid_field.offset != offsetof(struct inode_security_struct, sid)) {
		ret = -EINVAL;
		goto fail;
	}
	ret = copy_from_kernel_nofault(
	    &blob_offset, (const char *)&selinux_blob_sizes + blob_field.offset,
	    sizeof(blob_offset));
	if (ret) {
		pr_err("selinux_blob_sizes.lbs_inode unreadable (%d)\n", ret);
		goto fail;
	}
	if (blob_offset < 0 ||
	    blob_offset % __alignof__(struct inode_security_struct)) {
		ret = -EINVAL;
		goto fail;
	}
	inode_blob_offset = blob_offset;
	pr_info("offset from vmlinux.btf, lbs_inode@%u, blob=%u\n",
		blob_field.offset, inode_blob_offset);
	return 0;
fail:
	pr_err("vmlinux BTF (%d) unreadable!\n", ret);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
static u32 superblock_blob_offset;

int ksu_selinux_superblock_init(void)
{
	struct ksu_btf_field field;
	s32 offset;
	int ret;

	ret = ksu_btf_field("lsm_blob_sizes", "lbs_superblock", BTF_KIND_INT,
			    sizeof(int), &field);
	if (ret)
		return ret;
	ret = copy_from_kernel_nofault(
	    &offset, (const char *)&selinux_blob_sizes + field.offset,
	    sizeof(offset));
	if (ret)
		return ret;
	if (offset < 0 ||
	    offset % __alignof__(struct superblock_security_struct))
		return -EINVAL;
	superblock_blob_offset = offset;
	return 0;
}
#endif
#endif

#ifdef CONFIG_KSU_YUKIZYGISK
#if defined(MODULE) && defined(CONFIG_DEBUG_INFO_BTF)
#include <linux/bpf.h>
#include <linux/btf.h>
#define YZ_BTF_CALL
#include "feature/yukizygisk/policy/inode_layout.h"
#undef YZ_BTF_CALL
#endif

int ksu_fixed_inode_layout(void)
{
#if defined(MODULE) && defined(CONFIG_DEBUG_INFO_BTF)
	const struct btf *btf;
	s32 id;

	btf = bpf_get_btf_vmlinux();
	if (IS_ERR_OR_NULL(btf))
		return -EOPNOTSUPP;
	id = btf_find_by_name_kind(btf, "inode_security_struct",
				   BTF_KIND_STRUCT);
	if (id < 0 ||
	    !yz_fixed_inode_btf(btf, btf_type_by_id(btf, id),
				btf_name_by_offset, btf_type_skip_modifiers))
		return -EOPNOTSUPP;
	return 0;
#elif defined(MODULE)
	return -EOPNOTSUPP;
#else
	/* Built-in code uses the running kernel's own SELinux layout. */
	return 0;
#endif
}
#endif

struct inode_security_struct *ksu_selinux_inode(const struct inode *inode)
{
	struct inode_security_struct *security;
	struct inode *owner;
	int ret;

	if (!inode || !inode->i_security)
		return NULL;
#if defined(MODULE) && defined(CONFIG_DEBUG_INFO_BTF)
	security = (void *)((char *)inode->i_security + inode_blob_offset);
#else
	security = selinux_inode(inode);
#endif
	// Validate the back pointer before callers read or overwrite the SID.
	ret = copy_from_kernel_nofault(&owner, &security->inode, sizeof(owner));
	if (ret || owner != inode) {
		pr_err_ratelimited(
		    "SELinux inode security layout mismatch (%d)\n", ret);
		return ERR_PTR(ret ? ret : -EINVAL);
	}
	return security;
}

struct superblock_security_struct *
ksu_selinux_superblock(const struct super_block *sb)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0)
	return sb->s_security;
#elif defined(MODULE) && defined(CONFIG_DEBUG_INFO_BTF)
	return (void *)((char *)sb->s_security + superblock_blob_offset);
#else
	return selinux_superblock(sb);
#endif
}
