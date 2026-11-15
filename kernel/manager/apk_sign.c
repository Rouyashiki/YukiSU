#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/version.h>
#ifdef CONFIG_KSU_DEBUG
#include <linux/moduleparam.h>
#endif // #ifdef CONFIG_KSU_DEBUG
#include <crypto/hash.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#include <crypto/sha2.h>
#else
#include <crypto/sha.h>
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

#include "manager/apk_sign.h"
#include "util.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/dynamic_manager.h"
#include "manager/manager_sign.h"

#ifdef CONFIG_KSU_SUPERKEY
#include "manager/superkey.h"
extern bool ksu_signature_bypass;
#endif // #ifdef CONFIG_KSU_SUPERKEY

struct sdesc {
	struct shash_desc shash;
	char ctx[];
};

static apk_sign_key_t apk_sign_keys[] = {
    {EXPECTED_SIZE, EXPECTED_HASH, APK_SIGN_FLAG_TRUSTED, "YukiSU"},
    {LEGACY_SIZE, LEGACY_HASH, APK_SIGN_FLAG_TRUSTED, "YukiSU (legacy)"},
    {PRESET_SIZE_OFFICIAL, PRESET_HASH_OFFICIAL, 0, "KernelSU"},
    {PRESET_SIZE_MKSU, PRESET_HASH_MKSU, 0, "MKSU"},
    {PRESET_SIZE_RKSU, PRESET_HASH_RKSU, 0, "RKSU"},
    {PRESET_SIZE_SUKISU, PRESET_HASH_SUKISU, 0, "SukiSU"},
    {PRESET_SIZE_RESUKISU, PRESET_HASH_RESUKISU, 0, "ReSukiSU"},
    {PRESET_SIZE_KOWSU, PRESET_HASH_KOWSU, 0, "KowSU"},
    {PRESET_SIZE_KSUN, PRESET_HASH_KSUN, 0, "KernelSU-Next"},
    {PRESET_SIZE_XXKSU, PRESET_HASH_XXKSU, 0, "xxKSU"},
    {PRESET_SIZE_FINASU, PRESET_HASH_FINASU, 0, "FinaSU"},
};

static struct sdesc *init_sdesc(struct crypto_shash *alg)
{
	struct sdesc *sdesc;
	int size;

	size = sizeof(struct shash_desc) + crypto_shash_descsize(alg);
	sdesc = kzalloc(size, GFP_KERNEL);
	if (!sdesc)
		return ERR_PTR(-ENOMEM);
	sdesc->shash.tfm = alg;
	return sdesc;
}

static int calc_hash(struct crypto_shash *alg, const unsigned char *data,
		     unsigned int datalen, unsigned char *digest)
{
	struct sdesc *sdesc;
	int ret;

	sdesc = init_sdesc(alg);
	if (IS_ERR(sdesc)) {
		pr_info("can't alloc sdesc\n");
		return PTR_ERR(sdesc);
	}

	ret = crypto_shash_digest(&sdesc->shash, data, datalen, digest);
	kfree(sdesc);
	return ret;
}

static int ksu_sha256(const unsigned char *data, unsigned int datalen,
		      unsigned char *digest)
{
	struct crypto_shash *alg;
	char *hash_alg_name = "sha256";
	int ret;

	alg = crypto_alloc_shash(hash_alg_name, 0, 0);
	if (IS_ERR(alg)) {
		pr_info("can't alloc alg %s\n", hash_alg_name);
		return PTR_ERR(alg);
	}
	ret = calc_hash(alg, data, datalen, digest);
	crypto_free_shash(alg);
	return ret;
}

static bool read_exact(struct file *fp, void *buffer, size_t size, loff_t *pos,
		       loff_t end)
{
	if (*pos < 0 || *pos > end || size > (size_t)(end - *pos))
		return false;

	return kernel_read(fp, buffer, size, pos) == (ssize_t)size;
}

static bool read_length_prefixed_end(struct file *fp, loff_t *pos,
				     loff_t container_end, loff_t *value_end)
{
	u32 length;

	if (!read_exact(fp, &length, sizeof(length), pos, container_end))
		return false;
	if (length > INT_MAX || length > (u64)(container_end - *pos))
		return false;

	*value_end = *pos + length;
	return true;
}

static bool check_block(struct file *fp, loff_t *pos, loff_t block_end,
			struct apk_sign_match *match)
{
	loff_t signers_end, signer_end, signed_data_end, digests_end,
	    certificates_end;
	u32 certificate_size;
	bool signature_valid = false;
	int i;
	apk_sign_key_t sign_key;

	if (!read_length_prefixed_end(fp, pos, block_end, &signers_end) ||
	    !read_length_prefixed_end(fp, pos, signers_end, &signer_end) ||
	    !read_length_prefixed_end(fp, pos, signer_end, &signed_data_end) ||
	    !read_length_prefixed_end(fp, pos, signed_data_end, &digests_end))
		return false;

	*pos = digests_end;
	if (!read_length_prefixed_end(fp, pos, signed_data_end,
				      &certificates_end) ||
	    !read_exact(fp, &certificate_size, sizeof(certificate_size), pos,
			certificates_end))
		return false;
	if (certificate_size > INT_MAX ||
	    certificate_size > (u64)(certificates_end - *pos))
		return false;

#define CERT_MAX_LENGTH 1024
	if (certificate_size > CERT_MAX_LENGTH) {
		pr_info("cert length overlimit\n");
		return false;
	}

	char cert[CERT_MAX_LENGTH];
	if (!read_exact(fp, cert, certificate_size, pos, certificates_end))
		return false;

	unsigned char digest[SHA256_DIGEST_SIZE];
	if (ksu_sha256(cert, certificate_size, digest)) {
		pr_info("sha256 error\n");
		return false;
	}

	char hash_str[SHA256_DIGEST_SIZE * 2 + 1];
	hash_str[SHA256_DIGEST_SIZE * 2] = '\0';

	bin2hex(hash_str, digest, SHA256_DIGEST_SIZE);
	pr_info("sha256: %s\n", hash_str);
	for (i = 0; i < ARRAY_SIZE(apk_sign_keys); i++) {
		sign_key = apk_sign_keys[i];
		if (certificate_size != sign_key.size)
			continue;
		pr_info("sha256: %s, expected: %s, index: %d\n", hash_str,
			sign_key.sha256, i);
		if (strcmp(sign_key.sha256, hash_str) == 0) {
			signature_valid = true;
			if (match) {
				match->index = i;
				match->trusted = (sign_key.flags &
						  APK_SIGN_FLAG_TRUSTED) != 0;
				match->name = sign_key.name;
				match->size = certificate_size;
				strscpy(match->hash, hash_str,
					sizeof(match->hash));
			}
			break;
		}
	}
	if (!signature_valid &&
	    ksu_dynamic_manager_is_trusted_sign(certificate_size, hash_str)) {
		signature_valid = true;
		if (match) {
			match->index = -1;
			match->trusted = true;
			match->name = "dynamic";
			match->size = certificate_size;
			strscpy(match->hash, hash_str, sizeof(match->hash));
		}
	}

	return signature_valid;
}

struct zip_entry_header {
	uint32_t signature;
	uint16_t version;
	uint16_t flags;
	uint16_t compression;
	uint16_t mod_time;
	uint16_t mod_date;
	uint32_t crc32;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint16_t file_name_length;
	uint16_t extra_field_length;
} __attribute__((packed));

// This is a necessary but not sufficient condition, but it is enough for us
static bool has_v1_signature_file(struct file *fp)
{
	struct zip_entry_header header;
	const char MANIFEST[] = "META-INF/MANIFEST.MF";

	loff_t pos = 0;

	while (kernel_read(fp, &header, sizeof(struct zip_entry_header),
			   &pos) == sizeof(struct zip_entry_header)) {
		if (header.signature != 0x04034b50) {
			// ZIP magic: 'PK'
			return false;
		}
		// Read the entry file name
		if (header.file_name_length == sizeof(MANIFEST) - 1) {
			char fileName[sizeof(MANIFEST)];
			kernel_read(fp, fileName, header.file_name_length,
				    &pos);
			fileName[header.file_name_length] = '\0';

			// Check if the entry matches META-INF/MANIFEST.MF
			if (strncmp(MANIFEST, fileName, sizeof(MANIFEST) - 1) ==
			    0) {
				return true;
			}
		} else {
			// Skip the entry file name
			pos += header.file_name_length;
		}

		// Skip to the next entry
		pos += header.extra_field_length + header.compressed_size;
	}

	return false;
}

static __always_inline bool check_v2_signature(char *path,
					       struct apk_sign_match *match)
{
	unsigned char buffer[0x10] = {0};
	u32 cd_offset, cd_size;
	u32 zip64_locator_magic;
	u64 size_of_block, size_of_block_at_head;

	loff_t pos, pairs_end, file_size, eocd_offset;

	bool v2_signing_valid = false;
	int v2_signing_blocks = 0;

	int i;
	struct file *fp = ksu_filp_open_nonotify(path, O_RDONLY | O_NOATIME);
	if (IS_ERR(fp)) {
		pr_err("open %s error.\n", path);
		return false;
	}

	file_size = generic_file_llseek(fp, 0, SEEK_END);
	if (file_size < 0)
		goto clean;

	// https://en.wikipedia.org/wiki/Zip_(file_format)#End_of_central_directory_record_(EOCD)
	for (i = 0;; ++i) {
		unsigned short comment_size;
		u32 magic;
		pos = file_size - i - 2;
		if (!read_exact(fp, &comment_size, sizeof(comment_size), &pos,
				file_size))
			goto clean;
		if (comment_size == i) {
			pos -= 22;
			if (!read_exact(fp, &magic, sizeof(magic), &pos,
					file_size))
				goto clean;
			if (magic == 0x06054b50) {
				eocd_offset = pos - sizeof(magic);
				break;
			}
		}
		if (i == 0xffff) {
			pr_info("error: cannot find eocd\n");
			goto clean;
		}
	}

	// Reject ZIP64 before looking for a signing block.
	if (eocd_offset >= 20) {
		pos = eocd_offset - 20;
		if (!read_exact(fp, &zip64_locator_magic,
				sizeof(zip64_locator_magic), &pos, file_size))
			goto clean;
		if (zip64_locator_magic == 0x07064b50)
			goto clean;
	}

	pos = eocd_offset + 12;
	// Size of central directory.
	if (!read_exact(fp, &cd_size, sizeof(cd_size), &pos, file_size))
		goto clean;
	// offset of central directory
	if (!read_exact(fp, &cd_offset, sizeof(cd_offset), &pos, file_size))
		goto clean;
	if ((u64)cd_offset > (u64)eocd_offset ||
	    (u64)cd_size != (u64)eocd_offset - cd_offset)
		goto clean;
	if (cd_offset < 0x20)
		goto clean;

	pairs_end = (loff_t)cd_offset - 0x18;
	pos = pairs_end;

	if (!read_exact(fp, &size_of_block, sizeof(size_of_block), &pos,
			cd_offset))
		goto clean;
	if (!read_exact(fp, buffer, sizeof(buffer), &pos, cd_offset))
		goto clean;
	if (memcmp((char *)buffer, "APK Sig Block 42", sizeof(buffer))) {
		goto clean;
	}

	if (size_of_block < 0x18 || size_of_block > INT_MAX - 0x8 ||
	    size_of_block > (u64)cd_offset - 0x8)
		goto clean;
	pos = (loff_t)cd_offset - (loff_t)size_of_block - 0x8;
	if (!read_exact(fp, &size_of_block_at_head,
			sizeof(size_of_block_at_head), &pos, pairs_end))
		goto clean;
	if (size_of_block_at_head != size_of_block)
		goto clean;

	// Scan every length-prefixed pair, matching AOSP's signing block
	// parser. Each valid pair consumes an 8-byte length plus at least a
	// 4-byte ID, so malformed entries fail below instead of spinning in
	// place.
	while (pos < pairs_end) {
		uint32_t id;
		u64 size_of_pair;
		loff_t pair_end;

		if (!read_exact(fp, &size_of_pair, sizeof(size_of_pair), &pos,
				pairs_end))
			goto invalid;
		if (size_of_pair < sizeof(id) || size_of_pair > INT_MAX ||
		    size_of_pair > (u64)(pairs_end - pos))
			goto invalid;

		pair_end = pos + (loff_t)size_of_pair;
		if (!read_exact(fp, &id, sizeof(id), &pos, pair_end))
			goto invalid;

		if (id == 0x7109871au) {
			v2_signing_blocks++;
			v2_signing_valid =
			    check_block(fp, &pos, pair_end, match);
		} else if (id != 0x42726577u) { /* APK verity padding */
#ifdef CONFIG_KSU_DEBUG
			pr_info("Unexpected signature block id: 0x%08x\n", id);
#endif
			goto invalid;
		}
		pos = pair_end;
	}

	if (v2_signing_blocks != 1) {
#ifdef CONFIG_KSU_DEBUG
		pr_err("Unexpected v2 signature count: %d\n",
		       v2_signing_blocks);
#endif // #ifdef CONFIG_KSU_DEBUG
		v2_signing_valid = false;
	}

	if (v2_signing_valid) {
		int has_v1_signing = has_v1_signature_file(fp);
		if (has_v1_signing) {
			pr_err("Unexpected v1 signature scheme found!\n");
			goto invalid;
		}
	}

	goto clean;

invalid:
	v2_signing_valid = false;
clean:
	filp_close(fp, 0);

	return v2_signing_valid;
}

#ifdef CONFIG_KSU_DEBUG

int ksu_debug_manager_uid = -1;

#include "manager/manager_identity.h"

static int set_expected_size(const char *val, const struct kernel_param *kp)
{
	int rv = param_set_uint(val, kp);
	ksu_set_manager_uid(ksu_debug_manager_uid);
	pr_info("ksu_manager_uid set to %d\n", ksu_debug_manager_uid);
	return rv;
}

static struct kernel_param_ops expected_size_ops = {
    .set = set_expected_size,
    .get = param_get_uint,
};

module_param_cb(ksu_debug_manager_uid, &expected_size_ops,
		&ksu_debug_manager_uid, S_IRUSR | S_IWUSR);

#endif // #ifdef CONFIG_KSU_DEBUG

bool is_manager_apk_ex(char *path, int *signature_index)
{
	struct apk_sign_match match = {
	    .index = -1,
	};

#ifdef CONFIG_KSU_SUPERKEY
	// SuperKey mode: signature verification is bypassed entirely.
	// The manager is identified by password (via prctl), not by APK
	// signature — so this function correctly returns false (no
	// signature match) and the caller falls through to SuperKey auth.
	if (superkey_is_set() && superkey_is_signature_bypassed()) {
		return false;
	}
#endif // #ifdef CONFIG_KSU_SUPERKEY
	if (!check_v2_signature(path, &match) || !match.trusted)
		return false;
	if (signature_index)
		*signature_index = match.index;
	return true;
}

bool is_manager_apk(char *path)
{
	/*
	 * If path is NULL, we just want to know whether signature
	 * verification is generally enabled/ok for the manager.
	 * In SuperKey mode, signature verification is irrelevant —
	 * the manager is identified by password, not by APK signature.
	 */
	if (!path) {
#ifdef CONFIG_KSU_SUPERKEY
		// In SuperKey bypass mode, signature verification is
		// disabled — report false so the caller knows not to rely
		// on signature checks.
		if (superkey_is_set() && superkey_is_signature_bypassed())
			return false;
#endif // #ifdef CONFIG_KSU_SUPERKEY
		return true;
	}

	return is_manager_apk_ex(path, NULL);
}

bool match_apk_signature(char *path, struct apk_sign_match *match)
{
	if (match) {
		match->index = -1;
		match->trusted = false;
		match->name = NULL;
		match->size = 0;
		match->hash[0] = '\0';
	}
	return check_v2_signature(path, match);
}
