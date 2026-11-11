"""Run production su ownership and refresh checks against modeled mount views."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def extract_function(source, signature):
    start = source.rindex(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


FIXTURE = r"""
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
struct inode_operations { int unused; };
struct inode {
    const struct inode_operations *i_op;
    unsigned long i_ino;
};
struct dentry {
    struct inode *d_inode;
    struct dentry *lower;
};
struct path {
    struct dentry *dentry;
    const void *mnt;
};
struct file { struct path f_path; };
struct fd { struct file *file; };
struct pt_regs { unsigned long args[5]; };
struct cred { int unused; };
struct ksu_sulog_pending_event { int unused; };
struct test_uid { uid_t val; };
struct kasumi_dh_dir {
    struct inode *dir_inode;
    bool retiring;
};
struct kasumi_dh_iop { struct kasumi_dh_dir *dir; };
struct kasumi_dh_child {
    unsigned int flags;
    unsigned long v_ino;
};

#define KASUMI_NOCFI
#define KASUMI_VNODE_F_SU 16U
#define LOOKUP_FOLLOW 1U
#define LOOKUP_DIRECTORY 2U
#define MAY_WRITE 2U
#define READ_ONCE(value) (value)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define srcu_read_lock(lock) ((void)(lock), 0)
#define srcu_read_unlock(lock, index) ((void)(lock), (void)(index))
#define kfree(pointer) free(pointer)
#define __user
#define PT_REGS_SYSCALL_PARM1(regs) ((regs)->args[0])
#define PT_REGS_PARM2(regs) ((regs)->args[1])
#define PT_REGS_PARM3(regs) ((regs)->args[2])
#define PT_REGS_SYSCALL_PARM4(regs) ((regs)->args[3])
#define PT_REGS_PARM5(regs) ((regs)->args[4])
#define get_user(value, pointer) ((value) = *(pointer), 0)
#define fd_file(fd) ((fd).file)
#define unlikely(value) (value)
#define GFP_KERNEL 0
#define IS_ERR(pointer) ((intptr_t)(pointer) < 0)
#define PTR_ERR(pointer) ((long)(pointer))
#define KSUD_PATH "/data/adb/ksud"
#define __NR_execveat 0
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define pr_warn(...) ((void)0)

static const struct inode_operations su_inode_ops, real_inode_ops, overlay_inode_ops;
static int kasumi_dh_srcu;
static bool su_enabled = true;
static bool su_visible = true;
static unsigned long su_ino = 41;
static struct kasumi_dh_child rule = { KASUMI_VNODE_F_SU, 41 };
static struct kasumi_dh_iop parent_iop;
static struct path visible_parent, visible_leaf;
static const char *visible_name = "/system/bin/su";
static bool governed = true;
static int leaf_error;
static unsigned int leaf_lookups;
static unsigned int path_refs;
static bool caller_allowed = true;
static bool ksu_su_compat_enabled;
static bool installed_fd;
static unsigned int prompt_requests, opened_ksud, closed_fds;
static unsigned int root_escapes, redirected_execs, forwarded_execs;
static struct file retained_file, ksud_file;
static const struct cred root_cred;
static const struct cred *ksu_cred = &root_cred;
static struct ksu_sulog_pending_event log_event;
enum rebind_stage { NO_REBIND, DURING_RESOLVE, DURING_PROMPT, DURING_OPEN, DURING_CAPTURE };
static enum rebind_stage rebind_stage;

static void rebind_at(enum rebind_stage stage)
{
    if (rebind_stage == stage)
        ++su_ino;
}

static unsigned long untagged_addr(unsigned long address)
{
    return address;
}

static void path_get(struct path *path)
{
    (void)path;
    ++path_refs;
}

static struct fd fdget_raw(int fd)
{
    assert(fd == 11);
    rebind_at(DURING_RESOLVE);
    return (struct fd){ &retained_file };
}

static void fdput(struct fd fd)
{
    assert(fd.file == &retained_file);
}

static struct test_uid current_uid(void)
{
    return (struct test_uid){ 10000 };
}

static bool ksu_is_allow_uid_for_current(uid_t uid)
{
    assert(uid == 10000);
    return caller_allowed;
}

static bool ksu_sucompat_vfs_prompt_visible(void)
{
    return true;
}

static int ksu_sucompat_prompt_request(u32 *choice, u64 *generation)
{
    ++prompt_requests;
    *choice = 1;
    *generation = 7;
    rebind_at(DURING_PROMPT);
    return 0;
}

static bool prompt_grant_still_valid(uid_t uid, u32 choice, u64 generation)
{
    return uid == 10000 && choice == 1 && generation == 7;
}

static bool is_su_path(const char *path)
{
    return strcmp(path, "/system/bin/su") == 0;
}

static int get_unused_fd_flags(unsigned int flags)
{
    assert(flags == O_CLOEXEC && !installed_fd);
    return 71;
}

static const struct cred *override_creds(const struct cred *cred)
{
    assert(cred == ksu_cred);
    return cred;
}

static void revert_creds(const struct cred *cred)
{
    assert(cred == ksu_cred);
}

static struct file *filp_open(const char *name, int flags, int mode)
{
    assert(strcmp(name, KSUD_PATH) == 0 && flags == O_PATH && mode == 0);
    ++opened_ksud;
    rebind_at(DURING_OPEN);
    return &ksud_file;
}

static void put_unused_fd(int fd)
{
    assert(fd == 71 && !installed_fd);
}

static void fd_install(int fd, struct file *file)
{
    assert(fd == 71 && file == &ksud_file && !installed_fd);
    installed_fd = true;
}

static void close_tmp_fd(int fd)
{
    assert(fd == 71 && installed_fd);
    installed_fd = false;
    ++closed_fds;
}

static char *empty_user_path(void)
{
    static char empty[] = "";
    return empty;
}

static struct ksu_sulog_pending_event *ksu_sulog_capture_sucompat(
    const char *filename, const char *const *argv, int flags)
{
    (void)filename;
    (void)argv;
    (void)flags;
    rebind_at(DURING_CAPTURE);
    return &log_event;
}

static void ksu_sulog_emit_pending(struct ksu_sulog_pending_event *event, long result, int flags)
{
    (void)event;
    (void)result;
    (void)flags;
}

static int escape_with_root_profile(void)
{
    ++root_escapes;
    return 0;
}

static int ksu_install_su_fd(void)
{
    return 72;
}

static long redirected_exec(const struct pt_regs *regs)
{
    assert(PT_REGS_SYSCALL_PARM1(regs) == 71 && installed_fd);
    assert(PT_REGS_PARM5(regs) == AT_EMPTY_PATH);
    installed_fd = false;
    ++redirected_execs;
    return 0;
}

static long forwarded_exec(const struct pt_regs *regs)
{
    (void)regs;
    ++forwarded_execs;
    return -ENOEXEC;
}

static long (*ksu_syscall_table[])(const struct pt_regs *) = { redirected_exec, forwarded_exec };

static bool ksu_sucompat_vfs_enabled(void)
{
    return su_enabled;
}

static bool ksu_sucompat_vfs_visible(void)
{
    return su_visible;
}

static struct inode *d_inode(const struct dentry *entry)
{
    return entry->d_inode;
}

static struct inode *d_real_inode(const struct dentry *entry)
{
    if (entry->lower) {
        assert(entry->d_inode->i_op == &overlay_inode_ops);
        return d_real_inode(entry->lower);
    }
    return entry->d_inode;
}

static int kern_path(const char *name, unsigned int flags, struct path *path)
{
    if (flags & LOOKUP_DIRECTORY) {
        *path = visible_parent;
    } else {
        assert(strcmp(name, visible_name) == 0);
        ++leaf_lookups;
        if (leaf_error)
            return leaf_error;
        *path = visible_leaf;
    }
    ++path_refs;
    return 0;
}

static int user_path_at(int fd, const char *name, unsigned int flags, struct path *path)
{
    assert(fd == AT_FDCWD);
    rebind_at(DURING_RESOLVE);
    return kern_path(name, flags, path);
}

static void path_put(struct path *path)
{
    (void)path;
    assert(path_refs > 0);
    --path_refs;
}

static bool path_equal(const struct path *left, const struct path *right)
{
    return left->dentry == right->dentry && left->mnt == right->mnt;
}

static int kasumi_dh_split_parent(const char *path, char **parent, const char **child)
{
    const char *slash = strrchr(path, '/');
    assert(slash);
    *child = slash + 1;
    *parent = strndup(path, (size_t)(slash - path));
    return *parent ? 0 : -ENOMEM;
}

static struct kasumi_dh_iop *kasumi_dh_iop_of(struct inode *inode)
{
    return parent_iop.dir && parent_iop.dir->dir_inode == inode ? &parent_iop : NULL;
}

static u32 full_name_hash(const void *parent, const char *name, size_t length)
{
    (void)parent;
    (void)name;
    return (u32)length;
}

static struct kasumi_dh_child *kasumi_dh_find_child(
    struct kasumi_dh_dir *dir, const char *name, u16 length, u32 hash)
{
    assert(dir == parent_iop.dir);
    assert(length == strlen(name) && hash == length);
    assert(strcmp(name, strrchr(visible_name, '/') + 1) == 0);
    return governed ? &rule : NULL;
}

@FUNCTIONS@

static void check_mount_views(void)
{
    struct inode parent_inode = { .i_op = &real_inode_ops };
    struct inode current_su = { .i_op = &su_inode_ops, .i_ino = 41 };
    struct inode stale_su = { .i_op = &su_inode_ops, .i_ino = 40 };
    struct inode foreign_file = { .i_op = &real_inode_ops, .i_ino = 41 };
    struct inode overlay_inode = { .i_op = &overlay_inode_ops, .i_ino = 100 };
    struct dentry parent = { .d_inode = &parent_inode };
    struct dentry current = { .d_inode = &current_su };
    struct dentry stale = { .d_inode = &stale_su };
    struct dentry foreign = { .d_inode = &foreign_file };
    struct dentry overlay = { &overlay_inode, &current };
    struct dentry stacked_overlay = { &overlay_inode, &overlay };
    struct kasumi_dh_dir directory = { .dir_inode = &parent_inode };
    int original_mount, bind_mount;
    const struct path bound_parent = { &parent, &original_mount };
    visible_parent = bound_parent;
    visible_leaf = (struct path){ &current, &bind_mount };
    parent_iop.dir = &directory;

    struct path missing = {0};
    struct dentry negative = {0};
    assert(ksu_sucompat_vfs_path_ino(NULL) == 0);
    assert(ksu_sucompat_vfs_path_ino(&missing) == 0);
    missing.dentry = &negative;
    assert(ksu_sucompat_vfs_path_ino(&missing) == 0);

    /* A bind changes the mount identity while retaining the source dentry. */
    assert(ksu_sucompat_vfs_path_ino(&visible_leaf));
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 1);

    /* A bind of another file must reach the existing collision check. */
    visible_leaf.dentry = &foreign;
    assert(!ksu_sucompat_vfs_path_ino(&visible_leaf));
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 0);

    visible_leaf.dentry = &stale;
    assert(ksu_sucompat_vfs_path_ino(&visible_leaf));
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 0);

    /* Magic can bind a file from an already overlaid directory. */
    visible_leaf.dentry = &stacked_overlay;
    assert(ksu_sucompat_vfs_path_ino(&visible_leaf));
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 0);
    overlay.lower = &foreign;
    assert(!ksu_sucompat_vfs_path_ino(&visible_leaf));
    overlay.lower = &current;

    /* An overlay can also expose a vnode previously mirrored by Magic. */
    struct path magic_source = { &current, &bind_mount };
    overlay.lower = magic_source.dentry;
    visible_leaf = (struct path){ &overlay, &original_mount };
    assert(ksu_sucompat_vfs_path_ino(&visible_leaf));
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 0);

    visible_leaf.dentry = &current;
    leaf_error = -ENOENT;
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 0);
    leaf_error = -EACCES;
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == -EACCES);
    leaf_error = 0;
    assert(leaf_lookups == 7);

    /* Replacing the parent mount requires a fresh binding. */
    visible_parent.mnt = &bind_mount;
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 0);
    assert(leaf_lookups == 7);
    visible_parent = bound_parent;

    visible_name = "/custom/rootcmd";
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 1);
    governed = false;
    assert(kasumi_dirhijack_match_su(visible_name, &bound_parent, 41) == 0);
    assert(leaf_lookups == 8);
    governed = true;
    assert(path_refs == 0);
    parent_iop.dir = NULL;

    assert(su_permission(&current_su, 0) == 0);
    assert(su_permission(&stale_su, 0) == -EACCES);
    su_ino = 42;
    assert(su_permission(&current_su, 0) == -EACCES);
    current_su.i_ino = 42;
    assert(su_permission(&current_su, 0) == 0);
    assert(su_permission(&current_su, MAY_WRITE) == -EROFS);
    su_enabled = false;
    assert(su_permission(&current_su, 0) == -EACCES);
    su_enabled = true;
    su_visible = false;
    assert(su_permission(&current_su, 0) == -EACCES);
    su_visible = true;
}

static void check_exec_admission(void)
{
    struct inode current_su = { .i_op = &su_inode_ops, .i_ino = 41 };
    struct inode retired_su = { .i_op = &su_inode_ops, .i_ino = 40 };
    struct inode overlay_inode = { .i_op = &overlay_inode_ops, .i_ino = 100 };
    struct inode foreign_inode = { .i_op = &real_inode_ops, .i_ino = 41 };
    struct dentry current = { .d_inode = &current_su };
    struct dentry retired = { .d_inode = &retired_su };
    struct dentry overlay = { &overlay_inode, &current };
    struct dentry retired_overlay = { &overlay_inode, &retired };
    struct dentry foreign = { .d_inode = &foreign_inode };
    const struct {
        struct dentry *entry;
        bool current_binding;
    } cases[] = {
        { &current, true },
        { &overlay, true },
        { &retired, false },
        { &retired_overlay, false },
        { &foreign, false },
    };
    int bind_mount;

    for (unsigned int entry = 0; entry < sizeof(cases) / sizeof(cases[0]); ++entry) {
        for (int by_fd = 0; by_fd < 2; ++by_fd) {
            for (int prompted = 0; prompted < 2; ++prompted) {
                for (int stage = NO_REBIND; stage <= DURING_CAPTURE; ++stage) {
                    su_ino = 41;
                    rebind_stage = stage;
                    caller_allowed = !prompted;
                    prompt_requests = opened_ksud = closed_fds = 0;
                    root_escapes = redirected_execs = forwarded_execs = 0;
                    visible_name = "/system/bin/su";
                    visible_leaf = (struct path){ cases[entry].entry, &bind_mount };
                    retained_file.f_path = visible_leaf;
                    struct pt_regs regs = { .args = { 11, 0, 0, 0, AT_EMPTY_PATH } };
                    const char *name = by_fd ? "" : visible_name;
                    const char *argv[] = { "su", NULL };
                    long result = ksu_handle_execve_sucompat_common(
                        &name, argv, 0, by_fd, 1, &regs);
                    bool stale_at_entry = !cases[entry].current_binding ||
                                          stage == DURING_RESOLVE;
                    bool revoked = stage == DURING_OPEN || stage == DURING_CAPTURE ||
                                   (stage == DURING_PROMPT && prompted);
                    if (stale_at_entry) {
                        assert(result == -ENOEXEC && forwarded_execs == 1);
                        assert(root_escapes == 0 && opened_ksud == 0 && prompt_requests == 0);
                    } else if (revoked) {
                        assert(result == -EACCES && forwarded_execs == 0);
                        assert(root_escapes == 0 && redirected_execs == 0);
                        assert(closed_fds == (stage == DURING_PROMPT ? 0U : 1U));
                    } else {
                        assert(result == 0 && root_escapes == 1 && redirected_execs == 1);
                        assert(prompt_requests == (unsigned int)prompted && closed_fds == 0);
                    }
                    assert(path_refs == 0 && !installed_fd);
                }
            }
        }
    }

    overlay.lower = &retired;
    su_ino = 41;
    rebind_stage = NO_REBIND;
    retained_file.f_path = (struct path){ &overlay, &bind_mount };
    struct pt_regs regs = { .args = { 11, 0, 0, 0, AT_EMPTY_PATH } };
    assert(vfs_su_exec_ino("", true, &regs) == retired_su.i_ino);
    assert(!ksu_sucompat_vfs_current_ino(vfs_su_exec_ino("", true, &regs)));
    const char *name = "";
    caller_allowed = true;
    root_escapes = redirected_execs = forwarded_execs = 0;
    assert(ksu_handle_execve_sucompat_common(&name, NULL, 0, true, 1, &regs) == -ENOEXEC);
    assert(root_escapes == 0 && redirected_execs == 0 && forwarded_execs == 1);

    su_enabled = false;
    ksu_su_compat_enabled = true;
    name = "/system/bin/su";
    assert(ksu_handle_execve_sucompat_common(&name, NULL, 0, false, 1, &regs) == 0);
    assert(root_escapes == 1 && redirected_execs == 1);
    assert(path_refs == 0 && !installed_fd);
}

int main(void)
{
    check_mount_views();
    check_exec_admission();
    puts("PASS: mount identity and exec admission across stale bindings and rebind windows");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    kernel = Path(__file__).resolve().parents[1]
    vfs = (kernel / "feature/sucompat_vfs.c").read_text(encoding="utf-8")
    directory = (kernel / "kasumi/core/kasumi_dirhijack.c").read_text(encoding="utf-8")
    execution = (kernel / "feature/sucompat.c").read_text(encoding="utf-8")
    functions = [
        extract_function(vfs, "bool ksu_sucompat_vfs_current_ino("),
        extract_function(vfs, "bool ksu_sucompat_vfs_is_inode("),
        extract_function(vfs, "unsigned long ksu_sucompat_vfs_path_ino("),
        extract_function(vfs, "static int su_permission(struct inode *inode,").replace(
            "#endif\n", ""
        ),
        extract_function(directory, "int kasumi_dirhijack_match_su("),
        extract_function(execution, "static int resolve_exec_path("),
        extract_function(execution, "static unsigned long vfs_su_exec_ino("),
        extract_function(execution, "static long\nksu_handle_execve_sucompat_common("),
    ]
    with tempfile.TemporaryDirectory(prefix="yukisu-sucompat-mount-") as temporary:
        source = Path(temporary) / "sucompat_mount_test.c"
        binary = Path(temporary) / "sucompat_mount_test"
        source.write_text(FIXTURE.replace("@FUNCTIONS@", "\n".join(functions)), encoding="utf-8")
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            flags.append("-fsanitize=address,undefined")
        subprocess.run([args.cc, *flags, str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
