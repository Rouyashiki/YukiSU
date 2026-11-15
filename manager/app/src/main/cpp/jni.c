#include "ksu.h"
#include "prelude.h"

#include <android/log.h>
#include <errno.h>
#include <jni.h>
#include <linux/capability.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

NativeBridgeNP(getVersion, jint) { return (jint)get_version(); }

NativeBridgeNP(getUapiVersion, jint) { return (jint)get_uapi_version(); }

NativeBridgeNP(getManagerUapiVersion, jint) {
  return (jint)get_manager_uapi_version();
}

// get VERSION FULL
NativeBridgeNP(getFullVersion, jstring) {
  char buff[255] = {0};
  get_full_version((char *)&buff);
  return GetEnvironment()->NewStringUTF(env, buff);
}

NativeBridgeNP(getAllowList, jintArray) {
  struct ksu_get_allow_list_cmd cmd = {};
  bool result = get_allow_list(&cmd);

  if (result) {
    jsize array_size = (jsize)cmd.count;
    if (array_size < 0 || (unsigned int)array_size != cmd.count) {
      LogDebug("Invalid array size: %u", cmd.count);
      return GetEnvironment()->NewIntArray(env, 0);
    }

    jintArray array = GetEnvironment()->NewIntArray(env, array_size);
    GetEnvironment()->SetIntArrayRegion(env, array, 0, array_size,
                                        (const jint *)(cmd.uids));

    return array;
  }

  return GetEnvironment()->NewIntArray(env, 0);
}

NativeBridgeNP(getSuperuserCount, jint) { return (jint)get_superuser_count(); }

NativeBridgeNP(isSafeMode, jboolean) { return is_safe_mode(); }

NativeBridgeNP(isManager, jboolean) { return is_manager(); }

NativeBridgeNP(isLateLoadMode, jboolean) { return is_late_load_mode(); }

NativeBridgeNP(isLkmBundled, jboolean) { return is_lkm_bundled(); }

NativeBridgeNP(isImagePatchMode, jboolean) { return is_image_patch_mode(); }

NativeBridgeNP(getLoadMode, jint) { return (jint)get_load_mode(); }

NativeBridgeNP(getSuPath, jbyteArray) {
  struct ksu_su_path_config config = {};
  if (ksu_kasumi_ioctl(KSU_IOCTL_GET_SU_PATH, &config) != 0 ||
      config.version != KSU_SU_PATH_VERSION || config.size != sizeof(config))
    return NULL;
  size_t length = strnlen(config.path, sizeof(config.path));
  if (length == sizeof(config.path))
    return NULL;
  jbyteArray result = GetEnvironment()->NewByteArray(env, (jsize)length);
  if (result)
    GetEnvironment()->SetByteArrayRegion(env, result, 0, (jsize)length,
                                         (const jbyte *)config.path);
  return result;
}

static void fillIntArray(JNIEnv *env, jobject list, int *data, int count) {
  jclass cls = GetEnvironment()->GetObjectClass(env, list);
  jmethodID add =
      GetEnvironment()->GetMethodID(env, cls, "add", "(Ljava/lang/Object;)Z");
  jclass integerCls = GetEnvironment()->FindClass(env, "java/lang/Integer");
  jmethodID constructor =
      GetEnvironment()->GetMethodID(env, integerCls, "<init>", "(I)V");
  for (int i = 0; i < count; ++i) {
    jobject integer =
        GetEnvironment()->NewObject(env, integerCls, constructor, data[i]);
    GetEnvironment()->CallBooleanMethod(env, list, add, integer);
  }
}

static void addIntToList(JNIEnv *env, jobject list, int ele) {
  jclass cls = GetEnvironment()->GetObjectClass(env, list);
  jmethodID add =
      GetEnvironment()->GetMethodID(env, cls, "add", "(Ljava/lang/Object;)Z");
  jclass integerCls = GetEnvironment()->FindClass(env, "java/lang/Integer");
  jmethodID constructor =
      GetEnvironment()->GetMethodID(env, integerCls, "<init>", "(I)V");
  jobject integer =
      GetEnvironment()->NewObject(env, integerCls, constructor, ele);
  GetEnvironment()->CallBooleanMethod(env, list, add, integer);
}

static uint64_t capListToBits(JNIEnv *env, jobject list) {
  jclass cls = GetEnvironment()->GetObjectClass(env, list);
  jmethodID get =
      GetEnvironment()->GetMethodID(env, cls, "get", "(I)Ljava/lang/Object;");
  jmethodID size = GetEnvironment()->GetMethodID(env, cls, "size", "()I");
  jint listSize = GetEnvironment()->CallIntMethod(env, list, size);
  jclass integerCls = GetEnvironment()->FindClass(env, "java/lang/Integer");
  jmethodID intValue =
      GetEnvironment()->GetMethodID(env, integerCls, "intValue", "()I");
  uint64_t result = 0;
  for (int i = 0; i < listSize; ++i) {
    jobject integer = GetEnvironment()->CallObjectMethod(env, list, get, i);
    int data = GetEnvironment()->CallIntMethod(env, integer, intValue);

    if (cap_valid(data)) {
      result |= (1ULL << data);
    }
  }

  return result;
}

static int getListSize(JNIEnv *env, jobject list) {
  jclass cls = GetEnvironment()->GetObjectClass(env, list);
  jmethodID size = GetEnvironment()->GetMethodID(env, cls, "size", "()I");
  return GetEnvironment()->CallIntMethod(env, list, size);
}

static void fillArrayWithList(JNIEnv *env, jobject list, int *data, int count) {
  jclass cls = GetEnvironment()->GetObjectClass(env, list);
  jmethodID get =
      GetEnvironment()->GetMethodID(env, cls, "get", "(I)Ljava/lang/Object;");
  jclass integerCls = GetEnvironment()->FindClass(env, "java/lang/Integer");
  jmethodID intValue =
      GetEnvironment()->GetMethodID(env, integerCls, "intValue", "()I");
  for (int i = 0; i < count; ++i) {
    jobject integer = GetEnvironment()->CallObjectMethod(env, list, get, i);
    data[i] = GetEnvironment()->CallIntMethod(env, integer, intValue);
  }
}

static jobject app_profile_to_java(JNIEnv *env, struct app_profile profile,
                                   bool useDefaultProfile) {
  jclass cls =
      GetEnvironment()->FindClass(env, "org/rouyashiki/yukisu/Natives$Profile");
  jmethodID constructor =
      GetEnvironment()->GetMethodID(env, cls, "<init>", "()V");
  jobject obj = GetEnvironment()->NewObject(env, cls, constructor);
  jfieldID keyField =
      GetEnvironment()->GetFieldID(env, cls, "name", "Ljava/lang/String;");
  jfieldID currentUidField =
      GetEnvironment()->GetFieldID(env, cls, "currentUid", "I");
  jfieldID allowSuField =
      GetEnvironment()->GetFieldID(env, cls, "allowSu", "Z");

  jfieldID rootUseDefaultField =
      GetEnvironment()->GetFieldID(env, cls, "rootUseDefault", "Z");
  jfieldID rootTemplateField = GetEnvironment()->GetFieldID(
      env, cls, "rootTemplate", "Ljava/lang/String;");

  jfieldID uidField = GetEnvironment()->GetFieldID(env, cls, "uid", "I");
  jfieldID gidField = GetEnvironment()->GetFieldID(env, cls, "gid", "I");
  jfieldID groupsField =
      GetEnvironment()->GetFieldID(env, cls, "groups", "Ljava/util/List;");
  jfieldID capabilitiesField = GetEnvironment()->GetFieldID(
      env, cls, "capabilities", "Ljava/util/List;");
  jfieldID domainField =
      GetEnvironment()->GetFieldID(env, cls, "context", "Ljava/lang/String;");
  jfieldID namespacesField =
      GetEnvironment()->GetFieldID(env, cls, "namespace", "I");

  jfieldID nonRootUseDefaultField =
      GetEnvironment()->GetFieldID(env, cls, "nonRootUseDefault", "Z");
  jfieldID umountModulesField =
      GetEnvironment()->GetFieldID(env, cls, "umountModules", "Z");

  GetEnvironment()->SetObjectField(
      env, obj, keyField, GetEnvironment()->NewStringUTF(env, profile.key));
  GetEnvironment()->SetIntField(env, obj, currentUidField, profile.curr_uid);

  if (useDefaultProfile) {
    // no profile found, so just use default profile:
    // don't allow root and use default profile!
    LogDebug("use default profile for: %s, %d", profile.key, profile.curr_uid);

    // allow_su = false
    // non root use default = true
    GetEnvironment()->SetBooleanField(env, obj, allowSuField, false);
    GetEnvironment()->SetBooleanField(env, obj, nonRootUseDefaultField, true);

    return obj;
  }

  bool allowSu = profile.allow_su;

  if (allowSu) {
    GetEnvironment()->SetBooleanField(env, obj, rootUseDefaultField,
                                      (jboolean)profile.rp_config.use_default);
    if (strlen(profile.rp_config.template_name) > 0) {
      GetEnvironment()->SetObjectField(
          env, obj, rootTemplateField,
          GetEnvironment()->NewStringUTF(env, profile.rp_config.template_name));
    }

    GetEnvironment()->SetIntField(env, obj, uidField,
                                  profile.rp_config.profile.uid);
    GetEnvironment()->SetIntField(env, obj, gidField,
                                  profile.rp_config.profile.gid);

    jobject groupList = GetEnvironment()->GetObjectField(env, obj, groupsField);
    int groupCount = profile.rp_config.profile.groups_count;
    if (groupCount > KSU_MAX_GROUPS) {
      LogDebug("kernel group count too large: %d???", groupCount);
      groupCount = KSU_MAX_GROUPS;
    }
    fillIntArray(env, groupList, profile.rp_config.profile.groups, groupCount);

    jobject capList =
        GetEnvironment()->GetObjectField(env, obj, capabilitiesField);
    for (int i = 0; i <= CAP_LAST_CAP; i++) {
      if (profile.rp_config.profile.capabilities.effective & (1ULL << i)) {
        addIntToList(env, capList, i);
      }
    }

    const char *extra_caps[] = {"capabilitiesPermitted",
                                "capabilitiesInheritable"};
    const uint64_t extra_bits[] = {
        profile.rp_config.profile.capabilities.permitted,
        profile.rp_config.profile.capabilities.inheritable};
    for (size_t c = 0; c < 2; ++c) {
      jobject list = GetEnvironment()->GetObjectField(
          env, obj,
          GetEnvironment()->GetFieldID(env, cls, extra_caps[c],
                                       "Ljava/util/List;"));
      for (int i = 0; i <= CAP_LAST_CAP; ++i)
        if (extra_bits[c] & (1ULL << i))
          addIntToList(env, list, i);
    }

    // Apps on the default root profile report an empty selinux_domain (the
    // kernel zeros rp_config for use_default). Surface the default ksu domain
    // so switching such an app to a custom profile carries a valid, non-empty
    // domain instead of being rejected by the kernel's profile_valid.
    const char *sel_domain = profile.rp_config.profile.selinux_domain;
    GetEnvironment()->SetObjectField(
        env, obj, domainField,
        GetEnvironment()->NewStringUTF(
            env, sel_domain[0] != '\0' ? sel_domain : "u:r:ksu:s0"));
    GetEnvironment()->SetIntField(env, obj, namespacesField,
                                  profile.rp_config.profile.namespaces);
    GetEnvironment()->SetLongField(
        env, obj, GetEnvironment()->GetFieldID(env, cls, "flags", "J"),
        (jlong)profile.rp_config.profile.flags);
    GetEnvironment()->SetBooleanField(env, obj, allowSuField, profile.allow_su);
  } else {
    GetEnvironment()->SetBooleanField(env, obj, nonRootUseDefaultField,
                                      profile.nrp_config.use_default);
    GetEnvironment()->SetBooleanField(
        env, obj, umountModulesField,
        profile.nrp_config.profile.umount_modules);
  }

  return obj;
}

NativeBridge(getAppProfile, jobject, jstring pkg, jint uid) {
  if (!pkg ||
      GetEnvironment()->GetStringUTFLength(env, pkg) >= KSU_MAX_PACKAGE_NAME) {
    return NULL;
  }

  char key[KSU_MAX_PACKAGE_NAME] = {0};
  const char *cpkg = GetEnvironment()->GetStringUTFChars(env, pkg, nullptr);
  strcpy(key, cpkg);
  GetEnvironment()->ReleaseStringUTFChars(env, pkg, cpkg);

  struct app_profile profile = {0};
  profile.version = KSU_APP_PROFILE_VER;

  strcpy(profile.key, key);
  profile.curr_uid = uid;

  bool useDefaultProfile = get_app_profile(&profile) != 0;

  return app_profile_to_java(env, profile, useDefaultProfile);
}

static void profile_read_error(JNIEnv *env, const char *message) {
  jclass type = GetEnvironment()->FindClass(env, "java/io/IOException");
  if (type)
    GetEnvironment()->ThrowNew(env, type, message);
}

NativeBridge(getProfileUids, jintArray, jboolean allow) {
  const size_t capacity = UINT16_MAX;
  struct ksu_new_get_allow_list_cmd *cmd =
      calloc(1, sizeof(*cmd) + (capacity * sizeof(__u32)));
  if (!cmd) {
    profile_read_error(env, "Cannot allocate profile list");
    return NULL;
  }
  cmd->count = capacity;
  int result = ksu_kasumi_ioctl(
      allow ? KSU_IOCTL_NEW_GET_ALLOW_LIST : KSU_IOCTL_NEW_GET_DENY_LIST, cmd);
  jintArray array = NULL;
  if (result || cmd->count != cmd->total_count) {
    profile_read_error(env, "Cannot read complete profile list");
  } else {
    array = GetEnvironment()->NewIntArray(env, cmd->count);
    if (array)
      GetEnvironment()->SetIntArrayRegion(env, array, 0, cmd->count,
                                          (const jint *)cmd->uids);
  }
  free(cmd);
  return array;
}

NativeBridge(readAppProfile, jobject, jint uid) {
  struct ksu_get_app_profile_cmd cmd = {};
  cmd.profile.curr_uid = uid;
  if (ksu_kasumi_ioctl(KSU_IOCTL_GET_APP_PROFILE, &cmd) != 0) {
    if (errno != ENOENT)
      profile_read_error(env, "Cannot read app profile");
    return NULL;
  }
  const struct app_profile *p = &cmd.profile;
  const uint64_t supported_caps = (1ULL << (CAP_LAST_CAP + 1)) - 1;
  if (p->allow_su && ((p->rp_config.profile.capabilities.effective |
                       p->rp_config.profile.capabilities.permitted |
                       p->rp_config.profile.capabilities.inheritable) &
                      ~supported_caps)) {
    profile_read_error(env, "Unsupported capability bits in app profile");
    return NULL;
  }
  if (p->version != KSU_APP_PROFILE_VER ||
      strnlen(p->key, sizeof(p->key)) == sizeof(p->key) ||
      (p->allow_su && (p->rp_config.profile.groups_count > KSU_MAX_GROUPS ||
                       strnlen(p->rp_config.template_name,
                               KSU_MAX_PACKAGE_NAME) == KSU_MAX_PACKAGE_NAME ||
                       strnlen(p->rp_config.profile.selinux_domain,
                               KSU_SELINUX_DOMAIN) == KSU_SELINUX_DOMAIN))) {
    profile_read_error(env, "Invalid kernel app profile");
    return NULL;
  }
  return app_profile_to_java(env, cmd.profile, false);
}

NativeBridgeNP(getProtectedProfileAppIds, jintArray) {
  struct ksu_dynamic_manager_app apps[KSU_DYNAMIC_MANAGER_MAX_APPS] = {};
  struct ksu_get_dynamic_managers_cmd cmd = {};
  cmd.count = KSU_DYNAMIC_MANAGER_MAX_APPS;
  cmd.apps = (uint64_t)(uintptr_t)apps;
  if (ksu_kasumi_ioctl(KSU_IOCTL_GET_DYNAMIC_MANAGERS, &cmd) != 0 ||
      cmd.count > KSU_DYNAMIC_MANAGER_MAX_APPS ||
      cmd.count != cmd.total_count) {
    profile_read_error(env, "Cannot verify protected manager UIDs");
    return NULL;
  }
  jint ids[KSU_DYNAMIC_MANAGER_MAX_APPS] = {};
  for (uint32_t i = 0; i < cmd.count; ++i)
    ids[i] = apps[i].appid;
  jintArray result = GetEnvironment()->NewIntArray(env, cmd.count);
  if (result)
    GetEnvironment()->SetIntArrayRegion(env, result, 0, cmd.count, ids);
  return result;
}

NativeBridge(setAppProfile, jboolean, jobject profile) {
  jclass cls =
      GetEnvironment()->FindClass(env, "org/rouyashiki/yukisu/Natives$Profile");

  jfieldID keyField =
      GetEnvironment()->GetFieldID(env, cls, "name", "Ljava/lang/String;");
  jfieldID currentUidField =
      GetEnvironment()->GetFieldID(env, cls, "currentUid", "I");
  jfieldID allowSuField =
      GetEnvironment()->GetFieldID(env, cls, "allowSu", "Z");

  jfieldID rootUseDefaultField =
      GetEnvironment()->GetFieldID(env, cls, "rootUseDefault", "Z");
  jfieldID rootTemplateField = GetEnvironment()->GetFieldID(
      env, cls, "rootTemplate", "Ljava/lang/String;");

  jfieldID uidField = GetEnvironment()->GetFieldID(env, cls, "uid", "I");
  jfieldID gidField = GetEnvironment()->GetFieldID(env, cls, "gid", "I");
  jfieldID groupsField =
      GetEnvironment()->GetFieldID(env, cls, "groups", "Ljava/util/List;");
  jfieldID capabilitiesField = GetEnvironment()->GetFieldID(
      env, cls, "capabilities", "Ljava/util/List;");
  jfieldID domainField =
      GetEnvironment()->GetFieldID(env, cls, "context", "Ljava/lang/String;");
  jfieldID namespacesField =
      GetEnvironment()->GetFieldID(env, cls, "namespace", "I");

  jfieldID nonRootUseDefaultField =
      GetEnvironment()->GetFieldID(env, cls, "nonRootUseDefault", "Z");
  jfieldID umountModulesField =
      GetEnvironment()->GetFieldID(env, cls, "umountModules", "Z");

  jobject key = GetEnvironment()->GetObjectField(env, profile, keyField);
  if (!key) {
    return false;
  }
  if (GetEnvironment()->GetStringUTFLength(env, (jstring)key) >=
      KSU_MAX_PACKAGE_NAME) {
    return false;
  }

  const char *cpkg =
      GetEnvironment()->GetStringUTFChars(env, (jstring)key, nullptr);
  char p_key[KSU_MAX_PACKAGE_NAME] = {0};
  strcpy(p_key, cpkg);
  GetEnvironment()->ReleaseStringUTFChars(env, (jstring)key, cpkg);

  jint currentUid =
      GetEnvironment()->GetIntField(env, profile, currentUidField);

  jint uid = GetEnvironment()->GetIntField(env, profile, uidField);
  jint gid = GetEnvironment()->GetIntField(env, profile, gidField);
  jobject groups = GetEnvironment()->GetObjectField(env, profile, groupsField);
  jobject capabilities =
      GetEnvironment()->GetObjectField(env, profile, capabilitiesField);
  jobject domain = GetEnvironment()->GetObjectField(env, profile, domainField);
  jboolean allowSu =
      GetEnvironment()->GetBooleanField(env, profile, allowSuField);
  jboolean umountModules =
      GetEnvironment()->GetBooleanField(env, profile, umountModulesField);

  struct app_profile p = {0};
  p.version = KSU_APP_PROFILE_VER;

  strcpy(p.key, p_key);
  p.allow_su = allowSu;
  p.curr_uid = currentUid;

  if (allowSu) {
    p.rp_config.use_default =
        GetEnvironment()->GetBooleanField(env, profile, rootUseDefaultField);
    jobject templateName =
        GetEnvironment()->GetObjectField(env, profile, rootTemplateField);
    if (templateName) {
      if (GetEnvironment()->GetStringUTFLength(env, (jstring)templateName) >=
          KSU_MAX_PACKAGE_NAME)
        return false;
      const char *ctemplateName = GetEnvironment()->GetStringUTFChars(
          env, (jstring)templateName, nullptr);
      strcpy(p.rp_config.template_name, ctemplateName);
      GetEnvironment()->ReleaseStringUTFChars(env, (jstring)templateName,
                                              ctemplateName);
    }

    p.rp_config.profile.uid = uid;
    p.rp_config.profile.gid = gid;

    int groups_count = getListSize(env, groups);
    if (groups_count > KSU_MAX_GROUPS) {
      LogDebug("groups count too large: %d", groups_count);
      return false;
    }
    p.rp_config.profile.groups_count = groups_count;
    fillArrayWithList(env, groups, p.rp_config.profile.groups, groups_count);

    p.rp_config.profile.capabilities.effective =
        capListToBits(env, capabilities);
    p.rp_config.profile.capabilities.permitted = capListToBits(
        env, GetEnvironment()->GetObjectField(
                 env, profile,
                 GetEnvironment()->GetFieldID(env, cls, "capabilitiesPermitted",
                                              "Ljava/util/List;")));
    p.rp_config.profile.capabilities.inheritable = capListToBits(
        env, GetEnvironment()->GetObjectField(
                 env, profile,
                 GetEnvironment()->GetFieldID(
                     env, cls, "capabilitiesInheritable", "Ljava/util/List;")));

    if (!domain || GetEnvironment()->GetStringUTFLength(env, (jstring)domain) >=
                       KSU_SELINUX_DOMAIN)
      return false;
    const char *cdomain =
        GetEnvironment()->GetStringUTFChars(env, (jstring)domain, nullptr);
    strcpy(p.rp_config.profile.selinux_domain, cdomain);
    GetEnvironment()->ReleaseStringUTFChars(env, (jstring)domain, cdomain);
    // A custom root profile must carry a non-empty SELinux domain or the kernel
    // rejects it (profile_valid). Fall back to the default ksu domain.
    if (!p.rp_config.use_default &&
        p.rp_config.profile.selinux_domain[0] == '\0') {
      strcpy(p.rp_config.profile.selinux_domain, "u:r:ksu:s0");
    }

    p.rp_config.profile.namespaces =
        GetEnvironment()->GetIntField(env, profile, namespacesField);
    p.rp_config.profile.flags = (uint64_t)GetEnvironment()->GetLongField(
        env, profile, GetEnvironment()->GetFieldID(env, cls, "flags", "J"));
  } else {
    p.nrp_config.use_default =
        GetEnvironment()->GetBooleanField(env, profile, nonRootUseDefaultField);
    p.nrp_config.profile.umount_modules = umountModules;
  }

  return set_app_profile(&p);
}

NativeBridge(uidShouldUmount, jboolean, jint uid) {
  return uid_should_umount(uid);
}

NativeBridgeNP(getDynamicManagers, jintArray) {
  struct ksu_dynamic_manager_app apps[KSU_DYNAMIC_MANAGER_MAX_APPS] = {};
  uint32_t count = get_dynamic_managers(apps, KSU_DYNAMIC_MANAGER_MAX_APPS);
  jsize array_size = (jsize)(count * 2);
  jintArray array = GetEnvironment()->NewIntArray(env, array_size);

  if (!array || !count) {
    return array;
  }

  jint flattened[KSU_DYNAMIC_MANAGER_MAX_APPS * 2] = {};
  for (uint32_t i = 0; i < count; i++) {
    const size_t offset = (size_t)i * 2;
    flattened[offset] = (jint)apps[i].appid;
    flattened[offset + 1] = (jint)apps[i].flags;
  }

  GetEnvironment()->SetIntArrayRegion(env, array, 0, array_size, flattened);
  return array;
}

NativeBridgeNP(isSuEnabled, jboolean) { return is_su_enabled(); }

NativeBridge(setSuEnabled, jboolean, jboolean enabled) {
  return set_su_enabled(enabled);
}

NativeBridgeNP(isMagiskCompatEnabled, jboolean) {
  return is_magisk_compat_enabled();
}

NativeBridge(setMagiskCompatEnabled, jboolean, jboolean enabled) {
  return set_magisk_compat_enabled(enabled);
}

NativeBridge(submitSuPrompt, jboolean, jlong request_id, jlong nonce,
             jint choice, jstring package_name) {
  const char *package_chars = "";
  if (package_name) {
    if (GetEnvironment()->GetStringUTFLength(env, package_name) >=
        KSU_MAX_PACKAGE_NAME) {
      return false;
    }
    package_chars =
        GetEnvironment()->GetStringUTFChars(env, package_name, nullptr);
    if (!package_chars) {
      return false;
    }
  }

  bool result = submit_su_prompt((uint64_t)request_id, (uint64_t)nonce,
                                 (uint32_t)choice, package_chars);
  if (package_name) {
    GetEnvironment()->ReleaseStringUTFChars(env, package_name, package_chars);
  }
  return result;
}

NativeBridge(suPromptReady, jint, jlong request_id, jlong nonce) {
  return su_prompt_ready((uint64_t)request_id, (uint64_t)nonce);
}

NativeBridgeNP(isKernelUmountEnabled, jboolean) {
  return is_kernel_umount_enabled();
}

NativeBridge(setKernelUmountEnabled, jboolean, jboolean enabled) {
  return set_kernel_umount_enabled(enabled);
}

NativeBridgeNP(isSuLogEnabled, jboolean) { return is_sulog_enabled(); }

NativeBridge(setSuLogEnabled, jboolean, jboolean enabled) {
  return set_sulog_enabled(enabled);
}

NativeBridgeNP(isAdbRootEnabled, jboolean) { return is_adb_root_enabled(); }

NativeBridge(setAdbRootEnabled, jboolean, jboolean enabled) {
  return set_adb_root_enabled(enabled);
}

NativeBridgeNP(isSelinuxHideEnabled, jboolean) {
  return is_selinux_hide_enabled();
}

NativeBridge(setSelinuxHideEnabled, jboolean, jboolean enabled) {
  return set_selinux_hide_enabled(enabled);
}

NativeBridgeNP(isDefaultNoNewPrivsEnabled, jboolean) {
  return is_default_no_new_privs_enabled();
}

NativeBridge(setDefaultNoNewPrivsEnabled, jboolean, jboolean enabled) {
  return set_default_no_new_privs_enabled(enabled);
}

NativeBridgeNP(isHideBootloaderEnabled, jboolean) {
  return is_hide_bootloader_enabled();
}

NativeBridge(setHideBootloaderEnabled, jboolean, jboolean enabled) {
  return set_hide_bootloader_enabled(enabled);
}

NativeBridge(getFeatureFlags, jint, jint id) {
  return (jint)query_feature_flags((uint32_t)id);
}

NativeBridge(getFeature, jlong, jint id) {
  return (jlong)query_feature((uint32_t)id);
}

NativeBridge(getUserName, jstring, jint uid) {
  struct passwd *pw = getpwuid((uid_t)uid);
  if (pw && pw->pw_name && pw->pw_name[0] != '\0') {
    return GetEnvironment()->NewStringUTF(env, pw->pw_name);
  }
  return NULL;
}

// Get HOOK type
NativeBridgeNP(getHookType, jstring) {
  char hook_type[32] = {0};
  get_hook_type((char *)&hook_type);
  return GetEnvironment()->NewStringUTF(env, hook_type);
}

// SuperKey authentication
NativeBridge(authenticateSuperKey, jboolean, jstring superKey) {
  if (!superKey) {
    LogDebug("authenticateSuperKey: superKey is null");
    return false;
  }

  const char *cSuperKey =
      GetEnvironment()->GetStringUTFChars(env, superKey, nullptr);
  bool result = authenticate_superkey(cSuperKey);
  GetEnvironment()->ReleaseStringUTFChars(env, superKey, cSuperKey);

  LogDebug("authenticateSuperKey: result=%d", result);
  return result;
}

// Check if KSU driver is present
NativeBridgeNP(isKsuDriverPresent, jboolean) { return ksu_driver_present(); }

// Check if SuperKey is configured in kernel
NativeBridgeNP(isSuperKeyConfigured, jboolean) {
  return is_superkey_configured();
}

// Check if already authenticated via SuperKey
NativeBridgeNP(isSuperKeyAuthenticated, jboolean) {
  return is_superkey_authenticated();
}

// Check if manager signature is considered OK
NativeBridgeNP(isSignatureOk, jboolean) { return is_signature_ok(); }

static int wait_child_exit(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    LogDebug("waitpid failed: %s", strerror(errno));
    return -1;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    LogDebug("magica bootstrap child failed, status=%d", status);
  }
  return status;
}

static int fork_dont_care_and_exec_ksud(const char *path) {
  pid_t pid = fork();
  if (pid < 0) {
    LogDebug("fork failed: %s", strerror(errno));
    return -1;
  }

  if (pid > 0) {
    wait_child_exit(pid);
    return (int)pid;
  }

  if (setuid(0) != 0) {
    LogDebug("setuid failed: %s", strerror(errno));
    _exit(1);
  }

  pid = fork();
  if (pid < 0) {
    LogDebug("fork(2) failed: %s", strerror(errno));
    _exit(1);
  }

  if (pid > 0) {
    _exit(0);
  }

  execl(path, "ksud", "late-load", "--magica", "5555", NULL);
  LogDebug("exec failed: %s", strerror(errno));
  _exit(1);
}

JNIEXPORT void JNICALL
Java_org_rouyashiki_yukisu_magica_AppZygotePreload_forkDontCareAndExecKsud(
    JNIEnv *env, jclass clazz, jstring ksud_path) {
  (void)clazz;
  if (!ksud_path) {
    return;
  }

  const char *path = (*env)->GetStringUTFChars(env, ksud_path, nullptr);
  if (!path) {
    return;
  }

  LogDebug("executing magica %s", path);
  fork_dont_care_and_exec_ksud(path);
  (*env)->ReleaseStringUTFChars(env, ksud_path, path);
}
