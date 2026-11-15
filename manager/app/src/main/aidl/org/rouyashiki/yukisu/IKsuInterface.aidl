package org.rouyashiki.yukisu;

import android.content.pm.PackageInfo;
import org.rouyashiki.yukisu.BackupPackage;
import java.util.List;

interface IKsuInterface {
    int getPackageCount();
    List<PackageInfo> getPackages(int start, int maxCount);
    int refreshPackages();
    String[] getUidPackagesForBackup(int uid);
    String readProfileRulesForBackup(String packageName);
    boolean checkProfileRulesForBackup(String rules);
    boolean writeProfileRulesForBackup(String packageName, String rules);
    String beginBackupSnapshot();
    List<BackupPackage> getBackupPackages(String snapshot, int start, int count);
    List<BackupPackage> getBackupSignatures(String snapshot, in int[] indices);
    void closeBackupSnapshot(String snapshot);
    List<String> getBackupProfileRules(in String[] keys);
    List<BackupPackage> getBackupUidIdentity(int uid);
}
