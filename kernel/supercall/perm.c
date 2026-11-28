#include <linux/cred.h>

#include "manager/manager_identity.h"
#include "policy/allowlist.h"
#include "selinux/avc_compat.h"
#include "objsec.h"
#include "supercall/internal.h"

bool only_manager(void)
{
	return is_manager();
}

bool only_root(void)
{
	return current_uid().val == 0;
}

bool require_load_policy_perm(void)
{
	return ksu_avc_has_perm_compat(current_sid(), SECINITSID_SECURITY, SECCLASS_SECURITY, SECURITY__LOAD_POLICY, NULL) == 0;
}

bool manager_or_root(void)
{
	return current_uid().val == 0 || is_manager();
}

bool always_allow(void)
{
	return true; // No permission check
}

bool allowed_for_su(void)
{
	return is_manager() || ksu_is_allow_uid_for_current(current_uid().val);
}

/* Gate for KSU_IOCTL_YZ_UNMAP_SELF: only an app process may ask the kernel to
 * unmap-self. The op acts on current's own address space only, and the handler
 * re-validates the segments, so an app uid gate is sufficient -- it keeps
 * root/system/manager callers (which never need self-unmap) out. */
bool injected_app(void)
{
	return is_appuid(current_uid().val);
}
