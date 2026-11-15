package org.rouyashiki.yukisu;

import android.content.IntentSender;
import android.os.ParcelFileDescriptor;

interface ICiUpdateInstaller {
    int createSession(long apkSize, String packageName);
    void writeSession(int sessionId, in ParcelFileDescriptor apkStream, long apkSize);
    void commitSession(int sessionId, in IntentSender statusReceiver);
    void abandonSession(int sessionId);
}
