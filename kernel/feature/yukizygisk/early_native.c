#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "api.h"
#include "internal.h"
#include "fixed_policy.h"
#include "ksu.h"
#include "klog.h" // IWYU pragma: keep
#include "uapi/yukizygisk.h"

static DEFINE_MUTEX(yz_early_native_lock);
static struct yz_early_native_entry
    yz_early_native_entries[YZ_NATIVE_TARGET_MAX];
static u32 yz_early_native_count;
static u16 yz_early_load_flags;
static bool yz_early_native_loaded;
static bool yz_early_native_enabled;
static bool yz_early_native_watchdog;
static u64 yz_early_dlopen_off;
static u64 yz_early_dlsym_off;
static u64 yz_early_dlopen32_off;
static u64 yz_early_dlsym32_off;
static unsigned long yz_early_native_retry_deadline;
static bool yz_early_native_missing_logged;

#define YZ_EARLY_NATIVE_RETRY_WINDOW (15 * HZ)

static struct file *yz_open_first(const char *primary, const char *fallback,
				  const char **chosen_path)
{
	struct file *file;
	const struct cred *old_cred;

	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;
	file = filp_open(primary, O_RDONLY, 0);
	if (IS_ERR(file)) {
		file = filp_open(fallback, O_RDONLY, 0);
		if (!IS_ERR(file) && chosen_path)
			*chosen_path = fallback;
	} else if (chosen_path) {
		*chosen_path = primary;
	}
	if (old_cred)
		revert_creds(old_cred);
	return file;
}

static bool yz_read_exact_file(struct file *file, void *buf, size_t size,
			       loff_t *pos)
{
	ssize_t got = kernel_read(file, buf, size, pos);

	return got == (ssize_t)size;
}

enum yz_file_size_check {
	YZ_FILE_SIZE_MATCH = 0,
	YZ_FILE_SIZE_MISMATCH,
	YZ_FILE_SIZE_UNAVAILABLE,
};

static enum yz_file_size_check
yz_check_file_size(const char *path, u64 want_size, u64 *actual, long *open_err)
{
	const struct cred *old_cred;
	struct file *file;
	u64 size;

	if (actual)
		*actual = 0;
	if (open_err)
		*open_err = 0;
	if (!want_size)
		return YZ_FILE_SIZE_MISMATCH;

	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;
	file = filp_open(path, O_RDONLY, 0);
	if (old_cred)
		revert_creds(old_cred);
	if (IS_ERR(file)) {
		if (open_err)
			*open_err = PTR_ERR(file);
		return YZ_FILE_SIZE_UNAVAILABLE;
	}

	size = i_size_read(file_inode(file));
	filp_close(file, NULL);
	if (actual)
		*actual = size;
	return size == want_size ? YZ_FILE_SIZE_MATCH : YZ_FILE_SIZE_MISMATCH;
}

static bool yz_early_entry_valid(struct yz_early_native_entry *entry)
{
	entry->module_id[YZ_NATIVE_MODULE_ID_MAX - 1] = '\0';
	entry->target[YZ_NATIVE_TARGET_VALUE_MAX - 1] = '\0';
	entry->lib_path[YZ_NATIVE_MODULE_PATH_MAX - 1] = '\0';
	if (entry->target_type != YZ_NATIVE_TARGET_NAME &&
	    entry->target_type != YZ_NATIVE_TARGET_PATH)
		return false;
	if (!(entry->flags &
	      (YZ_EARLY_NATIVE_ENTRY_ABI32 | YZ_EARLY_NATIVE_ENTRY_ABI64)))
		return false;
	if (entry->module_id[0] == '\0' || entry->target[0] == '\0' ||
	    entry->lib_path[0] == '\0')
		return false;
	return true;
}

static void yz_load_early_native_locked(void)
{
	struct yz_early_native_snapshot_header hdr;
	struct file *file;
	const char *path = NULL;
	loff_t pos = 0;
	enum yz_file_size_check linker_size_check = YZ_FILE_SIZE_MISMATCH;
	enum yz_file_size_check linker32_size_check = YZ_FILE_SIZE_MISMATCH;
	u64 linker_actual_size = 0;
	u64 linker32_actual_size = 0;
	long linker_open_err = 0;
	long linker32_open_err = 0;
	bool invalid;
	u32 i;

	if (yz_early_native_loaded)
		return;
	yz_early_native_enabled = false;
	yz_early_native_watchdog = false;
	yz_early_native_count = 0;
	yz_early_dlopen_off = 0;
	yz_early_dlsym_off = 0;
	yz_early_dlopen32_off = 0;
	yz_early_dlsym32_off = 0;

	file = yz_open_first(YZ_EARLY_MANIFEST_WATCHDOG,
			     YZ_EARLY_MANIFEST_DEFAULT, &path);
	if (IS_ERR(file)) {
		if (!yz_early_native_retry_deadline)
			yz_early_native_retry_deadline =
			    jiffies + YZ_EARLY_NATIVE_RETRY_WINDOW;
		if (time_after(jiffies, yz_early_native_retry_deadline)) {
			yz_early_native_loaded = true;
			if (!yz_early_native_missing_logged) {
				yz_early_native_missing_logged = true;
				pr_info(
				    "yukizygisk: early native snapshot "
				    "unavailable; disabled for this boot\n");
			}
		}
		return;
	}

	yz_early_native_loaded = true;

	if (!yz_read_exact_file(file, &hdr, sizeof(hdr), &pos)) {
		pr_info("yukizygisk: early native snapshot header read failed "
			"path=%s\n",
			path ?: "(unknown)");
		goto out;
	}
	invalid =
	    hdr.magic != YZ_EARLY_NATIVE_MAGIC ||
	    hdr.version != YZ_EARLY_NATIVE_VERSION ||
	    hdr.header_size != sizeof(hdr) ||
	    hdr.entry_size != sizeof(struct yz_early_native_entry) ||
	    hdr.count > YZ_NATIVE_TARGET_MAX ||
	    !(hdr.flags & YZ_EARLY_NATIVE_FLAG_ENABLED) ||
	    (hdr.count && !(hdr.flags & (YZ_EARLY_NATIVE_FLAG_ABI32 |
					 YZ_EARLY_NATIVE_FLAG_ABI64))) ||
	    ((hdr.flags & YZ_EARLY_NATIVE_FLAG_ABI64) &&
	     (!hdr.dlopen_offset || !hdr.dlsym_offset || !hdr.linker_size)) ||
	    ((hdr.flags & YZ_EARLY_NATIVE_FLAG_ABI32) &&
	     (!hdr.dlopen32_offset || !hdr.dlsym32_offset ||
	      !hdr.linker32_size));
	if (!invalid && (hdr.flags & YZ_EARLY_NATIVE_FLAG_ABI64)) {
		linker_size_check =
		    yz_check_file_size(YZ_SYSTEM_LINKER64, hdr.linker_size,
				       &linker_actual_size, &linker_open_err);
		invalid = linker_size_check == YZ_FILE_SIZE_MISMATCH;
	}
	if (!invalid && (hdr.flags & YZ_EARLY_NATIVE_FLAG_ABI32)) {
		linker32_size_check = yz_check_file_size(
		    YZ_SYSTEM_LINKER32, hdr.linker32_size,
		    &linker32_actual_size, &linker32_open_err);
		invalid = linker32_size_check == YZ_FILE_SIZE_MISMATCH;
	}
	if (invalid) {
		pr_info("yukizygisk: early native snapshot invalid path=%s "
			"magic=0x%x version=%u flags=0x%x count=%u "
			"linker=%llu/%llu compat_linker=%llu/%llu\n",
			path ?: "(unknown)", hdr.magic, hdr.version, hdr.flags,
			hdr.count, hdr.linker_size, linker_actual_size,
			hdr.linker32_size, linker32_actual_size);
		goto out;
	}
	if (linker_size_check == YZ_FILE_SIZE_UNAVAILABLE)
		pr_info("yukizygisk: linker size unavailable path=%s "
			"expected=%llu err=%ld; using snapshot\n",
			YZ_SYSTEM_LINKER64, hdr.linker_size, linker_open_err);
	if (linker32_size_check == YZ_FILE_SIZE_UNAVAILABLE)
		pr_info("yukizygisk: compat linker size unavailable path=%s "
			"expected=%llu err=%ld; using snapshot\n",
			YZ_SYSTEM_LINKER32, hdr.linker32_size,
			linker32_open_err);

	for (i = 0; i < hdr.count; i++) {
		struct yz_early_native_entry entry;

		if (!yz_read_exact_file(file, &entry, sizeof(entry), &pos))
			break;
		if (!yz_early_entry_valid(&entry))
			continue;
		yz_early_native_entries[yz_early_native_count++] = entry;
	}

	yz_early_dlopen_off = hdr.dlopen_offset;
	yz_early_load_flags = hdr.load_flags;
	yz_early_dlsym_off = hdr.dlsym_offset;
	yz_early_dlopen32_off = hdr.dlopen32_offset;
	yz_early_dlsym32_off = hdr.dlsym32_offset;
	yz_early_native_watchdog =
	    path && !strcmp(path, YZ_EARLY_MANIFEST_WATCHDOG);
	yz_early_native_enabled = yz_early_native_count > 0;
	if (yz_early_native_enabled)
		pr_info("yukizygisk: early native snapshot loaded path=%s "
			"count=%u dlopen=0x%llx dlsym=0x%llx dlopen32=0x%llx "
			"dlsym32=0x%llx\n",
			path ?: "(unknown)", yz_early_native_count,
			yz_early_dlopen_off, yz_early_dlsym_off,
			yz_early_dlopen32_off, yz_early_dlsym32_off);
out:
	filp_close(file, NULL);
}

bool yz_early_native_active(void)
{
	bool active;

	mutex_lock(&yz_early_native_lock);
	yz_load_early_native_locked();
	active = yz_early_native_enabled;
	mutex_unlock(&yz_early_native_lock);
	return active;
}

void yz_early_native_disable(void)
{
	mutex_lock(&yz_early_native_lock);
	yz_early_native_enabled = false;
	yz_early_native_loaded = true;
	yz_early_native_count = 0;
	mutex_unlock(&yz_early_native_lock);
}

bool yz_match_early_native_target(const char *filename, char *label,
				  size_t label_len, u8 *target_type)
{
	const char *base = yz_basename(filename);
	bool matched = false;
	u32 i;

	if (!filename || !base)
		return false;

	mutex_lock(&yz_early_native_lock);
	yz_load_early_native_locked();
	if (!yz_early_native_enabled)
		goto out;

	for (i = 0; i < yz_early_native_count; i++) {
		struct yz_early_native_entry *entry =
		    &yz_early_native_entries[i];

		if (entry->target_type == YZ_NATIVE_TARGET_NAME) {
			if (strcmp(base, entry->target))
				continue;
		} else if (entry->target_type == YZ_NATIVE_TARGET_PATH) {
			if (strcmp(filename, entry->target))
				continue;
		} else {
			continue;
		}

		if (!yz_dlopen_off)
			yz_dlopen_off = yz_early_dlopen_off;
		if (!yz_dlsym_off)
			yz_dlsym_off = yz_early_dlsym_off;
		if (!yz_dlopen32_off)
			yz_dlopen32_off = yz_early_dlopen32_off;
		if (!yz_dlsym32_off)
			yz_dlsym32_off = yz_early_dlsym32_off;
		yz_copy_name(label, label_len, entry->target);
		if (target_type)
			*target_type = entry->target_type;
		matched = true;
		break;
	}
out:
	mutex_unlock(&yz_early_native_lock);
	return matched;
}

const char *yz_early_loader_path(bool compat)
{
	if (compat)
		return yz_early_native_watchdog ? YZ_EARLY_LOADER32_WATCHDOG
						: YZ_EARLY_LOADER32_DEFAULT;
	return yz_early_native_watchdog ? YZ_EARLY_LOADER64_WATCHDOG
					: YZ_EARLY_LOADER64_DEFAULT;
}

const char *yz_early_native_core_path(bool compat)
{
	if (compat)
		return yz_early_native_watchdog
			   ? YZ_EARLY_NATIVE_CORE32_WATCHDOG
			   : YZ_EARLY_NATIVE_CORE32_DEFAULT;
	return yz_early_native_watchdog ? YZ_EARLY_NATIVE_CORE64_WATCHDOG
					: YZ_EARLY_NATIVE_CORE64_DEFAULT;
}

void yz_early_packet_state_init(struct yz_early_packet_state *state)
{
	u32 i;

	state->packet_fd = -1;
	state->endpoint_fd = -1;
	state->module_fd_count = 0;
	for (i = 0; i < YZ_NATIVE_TARGET_MAX; i++)
		state->module_fds[i] = -1;
}

void yz_close_early_packet_state(struct yz_early_packet_state *state)
{
	u32 i;

	if (!state)
		return;
	if (state->packet_fd >= 0) {
		yz_close_current_fd(state->packet_fd);
		state->packet_fd = -1;
	}
	if (state->endpoint_fd >= 0) {
		yz_close_current_fd(state->endpoint_fd);
		state->endpoint_fd = -1;
	}
	for (i = 0; i < state->module_fd_count; i++) {
		if (state->module_fds[i] >= 0)
			yz_close_current_fd(state->module_fds[i]);
		state->module_fds[i] = -1;
	}
	state->module_fd_count = 0;
}

static int yz_install_packet_fd(const void *buf, size_t size)
{
	const struct cred *old_cred;
	struct file *mfd;
	loff_t pos = 0;
	ssize_t w;
	int fd, ret;

	mfd = shmem_file_setup(YZ_VMA_NAME, size, 0);
	if (IS_ERR(mfd))
		return PTR_ERR(mfd);
	mfd->f_mode |= FMODE_PREAD | FMODE_PWRITE | FMODE_LSEEK;
	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;
	w = kernel_write(mfd, buf, size, &pos);
	if (old_cred)
		revert_creds(old_cred);
	if (w != (ssize_t)size) {
		fput(mfd);
		return w < 0 ? (int)w : -EIO;
	}

	ret = yz_fixed_policy_label(mfd, NULL);
	if (ret) {
		fput(mfd);
		return ret;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(mfd);
		return fd;
	}
	fd_install(fd, mfd);
	return fd;
}

int yz_stage_bootstrap_packet(u8 target_type, const char *target, bool compat,
			      struct yz_early_packet_state *state)
{
	struct yz_early_native_entry *matches;
	struct yz_bootstrap_packet_header *hdr;
	struct yz_early_native_packet_entry *entries;
	void *packet;
	size_t packet_size;
	u32 match_count = 0;
	u16 load_flags = 0;
	u32 i;
	int ret = 0;

	if (!state)
		return -EINVAL;

	matches = kcalloc(YZ_NATIVE_TARGET_MAX, sizeof(*matches), GFP_KERNEL);
	if (!matches)
		return -ENOMEM;

	if (target) {
		mutex_lock(&yz_early_native_lock);
		yz_load_early_native_locked();
		load_flags = yz_early_load_flags;
		for (i = 0;
		     yz_early_native_enabled && i < yz_early_native_count;
		     i++) {
			struct yz_early_native_entry *entry =
			    &yz_early_native_entries[i];

			if (entry->target_type != target_type)
				continue;
			if (strcmp(entry->target, target))
				continue;
			if (compat &&
			    !(entry->flags & YZ_EARLY_NATIVE_ENTRY_ABI32))
				continue;
			if (!compat &&
			    !(entry->flags & YZ_EARLY_NATIVE_ENTRY_ABI64))
				continue;
			matches[match_count++] = *entry;
		}
		mutex_unlock(&yz_early_native_lock);
	}

	packet_size = sizeof(*hdr) + match_count * sizeof(*entries);
	packet = kvzalloc(packet_size, GFP_KERNEL);
	if (!packet) {
		ret = -ENOMEM;
		goto out_free_matches;
	}

	hdr = packet;
	entries = (struct yz_early_native_packet_entry *)(hdr + 1);
	hdr->magic = YZ_BOOTSTRAP_PACKET_MAGIC;
	hdr->version = YZ_BOOTSTRAP_PACKET_VERSION;
	hdr->header_size = sizeof(*hdr);
	hdr->entry_size = sizeof(*entries);
	hdr->load_flags = load_flags;

	for (i = 0; i < match_count; i++) {
		int fd = yz_stage_fd(matches[i].lib_path, YZ_VMA_NAME, NULL);

		if (fd < 0) {
			pr_info("yukizygisk: early native module staging "
				"failed path=%s err=%d\n",
				matches[i].lib_path, fd);
			continue;
		}
		state->module_fds[state->module_fd_count++] = fd;
		entries[hdr->count].module = matches[i];
		entries[hdr->count].fd = fd;
		hdr->count++;
	}
	state->endpoint_fd = yz_endpoint_install_fd(compat ? YZ_RUNTIME_ABI_32
							   : YZ_RUNTIME_ABI_64);
	if (state->endpoint_fd < 0) {
		ret = state->endpoint_fd;
		kvfree(packet);
		yz_close_early_packet_state(state);
		goto out_free_matches;
	}
	hdr->endpoint_fd = state->endpoint_fd;

	packet_size = sizeof(*hdr) + hdr->count * sizeof(*entries);
	match_count = hdr->count;
	ret = yz_install_packet_fd(packet, packet_size);
	kvfree(packet);
	if (ret < 0) {
		yz_close_early_packet_state(state);
		goto out_free_matches;
	}
	state->packet_fd = ret;
	pr_info("yukizygisk: early native packet staged target=%s modules=%u "
		"fd=%d\n",
		target ?: "zygote", match_count, state->packet_fd);
	ret = 0;

out_free_matches:
	kfree(matches);
	return ret;
}
