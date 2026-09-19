// IBridgeBinder.aidl
package io.github.lumaparallax.vsouchi;

import android.os.ParcelFileDescriptor;

interface IBridgeBinder {
    ParcelFileDescriptor socketPairFD();
}
