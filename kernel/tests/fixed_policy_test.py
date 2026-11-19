"""Exercise fixed-policy status across boot installation and SELinux reloads."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


def definition(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


class FixedPolicyTest(unittest.TestCase):
    def test_boot_reader_requires_an_independent_inode(self):
        root = Path(__file__).resolve().parents[2]
        state = (root / 'kernel/feature/yukizygisk/fixed_policy.c').read_text()
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "uapi/fixed_policy.h"
typedef uint32_t u32;
struct cred { int unused; };
static const struct cred *ksu_cred;
struct inode { mode_t i_mode; uid_t i_uid; nlink_t i_nlink; size_t size; };
struct file { struct inode inode; };
static struct file metadata;
static struct yz_fixed_policy_config yz_fixed_config;
static bool link_during_read;
static bool inject_self_domain;
static unsigned reads;
#define YZ_POLICY_PRIMARY "primary"
#define yz_policy_open(path) (&metadata)
#define yz_policy_close(file) ((void)0)
#define override_creds(c) (c)
#define revert_creds(c) ((void)0)
#define IS_ERR(p) false
#define PTR_ERR(p) (-ENOENT)
#define file_inode(f) (&(f)->inode)
#define uid_eq(a, b) ((a) == (b))
#define GLOBAL_ROOT_UID 0
#define READ_ONCE(x) (x)
#define i_size_read(i) ((i)->size)
static ssize_t yz_policy_read(struct file *file, void *out, size_t n, loff_t *pos) {
    struct yz_fixed_policy_config record = {.version = YZ_FIXED_POLICY_VERSION, .enabled = 1};
    if (inject_self_domain) {
        record.count = 1;
        strcpy(record.domains[0], "self");
    }
    ++reads;
    assert(n == sizeof(record));
    memcpy(out, &record, n);
    if (link_during_read) ++file->inode.i_nlink;
    return n;
}
''' + definition(state, 'static int yz_fixed_read_config(') + r'''
int main(void) {
    metadata.inode = (struct inode){S_IFREG | 0600, 0, 2, sizeof(yz_fixed_config)};
    assert(yz_fixed_read_config() == -EINVAL && reads == 0);
    metadata.inode.i_nlink = 1;
    assert(yz_fixed_read_config() == 0 && reads == 1 && yz_fixed_config.enabled);
    inject_self_domain = true;
    assert(yz_fixed_read_config() == -EINVAL && reads == 2);
    inject_self_domain = false;
    link_during_read = true;
    assert(yz_fixed_read_config() == -EINVAL && reads == 3);
}
'''
        with tempfile.TemporaryDirectory() as work:
            binary = Path(work) / 'boot-profile'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror',
                            '-I', str(root), '-x', 'c', '-', '-o', str(binary)],
                           input=fixture, text=True, check=True)
            subprocess.run([str(binary)], check=True)

    def test_apply_and_reload(self):
        root = Path(__file__).resolve().parents[2]
        feature = root / 'kernel/feature/yukizygisk'
        state = (feature / 'fixed_policy.c').read_text()
        database = (feature / 'policy/database.h').read_text()
        host = (root / 'kernel/selinux/rules.c').read_text()
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "uapi/fixed_policy.h"
typedef uint32_t u32;
struct class_datum { unsigned value; };
struct type_datum { u32 value; bool primary, attribute; };
struct symtab { u32 nprim; };
struct ebitmap_node { u32 bits; };
struct ebitmap { struct ebitmap_node node; };
#define ebitmap_for_each_positive_bit(map, nodep, bit) \
    for ((nodep) = &(map)->node, (bit) = 0; (bit) < 8; ++(bit)) if ((nodep)->bits & (1U << (bit)))
#define FILE_ATTRS ((1U << 5) | (1U << 3) | (1U << 4))
#define SYM_TYPES 0
struct policydb {
    struct symtab p_types, p_classes; bool allowed;
    const char *sym_val_to_name[1][6];
    struct ebitmap type_attr_map_array[6];
};
struct selinux_policy { u32 latest_granting; struct policydb policydb; };
static struct { bool initialized; int policy_mutex; struct selinux_policy *policy; } selinux_state;
static struct yz_fixed_policy_config yz_fixed_config = {.enabled = 1};
static struct yz_fixed_policy_status yz_fixed_status = {.requested = 1};
static int yz_fixed_lock, verify_count, install_error;
static bool yz_fixed_apply_pending = true;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#define KERNEL_SU_DOMAIN "ksu"
#define yz_fixed_policy_latch() ((void)0)
#define ksu_fixed_inode_layout() 0
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p, val) (*(p) = (val))
#define mutex_lock(p) assert((*(p))++ == 0)
#define mutex_unlock(p) assert((*(p))-- == 1)
#define lockdep_assert_held(p) assert(*(p) == 1)
#define rcu_dereference_protected(p, held) (p)
#define yz_fp_state (&selinux_state)
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define spin_lock_irqsave(p, flags) ((flags) = 0)
#define spin_unlock_irqrestore(p, flags) ((void)(flags))
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define IS_ERR(ptr) (false)
#define PTR_ERR(ptr) (-EINVAL)
#define strscpy(dst, src, size) (assert(strlen(src) < (size)), strcpy(dst, src))
static struct selinux_policy scratch;
static struct selinux_policy *ksu_dup_sepolicy(struct selinux_policy *old) {
    scratch = *old;
    return &scratch;
}
#define ksu_destroy_sepolicy(policy) assert((policy) == &scratch)
static void publish_policy(struct selinux_policy *next, struct selinux_policy *old) {
    next->latest_granting = old->latest_granting + 1;
    selinux_state.policy = next;
}
static void *yz_fp_search(struct symtab *table, const char *name) {
    static struct type_datum types[] = {{1, true}, {2, true}, {3, false, true},
        {4, false, true}, {5, false, true}, {6, true}};
    static struct class_datum cls = {1};
    if (!strcmp(name, "fd")) return &cls;
    if (!strcmp(name, "ksu")) return &types[0];
    if (!strcmp(name, "domain")) return &types[2];
    if (!strcmp(name, "file_type")) return &types[3];
    if (!strcmp(name, "mlstrustedobject")) return &types[4];
    if (!strcmp(name, YZ_FIXED_FILE_TYPE)) return &types[5];
    if (!strcmp(name, "zygote")) return &types[1];
    return NULL;
}
#define ksu_ebitmap_has_bit(map, bit) (true)
#define yz_fixed_add_file_type(db) yz_fixed_verify_file_type(db)
#define ksu_file_perm_mask(cls, name) ((cls) && !strcmp(name, "use") ? 4U : 0U)
struct yz_fixed_rule { const char *source, *target, *class_name, *permission; };
static const struct yz_fixed_rule yz_fixed_rules[] = {{"domain", "$daemon", "fd", "use"}};
static int yz_fp_has_rule(struct policydb *db, const char *s, const char *t, const char *c,
                          const char *p) {
    ++verify_count;
    return db->allowed ? 0 : -EACCES;
}
static int yz_fp_allow(struct policydb *db, const char *s, const char *t, const char *c,
                       const char *p) {
    assert(db == &scratch.policydb && selinux_state.policy_mutex);
    db->allowed = !install_error;
    return install_error;
}
'''
        fixture += definition(state, 'void yz_fixed_set_missing(')
        fixture += definition(database, 'static int yz_fixed_verify_file_type(')
        fixture += definition(database, 'static const char *yz_fixed_resolve(')
        fixture += definition(database, 'static int yz_fixed_apply_rules(')
        start = host.index('static const struct yz_fixed_policy_config *yz_fixed_boot_config;')
        fixture += host[start:host.index('#include "feature/yukizygisk/policy/access.h"', start)]
        access = (feature / 'policy/access.h').read_text()
        fixture += 'struct context { u32 type; };\n'
        fixture += definition(access, 'static struct selinux_policy *yz_fixed_lock_policy(')
        fixture += definition(access, 'static const char *yz_fixed_type_name(')
        fixture += definition(host, 'int yz_fixed_policy_apply(')
        fixture += definition(state, 'int yz_fixed_policy_boot(')
        fixture += definition(state, 'void yz_fixed_policy_status(')
        fixture += r'''
static void expect_status(bool active, int error) {
    struct yz_fixed_policy_status status;
    yz_fixed_policy_status(&status);
    assert(status.initialized == (error != -ENOMEM));
    assert(status.requested && status.active == active && status.error == error);
    if (error == -ENOMEM || error == -EACCES) {
        assert(!strcmp(status.source, "domain") && !strcmp(status.target, "ksu"));
        assert(!strcmp(status.class_name, "fd") && status.missing_av == 4);
    } else {
        assert(!status.source[0] && !status.target[0] && !status.class_name[0] && !status.missing_av);
    }
}
int main(void) {
    struct selinux_policy original = {.latest_granting = 10, .policydb = {
        .p_types = {6}, .sym_val_to_name = {{"ksu", "zygote", "domain", [5] = YZ_FIXED_FILE_TYPE}},
        .type_attr_map_array = {[5].node.bits = FILE_ATTRS | (1U << 6)}}};
    selinux_state.initialized = true;
    selinux_state.policy = &original;
    assert(yz_fixed_policy_apply(&yz_fixed_config, NULL) == -EEXIST && selinux_state.policy == &original);
    original.policydb.type_attr_map_array[5].node.bits = FILE_ATTRS;
    /* Legacy boot profiles carrying retired ZN domains still apply cleanly. */
    yz_fixed_config.count = 1;
    strcpy(yz_fixed_config.domains[0], "old_system_server");
    install_error = -ENOMEM;
    assert(yz_fixed_policy_boot(true) == -ENOMEM && selinux_state.policy == &original);
    expect_status(false, -ENOMEM);
    install_error = 0;
    assert(yz_fixed_policy_boot(true) == 0 && selinux_state.policy == &scratch);
    expect_status(true, 0);
    ++scratch.latest_granting;
    scratch.policydb.sym_val_to_name[SYM_TYPES][2] = "replacement_attr";
    expect_status(true, -EINVAL);
    ++scratch.latest_granting;
    scratch.policydb.sym_val_to_name[SYM_TYPES][2] = "domain";
    expect_status(true, 0);
    ++scratch.latest_granting;
    scratch.policydb.sym_val_to_name[SYM_TYPES][0] = "replacement_daemon";
    expect_status(true, -EINVAL);
    ++scratch.latest_granting;
    scratch.policydb.sym_val_to_name[SYM_TYPES][0] = "ksu";
    expect_status(true, 0);
    assert(yz_fixed_lock_policy() == &scratch && selinux_state.policy_mutex);
    mutex_unlock(&selinux_state.policy_mutex);
    ++scratch.latest_granting;
    scratch.policydb.allowed = false;
    assert(yz_fixed_lock_policy() == ERR_PTR(-EACCES) && !selinux_state.policy_mutex);
    expect_status(true, -EACCES);
    int checked = verify_count;
    expect_status(true, -EACCES);
    assert(verify_count == checked);
    ++scratch.latest_granting;
    scratch.policydb.allowed = true;
    expect_status(true, 0);
    ++scratch.latest_granting;
    scratch.policydb.type_attr_map_array[5].node.bits |= 1U << 6;
    expect_status(true, -EEXIST);
    ++scratch.latest_granting;
    scratch.policydb.type_attr_map_array[5].node.bits = FILE_ATTRS & ~(1U << 5);
    expect_status(true, -EEXIST);
    ++scratch.latest_granting;
    scratch.policydb.type_attr_map_array[5].node.bits = FILE_ATTRS;
    scratch.policydb.sym_val_to_name[SYM_TYPES][5] = "another_file";
    expect_status(true, -EEXIST);
    struct context bad_zero = {0}, bad_high = {99}, bad_null = {4}, ok_ctx = {2};
    assert(!strcmp(yz_fixed_type_name(&original.policydb, NULL), "?"));
    assert(!strcmp(yz_fixed_type_name(&original.policydb, &bad_zero), "?"));
    assert(!strcmp(yz_fixed_type_name(&original.policydb, &bad_high), "?"));
    assert(!strcmp(yz_fixed_type_name(&original.policydb, &bad_null), "?"));
    assert(!strcmp(yz_fixed_type_name(&original.policydb, &ok_ctx), "zygote"));
    selinux_state.policy = NULL;
    expect_status(true, -EAGAIN);
}
'''
        with tempfile.TemporaryDirectory() as work:
            binary = Path(work) / 'fixed-policy'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(root), '-x', 'c', '-',
                            '-o', str(binary)], input=fixture, text=True, check=True)
            subprocess.run([str(binary)], check=True)

    def test_label_checks_execute_and_fd_use(self):
        root = Path(__file__).resolve().parents[2]
        access = (root / 'kernel/feature/yukizygisk/policy/access.h').read_text()
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "uapi/fixed_policy.h"
typedef uint32_t u32;
struct cred { u32 sid; };
struct inode_security_struct { u32 sid, sclass, initialized; int lock; };
struct super_block { int s_magic; };
struct inode { int i_mode; struct super_block *i_sb; struct inode_security_struct security; };
struct file { struct inode *inode; const struct cred *f_cred; };
struct selinux_policy { u32 latest_granting; };
static struct { int policy_mutex; struct selinux_policy *policy; } selinux_state;
static struct selinux_policy policy = {.latest_granting = 11};
static int checks;
static bool saw_exec, saw_fd;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#define yz_fp_state (&selinux_state)
#define yz_fp_inode(i) (&(i)->security)
#define yz_fp_cred_sid(c) ((c)->sid)
#define file_inode(f) ((f)->inode)
#define yz_fixed_policy_active() (true)
#define yz_fixed_private_module_image(f, i) (true)
#define TMPFS_MAGIC 42
#define SECCLASS_FILE 1
#define LABEL_INITIALIZED 1
#define GFP_KERNEL 0
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((intptr_t)(p))
#define mutex_unlock(p) assert((*(p))-- == 1)
#define spin_lock(l) assert(selinux_state.policy_mutex && !(*(l))++)
#define spin_unlock(l) assert(selinux_state.policy_mutex && (*(l))-- == 1)
static struct selinux_policy *yz_fixed_lock_policy(void) {
    assert(selinux_state.policy_mutex++ == 0);
    return &policy;
}
static int yz_fp_context_sid(const char *ctx, size_t len, u32 *sid, int flags) {
    (void)ctx; (void)len; (void)flags;
    *sid = 99;
    return 0;
}
static int yz_fixed_check_access_locked(struct selinux_policy *p, u32 s, u32 t,
                                        const char *cls, const char *const *perms, size_t count) {
    (void)p; (void)s;
    ++checks;
    if (!strcmp(cls, "fd")) {
        saw_fd = true;
        return 0;
    }
    assert(t == 99);
    for (size_t i = 0; i < count; ++i)
        saw_exec |= !strcmp(perms[i], "execute");
    return 0;
}
''' + definition(access, 'static int yz_fixed_check_file_locked(') + definition(access, 'int yz_fixed_policy_label(') + r'''
int main(void) {
    struct cred daemon = {1}, receiver = {2};
    struct super_block sb = {TMPFS_MAGIC};
    struct inode inode = {S_IFREG, &sb};
    struct file file = {&inode, &daemon};
    assert(yz_fixed_policy_label(&file, &receiver) == 0);
    assert(checks == 2 && saw_exec && saw_fd && inode.security.sid == 99);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as work:
            binary = Path(work) / 'label-exec'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(root), '-x', 'c', '-',
                            '-o', str(binary)], input=fixture, text=True, check=True)
            subprocess.run([str(binary)], check=True)

    def test_generated_rules_cover_all_domains(self):
        root = Path(__file__).resolve().parents[2]
        policy = root / 'kernel/feature/yukizygisk/policy'
        with tempfile.TemporaryDirectory() as work:
            generated = Path(work) / 'rules.h'
            subprocess.run(['python3', str(policy / 'generate.py'),
                            str(policy / 'sepolicy.rule'), str(generated)],
                           check=True)
            rules = generated.read_text()
            self.assertNotIn('$target', rules)
            self.assertNotIn('"self"', rules)
            self.assertIn('{ "domain", "domain", "process", "execmem" },', rules)
            self.assertIn('{ "domain", "yukizygisk_file", "file", "execute" },', rules)
            self.assertIn('{ "domain", "yukizygisk_file", "memfd_file", "execute" },', rules)
            self.assertIn('{ "domain", "$daemon", "fd", "use" },', rules)
            self.assertIn('{ "domain", "$daemon", "fifo_file", "open" },', rules)
            self.assertIn('{ "domain", "$daemon", "unix_stream_socket", "setopt" },', rules)
            self.assertIn('{ "domain", "$daemon", "unix_stream_socket", "shutdown" },', rules)


if __name__ == '__main__':
    unittest.main()
