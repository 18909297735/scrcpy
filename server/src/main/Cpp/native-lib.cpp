//
// Created by wushaojie on 3/25/20.
//

#include <jni.h>
#include <string>
#include <iostream>
using namespace std;

extern "C" {
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
}

AVOutputFormat *ofmt = NULL;
AVFormatContext *ofmt_ctx = NULL;
const char *out_filename = "rtmp://192.168.35.85:1935/live/home";
int stream_index = 0;
int waitI = 0, rtmpisinit = 0;
int ptsInc = 0;


//jbyteArray-->char *
char* ConvertJByteaArrayToChars(JNIEnv *env, jbyteArray bytearray)
{
    char *chars = NULL;
    jbyte *bytes;
    bytes = env->GetByteArrayElements(bytearray, 0);
    int chars_len = env->GetArrayLength(bytearray);
    chars = new char[chars_len + 1];
    memset(chars,0,chars_len + 1);
    memcpy(chars, bytes, chars_len);
    chars[chars_len] = 0;
    env->ReleaseByteArrayElements(bytearray, bytes, 0);
    return chars;
}

int GetSpsPpsFromH264(uint8_t* buf, int len)
{
    int i = 0;
    for (i = 0; i < len; i++) {
        if (buf[i+0] == 0x00
            && buf[i + 1] == 0x00
            && buf[i + 2] == 0x00
            && buf[i + 3] == 0x01
            && buf[i + 4] == 0x06) {
            break;
        }
    }
    if (i == len) {
        return 0;
    }

    for (int j = 0; j < i; j++) {
        printf("%x ", buf[j]);
    }
    return i;
}

static bool isIdrFrame2(uint8_t* buf, int len)
{
    switch (buf[0] & 0x1f) {
        case 7: // SPS
            return true;
        case 8: // PPS
            return true;
        case 5:
            return true;
        case 1:
            return false;

        default:
            return false;
            break;
    }
    return false;
}

static bool isIdrFrame1(uint8_t* buf, int size)
{
    int last = 0;
    for (int i = 2; i <= size; ++i) {
        if (i == size) {
            if (last) {
                bool ret = isIdrFrame2(buf + last, i - last);
                if (ret) {
                    return true;
                }
            }
        }
        else if (buf[i - 2] == 0x00 && buf[i - 1] == 0x00 && buf[i] == 0x01) {
            if (last) {
                int size = i - last - 3;
                if (buf[i - 3]) ++size;
                bool ret = isIdrFrame2(buf + last, size);
                if (ret) {
                    return true;
                }
            }
            last = i + 1;
        }
    }
    return false;
}

static int RtmpInit(void* spspps_date, int spspps_datalen)
{
    int ret = 0;
    AVStream *out_stream;
    AVCodecParameters *out_codecpar;

    av_register_all();
    //avcodec_register_all();

    avformat_network_init();

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", NULL);// out_filename);
    if (!ofmt_ctx) {
        ret = AVERROR_UNKNOWN;
        if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        if (ofmt_ctx) {
            avformat_free_context(ofmt_ctx);
            ofmt_ctx = NULL;
        }
        return -1;
    }

    ofmt = ofmt_ctx->oformat;

    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) {
        ret = AVERROR_UNKNOWN;
        if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        if (ofmt_ctx) {
            avformat_free_context(ofmt_ctx);
            ofmt_ctx = NULL;
        }
        return -1;
    }
    stream_index = out_stream->index;

    //因为输入是内存读出来的一帧帧的H264数据，所以没有输入的codecpar信息，必须手动添加输出的codecpar
    out_codecpar = out_stream->codecpar;
    out_codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    out_codecpar->codec_id = AV_CODEC_ID_H264;
    out_codecpar->bit_rate = 400000;
    out_codecpar->width = 1280;
    out_codecpar->height = 720;
    out_codecpar->codec_tag = 0;
    out_codecpar->format = AV_PIX_FMT_YUV420P;

    //必须添加extradata(H264第一帧的sps和pps数据)，否则无法生成带有AVCDecoderConfigurationRecord信息的FLV
    out_codecpar->extradata_size = spspps_datalen;
    out_codecpar->extradata = (uint8_t*)av_malloc(spspps_datalen + AV_INPUT_BUFFER_PADDING_SIZE);
    if (out_codecpar->extradata == NULL)
    {
        if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        if (ofmt_ctx) {
            avformat_free_context(ofmt_ctx);
            ofmt_ctx = NULL;
        }
        return -1;
    }
    memcpy(out_codecpar->extradata, spspps_date, spspps_datalen);

    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
                avio_closep(&ofmt_ctx->pb);
            if (ofmt_ctx) {
                avformat_free_context(ofmt_ctx);
                ofmt_ctx = NULL;
            }
            return -1;
        }
    }

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "flvflags", "add_keyframe_index", 0);
    ret = avformat_write_header(ofmt_ctx, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        if (ofmt_ctx) {
            avformat_free_context(ofmt_ctx);
            ofmt_ctx = NULL;
        }
        return -1;
    }

    waitI = 1;
    return 0;

/*
end:
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    if (ofmt_ctx) {
        avformat_free_context(ofmt_ctx);
        ofmt_ctx = NULL;
    }
    return -1;

    */

}

static void VideoWrite(void* data, int datalen)
{
    int ret = 0, isI = 0;
    AVStream *out_stream;
    AVPacket pkt;

    out_stream = ofmt_ctx->streams[stream_index];

    av_init_packet(&pkt);
    isI = isIdrFrame1((uint8_t*)data, datalen);
    pkt.flags |= isI ? AV_PKT_FLAG_KEY : 0;
    pkt.stream_index = out_stream->index;
    pkt.data = (uint8_t*)data;
    pkt.size = datalen;
    //wait I frame
    if (waitI) {
        if (0 == (pkt.flags & AV_PKT_FLAG_KEY))
            return;
        else
            waitI = 0;
    }

    AVRational time_base;
    time_base.den = 50;
    time_base.num = 1;
    pkt.pts = av_rescale_q((ptsInc++) * 2, time_base, out_stream->time_base);
    pkt.dts = av_rescale_q_rnd(pkt.dts, out_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    pkt.duration = av_rescale_q(pkt.duration, out_stream->time_base, out_stream->time_base);
    pkt.pos = -1;

    ret = av_interleaved_write_frame(ofmt_ctx, &pkt);

    av_packet_unref(&pkt);
}

static void RtmpUnit(void)
{
    if (ofmt_ctx)
        av_write_trailer(ofmt_ctx);
    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    if (ofmt_ctx) {
        avformat_free_context(ofmt_ctx);
        ofmt_ctx = NULL;
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_genymobile_scrcpy_ScreenEncoder_stringFromeJNI(JNIEnv *env, jobject thiz, jobject rtmph264) {
    // TODO: implement stringFromeJNI()

    jbyte * a = (jbyte*) env->GetDirectBufferAddress(rtmph264);
    int dwCapacity  = env->GetDirectBufferCapacity(rtmph264);
    jbyteArray data = env->NewByteArray(dwCapacity);
    env->SetByteArrayRegion(data, 0, dwCapacity, a);

    //jbyteArray-->char*
    int iPsLength = dwCapacity;
    char * h264buffer = new char[iPsLength];
    memcpy(h264buffer, data, iPsLength);

    //data h264--rtmp
    if (!rtmpisinit) {
        if (isIdrFrame1((uint8_t*)h264buffer, iPsLength)) {
            int spspps_len = GetSpsPpsFromH264((uint8_t*)h264buffer, iPsLength);
            if (spspps_len > 0) {
                char *spsbuffer = new char[spspps_len];
                memcpy(spsbuffer, h264buffer, spspps_len);
                rtmpisinit = 1;
                RtmpInit(spsbuffer, spspps_len);
                delete[] spsbuffer;
            }
        }
    }

    if (rtmpisinit) {
        VideoWrite(h264buffer, iPsLength);
    }

    RtmpUnit();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_genymobile_scrcpy_ScreenEncoder_a(JNIEnv *env, jobject thiz) {
    // TODO: implement a()
}