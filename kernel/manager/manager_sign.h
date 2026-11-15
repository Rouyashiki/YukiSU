#ifndef MANAGER_SIGN_H
#define MANAGER_SIGN_H

// Release manager signing certificates.
#define EXPECTED_SIZE 0x2c6
#define EXPECTED_HASH                                                          \
	"45bd30fe301b6a6ff798e0ae930606d5f443c16f078a95ec9d82d326498ed1c3"

#define LEGACY_SIZE 0x29c
#define LEGACY_HASH                                                            \
	"39559b380d4c0191eed81b7eba61533b6a2f247bc55bceba4259e983673f58b7"

#define APK_SIGN_FLAG_TRUSTED 0x1

typedef struct {
	u32 size;
	const char *sha256;
	u32 flags;
	const char *name;
} apk_sign_key_t;

#define PRESET_SIZE_OFFICIAL 0x033b
#define PRESET_HASH_OFFICIAL                                                   \
	"c371061b19d8c7d7d6133c6a9bafe198fa944e50c1b31c9d8daa8d7f1fc2d2d6"

#define PRESET_SIZE_MKSU 384
#define PRESET_HASH_MKSU                                                       \
	"7e0c6d7278a3bb8e364e0fcba95afaf3666cf5ff3c245a3b63c8833bd0445cc4"

#define PRESET_SIZE_RKSU 0x396
#define PRESET_HASH_RKSU                                                       \
	"f415f4ed9435427e1fdf7f1fccd4dbc07b3d6b8751e4dbcec6f19671f427870b"

#define PRESET_SIZE_SUKISU 0x35c
#define PRESET_HASH_SUKISU                                                     \
	"947ae944f3de4ed4c21a7e4f7953ecf351bfa2b36239da37a34111ad29993eef"

#define PRESET_SIZE_RESUKISU 0x377
#define PRESET_HASH_RESUKISU                                                   \
	"d3469712b6214462764a1d8d3e5cbe1d6819a0b629791b9f4101867821f1df64"

#define PRESET_SIZE_KOWSU 0x375
#define PRESET_HASH_KOWSU                                                      \
	"484fcba6e6c43b1fb09700633bf2fb4758f13cb0b2f4457b80d075084b26c588"

#define PRESET_SIZE_KSUN 0x3e6
#define PRESET_HASH_KSUN                                                       \
	"79e590113c4c4c0c222978e413a5faa801666957b1212a328e46c00c69821bf7"

#define PRESET_SIZE_XXKSU 0x363
#define PRESET_HASH_XXKSU                                                      \
	"4359c171f32543394cbc23ef908c4bb94cad7c8087002ba164c8230948c21549"

#define PRESET_SIZE_FINASU 0x2cc
#define PRESET_HASH_FINASU                                                     \
	"2d89b50aaf1db013c2b6738f973f48b62ea21a3a8d7216bfec3521065d448458"

#endif /* MANAGER_SIGN_H */
