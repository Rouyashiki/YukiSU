#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>

#include "policy/allowlist.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/apk_sign.h"
#include "manager/dynamic_manager.h"
#include "manager/manager_identity.h"
#include "manager/throne_tracker.h"
#include "ksu.h"
#include "util.h"

uid_t ksu_manager_uid = KSU_INVALID_UID;
uid_t ksu_manager_appid = KSU_INVALID_UID;
uid_t ksu_manager_appids[KSU_MAX_MANAGER_KEYS] = {KSU_INVALID_UID};

static uid_t locked_manager_appids[KSU_MAX_MANAGER_KEYS] = {KSU_INVALID_UID};

static void update_primary_manager(void)
{
	int i;
	ksu_manager_appid = KSU_INVALID_UID;
	ksu_manager_uid = KSU_INVALID_UID;
	for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
		if (ksu_manager_appids[i] != KSU_INVALID_UID) {
			ksu_manager_appid = ksu_manager_appids[i];
			/* full uid: assume user 0 if not in process context */
			ksu_manager_uid = ksu_manager_appids[i];
			break;
		}
	}
}

void ksu_set_manager_appid_for_index(uid_t appid, int signature_index)
{
	int i;

	if (signature_index < 0 || signature_index >= KSU_MAX_MANAGER_KEYS)
		return;

	/*
	 * Keep multi-manager coexistence stable:
	 * - If this appid is already present in any slot, reuse that slot.
	 * - If target signature slot is occupied by another app, place this app
	 *   into an empty slot instead of overwriting.
	 */
	for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
		if (ksu_manager_appids[i] == appid) {
			ksu_manager_appids[i] = appid;
			locked_manager_appids[i] = appid;
			update_primary_manager();
			return;
		}
	}

	if (ksu_manager_appids[signature_index] == KSU_INVALID_UID ||
	    ksu_manager_appids[signature_index] == appid) {
		ksu_manager_appids[signature_index] = appid;
		locked_manager_appids[signature_index] = appid;
		update_primary_manager();
		return;
	}

	for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
		if (ksu_manager_appids[i] == KSU_INVALID_UID) {
			ksu_manager_appids[i] = appid;
			locked_manager_appids[i] = appid;
			update_primary_manager();
			return;
		}
	}

	/* No empty slot left, keep historical behavior. */
	ksu_manager_appids[signature_index] = appid;
	locked_manager_appids[signature_index] = appid;
	update_primary_manager();
}

#define KSU_UID_LIST_PATH "/data/misc/user_uid/uid_list"
#define SYSTEM_PACKAGES_LIST_PATH "/data/system/packages.list"

struct uid_data {
	struct list_head list;
	u32 appid;
	char package[KSU_MAX_PACKAGE_NAME];
};

// Try read /data/misc/user_uid/uid_list
static int uid_from_um_list(struct list_head *uid_list)
{
	struct file *fp;
	char *buf = NULL;
	loff_t size, pos = 0;
	ssize_t nr;
	char *line = NULL;
	char *next = NULL;
	int cnt = 0;

	fp = filp_open(KSU_UID_LIST_PATH, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		return -ENOENT;
	}

	size = i_size_read(file_inode(fp));
	if (size <= 0) {
		filp_close(fp, NULL);
		return -ENODATA;
	}

	buf = kzalloc(size + 1, GFP_ATOMIC);
	if (!buf) {
		pr_err("uid_list: OOM %lld B\n", size);
		filp_close(fp, NULL);
		return -ENOMEM;
	}

	nr = kernel_read(fp, buf, size, &pos);
	filp_close(fp, NULL);
	if (nr != size) {
		pr_err("uid_list: short read %zd/%lld\n", nr, size);
		kfree(buf);
		return -EIO;
	}
	buf[size] = '\0';

	for (line = buf; line; line = next) {
		char *uid_str = NULL;
		char *pkg = NULL;
		u32 uid;
		struct uid_data *d = NULL;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';

		while (*line == ' ' || *line == '\t' || *line == '\r')
			++line;
		if (!*line)
			continue;

		uid_str = strsep(&line, " \t");
		pkg = line;
		if (!pkg)
			continue;
		while (*pkg == ' ' || *pkg == '\t')
			++pkg;
		if (!*pkg)
			continue;

		if (kstrtou32(uid_str, 10, &uid)) {
			pr_warn_once("uid_list: bad uid <%s>\n", uid_str);
			continue;
		}

		d = kzalloc(sizeof(*d), GFP_ATOMIC);
		if (unlikely(!d)) {
			pr_err("uid_list: OOM appid=%u\n", uid);
			continue;
		}

		d->appid = uid;
		strscpy(d->package, pkg, KSU_MAX_PACKAGE_NAME);
		list_add_tail(&d->list, uid_list);
		++cnt;
		// Log first few entries for debug
		if (cnt <= 5) {
		}
	}

	kfree(buf);
	pr_info("uid_list: loaded %d entries\n", cnt);
	return cnt > 0 ? 0 : -ENODATA;
}

static int get_pkg_from_apk_path(char *pkg, const char *path)
{
	int len = strlen(path);
	if (len >= KSU_MAX_PACKAGE_NAME || len < 1)
		return -1;

	const char *last_slash = NULL;
	const char *second_last_slash = NULL;

	int i;
	for (i = len - 1; i >= 0; i--) {
		if (path[i] == '/') {
			if (!last_slash) {
				last_slash = &path[i];
			} else {
				second_last_slash = &path[i];
				break;
			}
		}
	}

	if (!last_slash || !second_last_slash)
		return -1;

	const char *last_hyphen = strchr(second_last_slash, '-');
	if (!last_hyphen || last_hyphen > last_slash)
		return -1;

	int pkg_len = last_hyphen - second_last_slash - 1;
	if (pkg_len >= KSU_MAX_PACKAGE_NAME || pkg_len <= 0)
		return -1;

	// Copying the package name
	memcpy(pkg, second_last_slash + 1, pkg_len);
	pkg[pkg_len] = '\0';

	return 0;
}

static void crown_manager(const char *apk, struct list_head *uid_data,
			  int signature_index)
{
	char pkg[KSU_MAX_PACKAGE_NAME];
	if (get_pkg_from_apk_path(pkg, apk) < 0) {
		pr_err("Failed to get package name from apk path: %s\n", apk);
		return;
	}

	pr_info("manager pkg: %s, signature_index: %d\n", pkg, signature_index);

#ifdef KSU_MANAGER_PACKAGE
	// pkg is `/<real package>`
	if (strncmp(pkg, KSU_MANAGER_PACKAGE, sizeof(KSU_MANAGER_PACKAGE))) {
		pr_info("manager package is inconsistent with kernel build: %s "
			"vs %s\n",
			pkg, KSU_MANAGER_PACKAGE);
		return;
	}
#endif // #ifdef KSU_MANAGER_PACKAGE
	struct list_head *list = (struct list_head *)uid_data;
	struct uid_data *np;

	list_for_each_entry (np, list, list) {
		if (strncmp(np->package, pkg, KSU_MAX_PACKAGE_NAME) == 0) {
			pr_info("Crowning manager: %s (appid=%d) "
				"signature_index=%d\n",
				pkg, np->appid, signature_index);

			if (signature_index >= 0 &&
			    signature_index < KSU_MAX_MANAGER_KEYS) {
				ksu_set_manager_appid_for_index(
				    np->appid, signature_index);
			} else {
				ksu_set_manager_appid(np->appid);
			}
			break;
		}
	}
}

static void note_scanned_manager(const char *apk, struct list_head *uid_data,
				 const struct apk_sign_match *match)
{
	char pkg[KSU_MAX_PACKAGE_NAME];
	struct list_head *list = (struct list_head *)uid_data;
	struct uid_data *np;

	if (get_pkg_from_apk_path(pkg, apk) < 0) {
		pr_err("Failed to get package name from apk path: %s\n", apk);
		return;
	}

	list_for_each_entry (np, list, list) {
		if (strncmp(np->package, pkg, KSU_MAX_PACKAGE_NAME) == 0) {
			pr_info("Noting dynamic manager candidate: %s "
				"(appid=%d) "
				"signature=%s\n",
				pkg, np->appid,
				match && match->name ? match->name : "unknown");
			ksu_dynamic_manager_note_scanned(np->appid, match);
			break;
		}
	}
}

#define DATA_PATH_LEN 384 // 384 is enough for /data/app/<package>/base.apk

struct data_path {
	char dirpath[DATA_PATH_LEN];
	int depth;
	struct list_head list;
};

static bool manager_scan_forced;

struct my_dir_context {
	struct dir_context ctx;
	struct list_head *data_path_list;
	char *parent_dir;
	char *candidate_path;
	int depth;
};
// https://docs.kernel.org/filesystems/porting.html
// filldir_t (readdir callbacks) calling conventions have changed. Instead of
// returning 0 or -E... it returns bool now. false means "no more" (as -E...
// used to) and true - "keep going" (as 0 in old calling conventions).
// Rationale: callers never looked at specific -E... values anyway. ->
// iterate_shared() instances require no changes at all, all filldir_t ones in
// the tree converted.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define FILLDIR_RETURN_TYPE bool
#define FILLDIR_ACTOR_CONTINUE true
#define FILLDIR_ACTOR_STOP false
#else
#define FILLDIR_RETURN_TYPE int
#define FILLDIR_ACTOR_CONTINUE 0
#define FILLDIR_ACTOR_STOP -EINVAL
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
static FILLDIR_RETURN_TYPE my_actor(struct dir_context *ctx, const char *name,
				    int namelen, loff_t off, u64 ino,
				    unsigned int d_type)
{
	struct my_dir_context *my_ctx =
	    container_of(ctx, struct my_dir_context, ctx);
	char dirpath[DATA_PATH_LEN];

	if (!strncmp(name, "..", namelen) || !strncmp(name, ".", namelen))
		return FILLDIR_ACTOR_CONTINUE; // Skip "." and ".."

	if (d_type == DT_DIR && namelen >= 8 && !strncmp(name, "vmdl", 4) &&
	    !strncmp(name + namelen - 4, ".tmp", 4)) {
		pr_info("Skipping directory: %.*s\n", namelen, name);
		return FILLDIR_ACTOR_CONTINUE; // Skip staging package
	}

	if (snprintf(dirpath, DATA_PATH_LEN, "%s/%.*s", my_ctx->parent_dir,
		     namelen, name) >= DATA_PATH_LEN) {
		pr_err("Path too long: %s/%.*s\n", my_ctx->parent_dir, namelen,
		       name);
		return FILLDIR_ACTOR_CONTINUE;
	}

	if (d_type == DT_DIR && my_ctx->depth > 0) {
		struct data_path *data =
		    kzalloc(sizeof(struct data_path), GFP_ATOMIC);

		if (!data) {
			pr_err("Failed to allocate memory for %s\n", dirpath);
			return FILLDIR_ACTOR_CONTINUE;
		}

		strscpy(data->dirpath, dirpath, DATA_PATH_LEN);
		data->depth = my_ctx->depth - 1;
		list_add_tail(&data->list, my_ctx->data_path_list);
	} else if (d_type == DT_REG && namelen == 8 &&
		   !memcmp(name, "base.apk", 8)) {
		strscpy(my_ctx->candidate_path, dirpath, DATA_PATH_LEN);
	}

	return FILLDIR_ACTOR_CONTINUE;
}

static void search_manager(const char *path, int depth,
			   struct list_head *uid_data)
{
	int i;
	unsigned long data_app_magic = 0;
	LIST_HEAD(data_path_list);
	struct data_path data;
	char candidate_path[DATA_PATH_LEN];

	strscpy(data.dirpath, path, DATA_PATH_LEN);
	data.depth = depth;
	list_add_tail(&data.list, &data_path_list);

	for (i = depth; i >= 0; i--) {
		struct data_path *pos, *n;

		list_for_each_entry_safe (pos, n, &data_path_list, list) {
			struct my_dir_context ctx = {
			    .ctx.actor = my_actor,
			    .data_path_list = &data_path_list,
			    .parent_dir = pos->dirpath,
			    .candidate_path = candidate_path,
			    .depth = pos->depth,
			};
			struct apk_sign_match sign_match = {.index = -1};
			struct file *file;

			candidate_path[0] = '\0';
			file = ksu_filp_open_nonotify(
			    pos->dirpath,
			    O_RDONLY | O_NOFOLLOW | O_NOATIME | O_DIRECTORY);
			if (IS_ERR(file)) {
				pr_err(
				    "Failed to open directory: %s, err: %ld\n",
				    pos->dirpath, PTR_ERR(file));
				goto skip_iterate;
			}
			if (!data_app_magic)
				data_app_magic = file->f_inode->i_sb->s_magic;
			if (!data_app_magic ||
			    file->f_inode->i_sb->s_magic != data_app_magic) {
				filp_close(file, NULL);
				goto skip_iterate;
			}

			iterate_dir(file, &ctx.ctx);
			filp_close(file, NULL);

			// APK opens must run after iterate_dir releases the
			// directory lock.
			if (candidate_path[0] &&
			    match_apk_signature(candidate_path, &sign_match)) {
				if (sign_match.trusted && sign_match.index >= 0)
					crown_manager(candidate_path, uid_data,
						      sign_match.index);
				else
					note_scanned_manager(candidate_path,
							     uid_data,
							     &sign_match);
			}

		skip_iterate:
			list_del(&pos->list);
			if (pos != &data)
				kfree(pos);
		}
	}
}

static bool is_uid_exist(uid_t uid, char *package, void *data)
{
	struct list_head *list = (struct list_head *)data;
	struct uid_data *np;
	u32 appid = uid % 100000;
	bool exist = false;

	list_for_each_entry (np, list, list) {
		if (np->appid == appid &&
		    strncmp(np->package, package, KSU_MAX_PACKAGE_NAME) == 0) {
			exist = true;
			break;
		}
	}
	return exist;
}

void track_throne(bool prune_only)
{
	const struct cred *old_cred = override_creds(ksu_cred);
	struct list_head uid_list;
	struct uid_data *np, *n;
	struct file *fp;
	loff_t pos = 0;
	loff_t size;
	ssize_t nr;
	char *buf = NULL;
	char *line = NULL;
	char *next = NULL;
	static bool manager_exist = false;
	bool need_search = false;

	// init uid list head
	INIT_LIST_HEAD(&uid_list);

	fp = filp_open(SYSTEM_PACKAGES_LIST_PATH, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		pr_err("%s: open " SYSTEM_PACKAGES_LIST_PATH " failed: %ld\n",
		       __func__, PTR_ERR(fp));
		goto out;
	}

	size = i_size_read(file_inode(fp));
	if (size <= 0) {
		filp_close(fp, 0);
		goto out;
	}

	buf = kzalloc(size + 1, GFP_KERNEL);
	if (!buf) {
		filp_close(fp, 0);
		goto out;
	}

	nr = kernel_read(fp, buf, size, &pos);
	filp_close(fp, 0);
	if (nr <= 0) {
		pr_err("track_throne: read packages.list failed: %zd\n", nr);
		goto out;
	}
	buf[nr] = '\0';

	for (line = buf; line && *line; line = next) {
		struct uid_data *data = NULL;
		const char *delim = " \t";
		char *package = NULL;
		char *tmp = NULL;
		char *uid = NULL;
		u32 res;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		while (*line == ' ' || *line == '\t' || *line == '\r')
			line++;
		if (!*line)
			continue;

		tmp = line;
		package = strsep(&tmp, delim);
		uid = strsep(&tmp, delim);
		if (!uid || !package) {
			pr_err("update_uid: package or uid is NULL!\n");
			continue;
		}

		if (kstrtou32(uid, 10, &res)) {
			pr_err("track_throne: appid parse err\n");
			continue;
		}

		data = kzalloc(sizeof(*data), GFP_KERNEL);
		if (!data) {
			pr_err("track_throne: OOM appid=%u\n", res);
			continue;
		}
		data->appid = res;
		strscpy(data->package, package, KSU_MAX_PACKAGE_NAME);
		list_add_tail(&data->list, &uid_list);
	}
	kfree(buf);
	buf = NULL;

	if (prune_only)
		goto prune;

	/* For each manager slot, clear it if that appid is no longer in
	 * packages.list */
	{
		int i;
		for (i = 0; i < KSU_MAX_MANAGER_KEYS; i++) {
			uid_t aid = ksu_manager_appids[i];
			bool slot_still_exists = false;

			if (aid == KSU_INVALID_UID)
				continue;
			list_for_each_entry (np, &uid_list, list) {
				if (np->appid == aid) {
					slot_still_exists = true;
					break;
				}
			}
			if (!slot_still_exists) {
				pr_info("Manager slot %d (appid=%d) removed, "
					"clearing\n",
					i, aid);
				ksu_manager_appids[i] = KSU_INVALID_UID;
				locked_manager_appids[i] = KSU_INVALID_UID;
			}
		}
		update_primary_manager();
		manager_exist = (ksu_manager_appid != KSU_INVALID_UID);
		if (!manager_exist) {
#ifdef CONFIG_KSU_SUPERKEY
			extern void ksu_superkey_register_prctl_hook(void);
			ksu_superkey_register_prctl_hook();
#endif // #ifdef CONFIG_KSU_SUPERKEY
		}
	}

	need_search = !manager_exist || manager_scan_forced;
	manager_scan_forced = false;

	if (need_search) {
		pr_info("Searching for manager(s)...\n");
		search_manager("/data/app", 2, &uid_list);
		pr_info("Manager search finished\n");
	}

prune:
	// then prune the allowlist
	ksu_prune_allowlist(is_uid_exist, &uid_list);
out:
	kfree(buf);
	// free uid_list
	list_for_each_entry_safe (np, n, &uid_list, list) {
		list_del(&np->list);
		kfree(np);
	}
	revert_creds(old_cred);
}

/*
 * LKM: Delayed manager search when loaded after boot (packages.list may
 * already exist and won't trigger fsnotify). Schedule a delayed search.
 */
static struct delayed_work throne_search_work;

static void do_throne_search(struct work_struct *work)
{
	pr_info("throne_tracker: delayed search for manager...\n");
	track_throne(false);
}

void ksu_request_manager_rescan(void)
{
	manager_scan_forced = true;
	track_throne(false);
}

void ksu_throne_tracker_init()
{
	INIT_DELAYED_WORK(&throne_search_work, do_throne_search);
	schedule_delayed_work(&throne_search_work, msecs_to_jiffies(3000));
	pr_info("throne_tracker: init, scheduled manager search in 3s\n");
}

void ksu_throne_tracker_exit(void)
{
	cancel_delayed_work_sync(&throne_search_work);
	pr_info("throne_tracker: exit\n");
}
