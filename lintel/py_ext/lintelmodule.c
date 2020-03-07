/**
 * Copyright 2018 Brendan Duke.
 *
 * This file is part of Lintel.
 *
 * Lintel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Lintel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Lintel. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * Load video data.
 */
#define PY_SSIZE_T_CLEAN

#include "core/video_decode.h"
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <Python.h>
#include <pythread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>

#define UNUSED(x) x __attribute__ ((__unused__))

#define DEFAULT_TIMEOUT_SEC 3


#define LOADVID_TIMEOUT 1
#define LOADVID_SUCCESS 0
#define LOADVID_ERR (-1)
#define LOADVID_ERR_STREAM_INDEX (-2)

PyDoc_STRVAR(module_doc, "Module for loading video data.");

// callback
int32_t interrupt_callback(void *data) {
    struct video_stream_context *vid_ctx = data;
    if (vid_ctx->decode_time == 0) {
        vid_ctx->decode_time =  time(NULL); //start time
        return LOADVID_SUCCESS;
    } else {
        int64_t time_use =  time(NULL) - vid_ctx->decode_time;
        if (time_use > vid_ctx->timeout_sec) {// timeout
            vid_ctx->error_type = PyExc_TimeoutError;
            vid_ctx->error_msg = "decode video frame timeout.";
//             printf("decode video timeout.\n");
            return LOADVID_TIMEOUT;
        } else {
            return LOADVID_SUCCESS;
        }
    }
}

/**
 * Allocates a PyByteArrayObject, and `out_size_bytes` of buffer for that
 * object.
 *
 * If the memory allocation fails, the references will be cleaned up by this
 * function. If a reference to a PyByteArrayObject object is returned, that
 * reference is owned by the caller.
 */
static PyByteArrayObject *
alloc_pyarray(const uint32_t out_size_bytes)
{
        PyByteArrayObject *frames = PyObject_New(PyByteArrayObject,
                                                 &PyByteArray_Type);
        if (frames == NULL)
                return NULL;

        frames->ob_bytes = PyObject_Malloc(out_size_bytes);
        if (frames->ob_bytes == NULL) {
                Py_DECREF(frames);
                return (PyByteArrayObject *)PyErr_NoMemory();
        }

        Py_SIZE(frames) = out_size_bytes;
        frames->ob_alloc = out_size_bytes;
        frames->ob_start = frames->ob_bytes;
        frames->ob_exports = 0;

        return frames;
}

/**
 * setup_vid_stream_context() - Fills in the members of `vid_ctx` by allocating
 * and setting up FFmpeg contexts through libavformat and libavcodec.
 * @vid_ctx: Output video_stream_context to be filled in.
 * @input_buf: buffer_data structure injected into `vid_ctx`, which should have
 * the same lifetime as `vid_ctx`.
 *
 * LOADVID_ERR_STREAM_INDEX is returned if the video corresponding to
 * `input_buf`'s stream index was not found. For other errors, LOADVID_ERR is
 * returned. LOADVID_SUCCESS is returned on success.
 */
/* rewrite setup_vid_stream_context fun by read filename */
static int32_t
setup_vid_stream_context_filename(struct video_stream_context *vid_ctx,
                         const char *filename, int32_t timeout)
{
        vid_ctx->timeout_sec = timeout;
        vid_ctx->error_type = NULL;
        vid_ctx->decode_time = time(NULL);
    
        vid_ctx->format_context = avformat_alloc_context();
        if (vid_ctx->format_context == NULL) {
                vid_ctx->error_type = PyExc_IOError;
                vid_ctx->error_msg = "format context not found.";
                goto clean_up_format_context;        
        }

        
        vid_ctx->format_context->interrupt_callback.callback = interrupt_callback;
        vid_ctx->format_context->interrupt_callback.opaque = vid_ctx;


        char buf[1024];
        int32_t status = avformat_open_input(&vid_ctx->format_context, filename,
                                              NULL, NULL);
        if (status !=0 )
        {
                av_strerror(status, buf, 1024);
//                 printf("Cannot open the file, error code=%d, error message: %s\n", status, buf);
                vid_ctx->error_type = PyExc_IOError;
                vid_ctx->error_msg = buf;
                return LOADVID_ERR;
        }


        /*
        * Retrieve stream information
        */
        if (avformat_find_stream_info(vid_ctx->format_context, NULL) < 0) {
//                 printf("Stream index not found.\n");
                vid_ctx->error_type = PyExc_ValueError;
                vid_ctx->error_msg = "stream index not found.";
                goto clean_up_format_context;
        }

        /*
        * Detect streams types
        */
        uint32_t stream_index;
        AVStream *video_stream;
        for (stream_index = 0; stream_index < vid_ctx->format_context->nb_streams;
             ++stream_index)
        {
                video_stream = vid_ctx->format_context->streams[stream_index];

                if (video_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                        break;
        }
        if (stream_index >= vid_ctx->format_context->nb_streams) {
                vid_ctx->error_type = PyExc_IOError;
                vid_ctx->error_msg = "format context nb_streams not found.";
                return VID_DECODE_FFMPEG_ERR;
        }
        vid_ctx->video_stream_index = stream_index;

        video_stream = vid_ctx->format_context->streams[vid_ctx->video_stream_index];
        vid_ctx->codec_context = open_video_codec_ctx(video_stream);
        if (vid_ctx->codec_context == NULL) {
                vid_ctx->error_type = PyExc_IOError;
                vid_ctx->error_msg = "codec_context not found.";
                goto clean_up_format_context;
        }

        if (vid_ctx->codec_context->pix_fmt == AV_PIX_FMT_NONE) {
                vid_ctx->error_type = PyExc_IOError;
                vid_ctx->error_msg = "codec context AV_PIX_FMT_NONE error.";
                goto clean_up_format_context;
        }

        if ((video_stream->duration <= 0) || (video_stream->nb_frames <= 0)) {
                /**
                 * Some video containers (e.g., webm) contain indices of only
                 * frames-of-interest, e.g., keyframes, and therefore the whole
                 * file must be parsed to get the number of frames (nb_frames
                 * will be zero).
                 *
                 * Also, for webm only the duration of the entire file is
                 * specified in the header (as opposed to the stream duration),
                 * so the duration must be taken from the AVFormatContext, not
                 * the AVStream.
                 *
                 * See this SO answer: https://stackoverflow.com/a/32538549
                 */

                /**
                 * Compute nb_frames from fmt ctx duration (microseconds) and
                 * stream FPS (frames/second).
                 */
                // assert(video_stream->avg_frame_rate.den > 0);
                if (video_stream->avg_frame_rate.den <= 0) {
//                         printf("Read video frame rate error.");
                        vid_ctx->error_type = PyExc_ValueError;
                        vid_ctx->error_msg = "read video frame rate error.";
                        goto clean_up_format_context;
                }

                enum AVRounding rnd = (enum AVRounding)(AV_ROUND_DOWN |
                                                        AV_ROUND_PASS_MINMAX);
                int64_t fps_num = video_stream->avg_frame_rate.num;
                int64_t fps_den =
                        video_stream->avg_frame_rate.den*(int64_t)AV_TIME_BASE;
                vid_ctx->nb_frames =
                        av_rescale_rnd(vid_ctx->format_context->duration,
                                       fps_num,
                                       fps_den,
                                       rnd);

                /**
                 * NOTE(brendan): fmt ctx duration in microseconds =>
                 *
                 * fmt ctx duration == (stream duration)*(stream timebase)*1e6
                 *
                 * since stream timebase is in units of
                 * seconds / (stream timestamp). The rest of the code expects
                 * the duration in stream timestamps, so do the conversion
                 * here.
                 *
                 * Multiply the timebase numerator by AV_TIME_BASE to get a
                 * more accurate rounded duration by doing the rounding in the
                 * higher precision units.
                 */
                int64_t tb_num = video_stream->time_base.num*(int64_t)AV_TIME_BASE;
                int64_t tb_den = video_stream->time_base.den;
                vid_ctx->duration =
                        av_rescale_rnd(vid_ctx->format_context->duration,
                                       tb_den,
                                       tb_num,
                                       rnd);
        } else {
                vid_ctx->duration = video_stream->duration;
                vid_ctx->nb_frames = video_stream->nb_frames;
        }

        vid_ctx->frame = av_frame_alloc();
        if (vid_ctx->frame == NULL) {
                vid_ctx->error_type = PyExc_IOError;
                vid_ctx->error_msg = "vid_ctx frame not found";
                goto clean_up_avcodec;
        }

        return LOADVID_SUCCESS;

clean_up_avcodec:
        avcodec_close(vid_ctx->codec_context);
        avcodec_free_context(&vid_ctx->codec_context);
clean_up_format_context:
        avformat_close_input(&vid_ctx->format_context);

        return LOADVID_ERR;
}




static void
clean_up_vid_ctx(struct video_stream_context *vid_ctx)
{
        av_frame_free(&vid_ctx->frame);
        avcodec_close(vid_ctx->codec_context);
        avcodec_free_context(&vid_ctx->codec_context);
        // av_freep(&vid_ctx->format_context->pb->buffer);
        // av_freep(&vid_ctx->format_context->pb);
        avformat_close_input(&vid_ctx->format_context);
}

/**
 * get_vid_width_height() - Sets `width` and `height` dynamically based on the
 * video's `AVCodecContext` if they are not already set.
 * @width: In/out pointer to width (unchecked for NULL).
 * @height: In/out pointer to height (unchecked for NULL).
 * @codec_context: Already-opened video `AVCodecContext`.
 *
 * Returns true iff the size has been set dynamically.
 * Also, checks that the width/height matches the AVCodecContext regardless
 * (via assert).
 */
static bool
get_vid_width_height(uint32_t *width,
                     uint32_t *height,
                     struct video_stream_context *vid_ctx)
                     // AVCodecContext *codec_context)
{
        AVCodecContext *codec_context = vid_ctx->codec_context;
        /* NOTE(brendan): If no size is passed, dynamically find size. */
        bool is_size_dynamic = (*width == 0) && (*height == 0);
        if (is_size_dynamic) {
                *width = codec_context->width;
                *height = codec_context->height;
        }

//         assert(((uint32_t)codec_context->width == *width) &&
//                ((uint32_t)codec_context->height == *height));
    
        if (((uint32_t)codec_context->width != *width) ||
               ((uint32_t)codec_context->height != *height))
        {
                vid_ctx->error_type = PyExc_ValueError;
                vid_ctx->error_msg = "load video width or height error";
        }
    
    
        return is_size_dynamic;
}

static PyObject *
loadvid_frame_nums(PyObject *self, PyObject *args, PyObject *kw)
{
        PyObject *result = NULL;

        const char *filename = NULL;

        PyObject *frame_nums = NULL;
        uint32_t width = 0;
        uint32_t height = 0;

        // resize
        uint32_t resize = 0;
        uint32_t rewidth = 0;
        uint32_t reheight = 0;

        /* NOTE(brendan): should_seek must be int (not bool) because Python. */
        int32_t should_seek = false;
        int32_t should_key = false;
        
        /*timeout*/
        int32_t timeout = 0;
            
        static char *kwlist[] = {"filename",
                                 "frame_nums",
                                 "width",
                                 "height",
                                 "resize",
                                 "should_key",
                                 "should_seek",
                                 "timeout",
                                 0};

        if (!PyArg_ParseTupleAndKeywords(args,
                                         kw,
                                         "sO|IIIppI:loadvid_frame_nums",
                                         kwlist,
                                         &filename,
                                         &frame_nums,
                                         &width,
                                         &height,
                                         &resize,
                                         &should_key,
                                         &should_seek,
                                         &timeout))
                return NULL;

        if (!PySequence_Check(frame_nums)) {
                PyErr_SetString(PyExc_TypeError,
                                "frame_nums needs to be a sequence");
                return NULL;
        }
        if (should_key)
                should_seek = false;
    
        if (timeout <= 0)
                timeout = DEFAULT_TIMEOUT_SEC;

        struct video_stream_context vid_ctx;
        int32_t status = setup_vid_stream_context_filename(&vid_ctx, filename, timeout);

        if (status != LOADVID_SUCCESS) {
                PyErr_SetString(vid_ctx.error_type, vid_ctx.error_msg);
                return NULL;
        }

        bool is_size_dynamic = get_vid_width_height(&width,
                                                    &height,
                                                    &vid_ctx);
    
        if (vid_ctx.error_type != NULL) {
                PyErr_SetString(vid_ctx.error_type, vid_ctx.error_msg);
                return NULL;
        }

        /**
         * TODO(brendan): There is a hole in the logic here, where a bad status
         * could be returned from `setup_vid_stream_context`, but the width and
         * height from `codec_context` is still used to allocate `frames`.
         *
         * It is safer to pass the width and height as arguments, if there is a
         * possibility that videos in the dataset have no video stream.
         */
        if (resize == 0) {
                rewidth = width;
                reheight = height;
        }
        else
        {
                if (width < height) {
                        rewidth = resize;
                        reheight = (uint32_t)(resize * height / width);
                } else {
                        reheight = resize;
                        rewidth = (uint32_t)(resize * width / height);
                }
        }

        const Py_ssize_t num_frames = PySequence_Size(frame_nums);
        PyByteArrayObject *frames = alloc_pyarray(num_frames*rewidth*reheight*3);
        if (PyErr_Occurred() || (frames == NULL))
                return (PyObject *)frames;

        int32_t *frame_nums_buf = PyMem_RawMalloc(num_frames*sizeof(int32_t));
        if (frame_nums_buf == NULL)
                return PyErr_NoMemory();

        int32_t i;
        for (i = 0;
             i < num_frames;
             ++i) {
                PyObject *item = PySequence_GetItem(frame_nums, i);
                if (item == NULL)
                        goto clean_up;

                frame_nums_buf[i] = PyLong_AsLong(item);
                Py_DECREF(item);
                if (PyErr_Occurred())
                        goto clean_up;
        }

        result = (PyObject *)frames;

        decode_video_from_frame_nums((uint8_t *)(frames->ob_bytes),
                                                 &vid_ctx,
                                                 num_frames,
                                                 frame_nums_buf,
                                                 &rewidth,
                                                 &reheight,
                                                 should_key,
                                                 should_seek);
        if (vid_ctx.error_type != NULL) {
                PyErr_SetString(vid_ctx.error_type, vid_ctx.error_msg);
                return NULL;
        }
    
        PyMem_RawFree(frame_nums_buf);

clean_up:
        clean_up_vid_ctx(&vid_ctx);

        if (result != (PyObject *)frames) {
                Py_CLEAR(frames);
                return result;
        }

        if (!is_size_dynamic && resize == 0)
                return (PyObject *)frames;

        result = Py_BuildValue("Oii", frames, rewidth, reheight);
        Py_DECREF(frames);

        return result;
}

static PyObject *
frame_count(PyObject *self, PyObject *args, PyObject *kw)
{
        const char *filename = NULL;
        int64_t frame_num = 0;
        int32_t timeout = 0;

        static char *kwlist[] = {"filename",
                                 "timeout",
                                 0};

        if (!PyArg_ParseTupleAndKeywords(args,
                                         kw,
                                         "s|I:get_video_frame_num",
                                         kwlist,
                                         &filename,
                                         &timeout))
                return NULL;
    
        if (timeout <= 0) {
            timeout = DEFAULT_TIMEOUT_SEC;
        }

        struct video_stream_context vid_ctx;

        int32_t status = setup_vid_stream_context_filename(&vid_ctx, filename, timeout);
        
        if (status != LOADVID_SUCCESS) {
                PyErr_SetString(vid_ctx.error_type, vid_ctx.error_msg);
                return NULL;
        }
        frame_num = vid_ctx.nb_frames;

        clean_up_vid_ctx(&vid_ctx);

        return Py_BuildValue("i", frame_num);
}



static PyObject *
loadvid(PyObject *self, PyObject *args, PyObject *kw)
{
        PyObject *result = NULL;
        const char *filename = NULL;
        Py_ssize_t in_size_bytes = 0;
        bool should_random_seek = true;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t num_frames = 32;
        float seek_distance = 0.0f;
        int32_t timeout = 0;
        static char *kwlist[] = {"filename",
                                 "should_random_seek",
                                 "width",
                                 "height",
                                 "num_frames",
                                 "timeout",
                                 0};

        if (!PyArg_ParseTupleAndKeywords(args,
                                         kw,
                                         "s#|$pIIII:loadvid",
                                         kwlist,
                                         &filename,
                                         &in_size_bytes,
                                         &should_random_seek,
                                         &width,
                                         &height,
                                         &num_frames,
                                         &timeout))
                return NULL;
        
        if (timeout <= 0) {
            timeout = DEFAULT_TIMEOUT_SEC;
        }

        struct video_stream_context vid_ctx;
        int32_t status = setup_vid_stream_context_filename(&vid_ctx, filename, timeout);

        bool is_size_dynamic = get_vid_width_height(&width,
                                                    &height,
                                                    &vid_ctx);
        // add for width/height error
        if (vid_ctx.error_type != NULL) {
                PyErr_SetString(vid_ctx.error_type, vid_ctx.error_msg);
                return NULL;
        }

        PyByteArrayObject *frames = alloc_pyarray(num_frames*width*height*3);
        if (PyErr_Occurred() || (frames == NULL))
                return (PyObject *)frames;

        if (status != LOADVID_SUCCESS) {
                /**
                 * NOTE(brendan): In case there was a stream index error,
                 * return a garbage buffer.
                 */
                if (status == LOADVID_ERR_STREAM_INDEX) {
                          PyErr_SetString(PyExc_ValueError, "Load video stream index error.");
                          goto return_frames;
                }
                PyErr_SetString(PyExc_ValueError, "Load video error.");
                return NULL;
        }

        int64_t timestamp = seek_to_closest_keypoint(&seek_distance,
                                                     &vid_ctx,
                                                     should_random_seek,
                                                     num_frames);
    
        if (vid_ctx.error_type != NULL) {
                PyErr_SetString(vid_ctx.error_type, vid_ctx.error_msg);
                return NULL;
        }

        /*
         * NOTE(brendan): after this point, the only possible errors are due to
         * not having enough frames in the video stream past the initial seek
         * point. All other errors are covered by asserts.
         *
         * Therefore we return the frames buffer regardless, and it is a
         * feature to return garbage in the decoded video output buffer, rather
         * than returning an error, if there weren't any frames to decode in
         * the first place.
         */
        result = (PyObject *)frames;

        status = skip_past_timestamp(&vid_ctx, timestamp);
        if (status != VID_DECODE_SUCCESS) {
                PyErr_SetString(PyExc_ValueError, "skip past timestamp error.");
                goto clean_up_av_frame;
        }

        decode_video_to_out_buffer((uint8_t *)(frames->ob_bytes),
                                               &vid_ctx,
                                               num_frames);
        
        if (vid_ctx.error_type != NULL) {
                PyErr_SetString(vid_ctx.error_type, vid_ctx.error_msg);
                return NULL;
        }
        
clean_up_av_frame:
        clean_up_vid_ctx(&vid_ctx);

        if (result != (PyObject *)frames) {
                Py_CLEAR(frames);
                return result;
        }

return_frames:
        if (!is_size_dynamic)
                result = Py_BuildValue("Of", frames, seek_distance);
        else
                result = Py_BuildValue("Oiif",
                                       frames,
                                       width,
                                       height,
                                       seek_distance);
        Py_DECREF(frames);

        return result;
}

static PyMethodDef lintel_methods[] = {
        {"loadvid",
         (PyCFunction)loadvid,
         METH_VARARGS | METH_KEYWORDS,
         PyDoc_STR("loadvid(encoded_video, should_random_seek, width, height, num_frames) -> "
                   "tuple(decoded video ByteArray object, seek_distance) or\n"
                   "tuple(decoded video ByteArray object, width, height, seek_distance)\n"
                   "if width and height are not passed as arguments.")},
        {"loadvid_frame_nums",
         (PyCFunction)loadvid_frame_nums,
         METH_VARARGS | METH_KEYWORDS,
         PyDoc_STR("loadvid_frame_nums(filename, frame_nums, width, height, resize, should_key, should_seek) -> "
                   "decoded video ByteArray object or\n"
                   "tuple(decoded video ByteArray object, width, height)\n"
                   "if width and height are not passed as arguments, and resize is zero.")},

        {"frame_count",
         (PyCFunction)frame_count,
         METH_VARARGS | METH_KEYWORDS,
         PyDoc_STR("frame_count(filename, should_key) -> "
                   "frame_num")},
        {NULL, NULL, 0, NULL}
};

static struct PyModuleDef
lintelmodule = {
        PyModuleDef_HEAD_INIT,
        "_lintel",
        module_doc,
        0,
        lintel_methods,
        NULL,
        NULL,
        NULL,
        NULL
};

PyMODINIT_FUNC
PyInit__lintel(void)
{
        av_register_all();
        av_log_set_level(AV_LOG_ERROR);
        srand(time(NULL));

        return PyModuleDef_Init(&lintelmodule);
}
