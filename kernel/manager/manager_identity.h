#ifndef __KSU_H_MANAGER_IDENTITY
#define __KSU_H_MANAGER_IDENTITY

#include <linux/cred.h>
#include <linux/types.h>

#ifndef PER_USER_RANGE
#define PER_USER_RANGE 100000
#endif // #ifndef PER_USER_RANGE

#define KSU_INVALID_UID -1

#ifdef CONFIG_KSU_DISABLE_MANAGER
static inline bool ksu_is_manager_uid_valid(void)
{
	return true;
}

static inline bool ksu_is_manager_appid_valid(void)
{
	return true;
}

static inline bool is_manager(void)
{
	return current_uid().val == 0;
}

static inline uid_t ksu_get_manager_appid(void)
{
	return 0;
}

static inline uid_t ksu_get_manager_uid(void)
{
	return 0;
}

static inline bool ksu_is_uid_manager(uid_t uid)
{
	return uid == 0;
}

static inline bool ksu_is_uid_preset_manager(uid_t uid)
{
	(void)uid;
	return false;
}

static inline bool ksu_is_current_dynamic_manager(void)
{
	return false;
}

static inline void ksu_set_manager_uid(uid_t uid)
{
	(void)uid;
}

static inline void ksu_set_manager_appid(uid_t appid)
{
	(void)appid;
}

static inline void ksu_set_manager_appid_for_index(uid_t appid,
						   int signature_index)
{
	(void)appid;
	(void)signature_index;
}

static inline void ksu_invalidate_manager_uid(void)
{
}

static inline void ksu_invalidate_manager_appid(void)
{
}
#else

/* Static trusted signatures occupy the first slots in apk_sign_keys. */
#define KSU_MAX_MANAGER_KEYS 2

extern uid_t
    ksu_manager_uid; // primary full uid (first valid, for backward compat)
extern uid_t ksu_manager_appid; // primary appid (first valid)
extern uid_t ksu_manager_appids[KSU_MAX_MANAGER_KEYS]; // per signature_index

bool ksu_is_dynamic_manager_uid(uid_t uid);
bool ksu_is_preset_manager_uid(uid_t uid);
bool ksu_has_dynamic_manager(void);

#ifdef CONFIG_KSU_SUPERKEY
#include "manager/superkey.h"
#endif // #ifdef CONFIG_KSU_SUPERKEY

static inline bool ksu_is_manager_uid_valid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	return superkey_get_manager_uid() != (uid_t)-1 ||
	       ksu_manager_uid != KSU_INVALID_UID || ksu_has_dynamic_manager();
#else
	return ksu_manager_uid != KSU_INVALID_UID || ksu_has_dynamic_manager();
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

static inline bool ksu_is_manager_appid_valid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	return superkey_get_manager_uid() % PER_USER_RANGE != (uid_t)-1 ||
	       ksu_manager_appid != KSU_INVALID_UID;
#else
	return ksu_manager_appid != KSU_INVALID_UID;
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

static inline bool is_manager(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	if (superkey_is_manager())
		return true;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	{
		uid_t appid = current_uid().val % PER_USER_RANGE;
		int i;
		for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
			if (ksu_manager_appids[i] != KSU_INVALID_UID &&
			    ksu_manager_appids[i] == appid)
				return true;
		}
	}
	if (ksu_is_dynamic_manager_uid(current_uid().val))
		return true;
	return false;
}

static inline uid_t ksu_get_manager_appid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	uid_t superkey_uid = superkey_get_manager_uid();
	if (superkey_uid != (uid_t)-1)
		return superkey_uid % PER_USER_RANGE;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	if (is_manager())
		return current_uid().val % PER_USER_RANGE;
	return ksu_manager_appid;
}

static inline uid_t ksu_get_manager_uid(void)
{
#ifdef CONFIG_KSU_SUPERKEY
	uid_t superkey_uid = superkey_get_manager_uid();
	if (superkey_uid != (uid_t)-1)
		return superkey_uid;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	if (is_manager())
		return current_uid().val;
	return ksu_manager_uid;
}

static inline bool ksu_is_uid_manager(uid_t uid)
{
	uid_t appid = uid % PER_USER_RANGE;
	int i;

#ifdef CONFIG_KSU_SUPERKEY
	if (superkey_get_manager_uid() != (uid_t)-1 &&
	    superkey_get_manager_uid() % PER_USER_RANGE == appid)
		return true;
#endif // #ifdef CONFIG_KSU_SUPERKEY
	for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
		if (ksu_manager_appids[i] != KSU_INVALID_UID &&
		    ksu_manager_appids[i] == appid)
			return true;
	}
	if (ksu_is_dynamic_manager_uid(uid))
		return true;
	return false;
}

static inline bool ksu_is_uid_preset_manager(uid_t uid)
{
	return ksu_is_preset_manager_uid(uid);
}

static inline bool ksu_is_current_dynamic_manager(void)
{
	return ksu_is_dynamic_manager_uid(current_uid().val);
}

static inline void ksu_set_manager_uid(uid_t uid)
{
	ksu_manager_uid = uid;
}

static inline void ksu_set_manager_appid(uid_t appid)
{
	ksu_manager_appid = appid;
	ksu_manager_uid =
	    current_uid().val / PER_USER_RANGE * PER_USER_RANGE + appid;
	ksu_manager_appids[0] = appid;
}

void ksu_set_manager_appid_for_index(uid_t appid, int signature_index);

static inline void ksu_invalidate_manager_uid(void)
{
	ksu_manager_uid = KSU_INVALID_UID;
#ifdef CONFIG_KSU_SUPERKEY
	superkey_invalidate();
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

static inline void ksu_invalidate_manager_appid(void)
{
	ksu_manager_appid = KSU_INVALID_UID;
	{
		int i;
		for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++)
			ksu_manager_appids[i] = KSU_INVALID_UID;
	}
#ifdef CONFIG_KSU_SUPERKEY
	superkey_invalidate();
#endif // #ifdef CONFIG_KSU_SUPERKEY
}

#endif // #ifdef CONFIG_KSU_DISABLE_MANAGER

#endif // #ifndef __KSU_H_MANAGER_IDENTITY
