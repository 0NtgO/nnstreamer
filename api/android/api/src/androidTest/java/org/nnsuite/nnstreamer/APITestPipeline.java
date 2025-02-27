package org.nnsuite.nnstreamer;

import android.Manifest;
import android.support.test.rule.GrantPermissionRule;
import android.support.test.runner.AndroidJUnit4;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.nio.ByteBuffer;
import java.util.Arrays;

import static org.junit.Assert.*;

/**
 * Testcases for Pipeline.
 */
@RunWith(AndroidJUnit4.class)
public class APITestPipeline {
    private int mReceived = 0;
    private boolean mInvalidState = false;
    private int mPipelineState = NNStreamer.PIPELINE_STATE_NULL;

    private Pipeline.NewDataCallback mSinkCb = new Pipeline.NewDataCallback() {
        @Override
        public void onNewDataReceived(TensorsData data, TensorsInfo info) {
            /* validate received data (unit8 2:10:10:1) */
            if (data == null || data.getTensorsCount() != 1 ||
                data.getTensorData(0).capacity() != 200 ||
                info == null || info.getTensorsCount() != 1 ||
                info.getTensorName(0) != null ||
                info.getTensorType(0) != NNStreamer.TENSOR_TYPE_UINT8 ||
                !Arrays.equals(info.getTensorDimension(0), new int[]{2,10,10,1})) {
                /* received data is invalid */
                mInvalidState = true;
            }

            mReceived++;
        }
    };

    @Rule
    public GrantPermissionRule mPermissionRule =
            GrantPermissionRule.grant(Manifest.permission.READ_EXTERNAL_STORAGE);

    @Before
    public void setUp() {
        APITestCommon.initNNStreamer();

        mReceived = 0;
        mInvalidState = false;
        mPipelineState = NNStreamer.PIPELINE_STATE_NULL;
    }

    @Test
    public void testConstructInvalidElement() {
        String desc = "videotestsrc ! videoconvert ! video/x-raw,format=RGB ! " +
                "invalidelement ! tensor_converter ! tensor_sink";

        try {
            new Pipeline(desc);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testConstructNullDescription() {
        try {
            new Pipeline(null);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testConstructNullStateCb() {
        String desc = "videotestsrc ! videoconvert ! video/x-raw,format=RGB ! " +
                "tensor_converter ! tensor_sink";

        try (Pipeline pipe = new Pipeline(desc, null)) {
            Thread.sleep(100);
            assertEquals(NNStreamer.PIPELINE_STATE_PAUSED, pipe.getState());
            Thread.sleep(100);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testConstructWithStateCb() {
        String desc = "videotestsrc ! videoconvert ! video/x-raw,format=RGB ! " +
                "tensor_converter ! tensor_sink";

        /* pipeline state callback */
        Pipeline.StateChangeCallback stateCb = new Pipeline.StateChangeCallback() {
            @Override
            public void onStateChanged(int state) {
                mPipelineState = state;
            }
        };

        try (Pipeline pipe = new Pipeline(desc, stateCb)) {
            Thread.sleep(100);
            assertEquals(NNStreamer.PIPELINE_STATE_PAUSED, mPipelineState);

            /* start pipeline */
            pipe.start();
            Thread.sleep(300);

            assertEquals(NNStreamer.PIPELINE_STATE_PLAYING, mPipelineState);

            /* stop pipeline */
            pipe.stop();
            Thread.sleep(300);

            assertEquals(NNStreamer.PIPELINE_STATE_PAUSED, mPipelineState);
            Thread.sleep(100);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testGetState() {
        String desc = "videotestsrc ! videoconvert ! video/x-raw,format=RGB ! " +
                "tensor_converter ! tensor_sink";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* start pipeline */
            pipe.start();
            Thread.sleep(300);

            assertEquals(NNStreamer.PIPELINE_STATE_PLAYING, pipe.getState());

            /* stop pipeline */
            pipe.stop();
            Thread.sleep(300);

            assertEquals(NNStreamer.PIPELINE_STATE_PAUSED, pipe.getState());
            Thread.sleep(100);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testSetNullDataCb() {
        String desc = "videotestsrc ! videoconvert ! video/x-raw,format=RGB ! " +
                "tensor_converter ! tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            pipe.setSinkCallback("sinkx", null);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testSetDataCbInvalidName() {
        String desc = "videotestsrc ! videoconvert ! video/x-raw,format=RGB ! " +
                "tensor_converter ! tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            pipe.setSinkCallback("invalid_sink", mSinkCb);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testSetDataCbNullName() {
        String desc = "videotestsrc ! videoconvert ! video/x-raw,format=RGB ! " +
                "tensor_converter ! tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            pipe.setSinkCallback(null, mSinkCb);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testRemoveDataCb() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* register sink callback */
            pipe.setSinkCallback("sinkx", mSinkCb);

            /* start pipeline */
            pipe.start();

            /* push input buffer */
            for (int i = 0; i < 10; i++) {
                /* dummy input */
                TensorsData in = new TensorsData();
                in.addTensorData(TensorsData.allocateByteBuffer(200));

                pipe.inputData("srcx", in);
                Thread.sleep(50);
            }

            /* pause pipeline and unregister sink callback */
            Thread.sleep(100);
            pipe.stop();

            pipe.setSinkCallback("sinkx", null);
            Thread.sleep(100);

            /* start pipeline again */
            pipe.start();

            /* push input buffer again */
            for (int i = 0; i < 10; i++) {
                /* dummy input */
                TensorsData in = new TensorsData();
                in.addTensorData(TensorsData.allocateByteBuffer(200));

                pipe.inputData("srcx", in);
                Thread.sleep(50);
            }

            /* check received data from sink */
            assertFalse(mInvalidState);
            assertEquals(10, mReceived);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testRunModel() {
        File model = APITestCommon.getTestModel();
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)3:224:224:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "tensor_filter framework=tensorflow-lite model=" + model.getAbsolutePath() + " ! " +
                "tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* register sink callback */
            pipe.setSinkCallback("sinkx", new Pipeline.NewDataCallback() {
                @Override
                public void onNewDataReceived(TensorsData data, TensorsInfo info) {
                    if (data == null || data.getTensorsCount() != 1 ||
                        info == null || info.getTensorsCount() != 1) {
                        mInvalidState = true;
                    } else {
                        ByteBuffer output = data.getTensorData(0);

                        if (!APITestCommon.isValidBuffer(output, 1001)) {
                            mInvalidState = true;
                        }
                    }

                    mReceived++;
                }
            });

            /* start pipeline */
            pipe.start();

            /* push input buffer */
            for (int i = 0; i < 15; i++) {
                /* dummy input */
                TensorsData in = new TensorsData();
                in.addTensorData(TensorsData.allocateByteBuffer(3 * 224 * 224));

                pipe.inputData("srcx", in);
                Thread.sleep(100);
            }

            /* sleep 500 to invoke */
            Thread.sleep(500);

            /* check received data from sink */
            assertFalse(mInvalidState);
            assertTrue(mReceived > 0);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testInputInvalidName() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* start pipeline */
            pipe.start();

            TensorsData in = new TensorsData();
            in.addTensorData(TensorsData.allocateByteBuffer(200));

            pipe.inputData("invalid_src", in);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testInputNullName() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* start pipeline */
            pipe.start();

            TensorsData in = new TensorsData();
            in.addTensorData(TensorsData.allocateByteBuffer(200));

            pipe.inputData(null, in);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testInputNullData() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* start pipeline */
            pipe.start();

            pipe.inputData("srcx", null);
            fail();
        } catch (Exception e) {
            /* expected */
        }
    }

    @Test
    public void testSelectSwitch() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "output-selector name=outs " +
                "outs.src_0 ! tensor_sink name=sinkx async=false " +
                "outs.src_1 ! tensor_sink async=false";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* register sink callback */
            pipe.setSinkCallback("sinkx", mSinkCb);

            /* start pipeline */
            pipe.start();

            /* push input buffer */
            for (int i = 0; i < 15; i++) {
                /* dummy input */
                TensorsData in = new TensorsData();
                in.addTensorData(TensorsData.allocateByteBuffer(200));

                pipe.inputData("srcx", in);
                Thread.sleep(50);

                if (i == 9) {
                    /* select pad */
                    pipe.selectSwitchPad("outs", "src_1");
                }
            }

            /* sleep 300 to pass all input buffers to sink */
            Thread.sleep(300);

            /* check received data from sink */
            assertFalse(mInvalidState);
            assertEquals(10, mReceived);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testGetSwitchPad() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "output-selector name=outs " +
                "outs.src_0 ! tensor_sink name=sinkx async=false " +
                "outs.src_1 ! tensor_sink async=false";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* start pipeline */
            pipe.start();

            /* get pad list of output-selector */
            String[] pads = pipe.getSwitchPads("outs");

            assertEquals(2, pads.length);
            assertEquals("src_0", pads[0]);
            assertEquals("src_1", pads[1]);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testGetSwitchInvalidName() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "output-selector name=outs " +
                "outs.src_0 ! tensor_sink name=sinkx async=false " +
                "outs.src_1 ! tensor_sink async=false";

        Pipeline pipe = new Pipeline(desc);

        /* start pipeline */
        pipe.start();

        try {
            /* get pad list with invalid switch name */
            pipe.getSwitchPads("invalid_outs");
            fail();
        } catch (Exception e) {
            /* expected */
        }

        pipe.close();
    }

    @Test
    public void testGetSwitchNullName() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "output-selector name=outs " +
                "outs.src_0 ! tensor_sink name=sinkx async=false " +
                "outs.src_1 ! tensor_sink async=false";

        Pipeline pipe = new Pipeline(desc);

        /* start pipeline */
        pipe.start();

        try {
            /* get pad list with null param */
            pipe.getSwitchPads(null);
            fail();
        } catch (Exception e) {
            /* expected */
        }

        pipe.close();
    }

    @Test
    public void testSelectInvalidPad() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "output-selector name=outs " +
                "outs.src_0 ! tensor_sink name=sinkx async=false " +
                "outs.src_1 ! tensor_sink async=false";

        Pipeline pipe = new Pipeline(desc);

        /* start pipeline */
        pipe.start();

        try {
            /* select invalid pad name */
            pipe.selectSwitchPad("outs", "invalid_src");
            fail();
        } catch (Exception e) {
            /* expected */
        }

        pipe.close();
    }

    @Test
    public void testControlValve() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "tee name=t " +
                "t. ! queue ! tensor_sink " +
                "t. ! queue ! valve name=valvex ! tensor_sink name=sinkx";

        try (Pipeline pipe = new Pipeline(desc)) {
            /* register sink callback */
            pipe.setSinkCallback("sinkx", mSinkCb);

            /* start pipeline */
            pipe.start();

            /* push input buffer */
            for (int i = 0; i < 15; i++) {
                /* dummy input */
                TensorsData in = new TensorsData();
                in.addTensorData(TensorsData.allocateByteBuffer(200));

                pipe.inputData("srcx", in);
                Thread.sleep(50);

                if (i == 9) {
                    /* close valve */
                    pipe.controlValve("valvex", false);
                }
            }

            /* sleep 300 to pass all input buffers to sink */
            Thread.sleep(300);

            /* check received data from sink */
            assertFalse(mInvalidState);
            assertEquals(10, mReceived);
        } catch (Exception e) {
            fail();
        }
    }

    @Test
    public void testControlInvalidValve() {
        String desc = "appsrc name=srcx ! " +
                "other/tensor,dimension=(string)2:10:10:1,type=(string)uint8,framerate=(fraction)0/1 ! " +
                "tee name=t " +
                "t. ! queue ! tensor_sink " +
                "t. ! queue ! valve name=valvex ! tensor_sink name=sinkx";

        Pipeline pipe = new Pipeline(desc);

        /* start pipeline */
        pipe.start();

        try {
            /* control valve with invalid name */
            pipe.controlValve("invalid_valve", false);
            fail();
        } catch (Exception e) {
            /* expected */
        }

        pipe.close();
    }
}
