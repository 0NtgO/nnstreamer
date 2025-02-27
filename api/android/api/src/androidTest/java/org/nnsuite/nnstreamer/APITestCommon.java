package org.nnsuite.nnstreamer;

import android.content.Context;
import android.os.Environment;
import android.support.test.InstrumentationRegistry;
import android.support.test.runner.AndroidJUnit4;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import static org.junit.Assert.*;

/**
 * Common definition to test NNStreamer API.
 *
 * Instrumented test, which will execute on an Android device.
 *
 * @see <a href="http://d.android.com/tools/testing">Testing documentation</a>
 */
@RunWith(AndroidJUnit4.class)
public class APITestCommon {
    private static boolean mInitialized = false;

    /**
     * Initializes NNStreamer API library.
     */
    public static void initNNStreamer() {
        if (!mInitialized) {
            try {
                Context context = InstrumentationRegistry.getTargetContext();
                mInitialized = NNStreamer.initialize(context);
            } catch (Exception e) {
                fail();
            }
        }
    }

    /**
     * Gets the File object of tensorflow-lite image classification model.
     * Note that, to invoke tensorflow-lite model in the storage, the permission READ_EXTERNAL_STORAGE is required.
     */
    public static File getTestModel() {
        String root = Environment.getExternalStorageDirectory().getAbsolutePath();
        File model = new File(root + "/nnstreamer/test/mobilenet_v1_1.0_224_quant.tflite");

        if (!model.exists()) {
            fail();
        }

        return model;
    }

    /**
     * Verifies the byte buffer is direct buffer with native order.
     *
     * @param buffer   The byte buffer
     * @param expected The expected capacity
     *
     * @return True if the byte buffer is valid.
     */
    public static boolean isValidBuffer(ByteBuffer buffer, int expected) {
        if (buffer != null && buffer.isDirect() && buffer.order() == ByteOrder.nativeOrder()) {
            int capacity = buffer.capacity();

            return (capacity == expected);
        }

        return false;
    }

    @Test
    public void useAppContext() {
        Context context = InstrumentationRegistry.getTargetContext();

        assertEquals("org.nnsuite.nnstreamer.test", context.getPackageName());
    }
}
