#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
}
#include "../PlayerFlvParser.h"
#include <thread>
#include <vector>
#include <fcntl.h>
#include <assert.h>
using namespace std;

//g++ AudioCodecTest.cc -o AudioCodecTest -lavformat -lswresample -lavcodec -lavutil -lSDL2
void preparFFmpeg(const char *url);

/**
 * 释放内存
 */
void free();
//一帧PCM的数据长度
unsigned int audioLen = 0;
int bufTotalSize;
//unsigned char *audioChunk = nullptr;
//当前读取的位置
unsigned char *audioPos = nullptr;
int playIndex = 0;
/** 被SDL2调用的回调函数 当需要获取数据喂入硬件播放的时候调用 **/
void fill_audio(void *codecContext, Uint8 *stream, int len) {
    //SDL2中必须首先使用SDL_memset()将stream中的数据设置为0
    SDL_memset(stream, 0, len);
    if (audioLen >= len){
        printf("cb: len: %d audiolen: %d\n", len, audioLen);
        //将数据合并到 stream 里
        SDL_MixAudio(stream, audioPos + playIndex, len, SDL_MIX_MAXVOLUME);
        printf("mix\n");
        //一帧的数据控制
        playIndex = (playIndex + len) % bufTotalSize;
        audioLen -= len;
    }
}



/** ########### SDL初始化 ############## **/
/** 自己想要的输出的音频格式 **/
SDL_AudioSpec wantSpec;
/** 重采样上下文 **/
SwrContext *auConvertContext;

/** ########### FFmpeg 相关 ############# **/
AVCodecContext *codecContext;
AVCodec *codec;
AVPacket *packet;
AVFrame *frame;
int audioIndex = -1;



/** 外部调用方法 **/
void playAudio(const char *url) {
    av_register_all();
    codecContext = avcodec_alloc_context3(NULL);
    if (!codecContext) {
        cout << "[error] alloc codec context error!" << endl;
        return;
    }

    //查找解码器
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        cout << "[error] find decoder error!" << endl;
        return;
    }

    //打开解码器
    avcodec_open2(codecContext, codec, nullptr);

    //初始化一个packet
    packet = av_packet_alloc();
    
    //初始化一个Frame
    frame = av_frame_alloc();
    /** ########## 获取实际音频的参数 ##########**/
    //单个通道中的采样数
    out_nb_samples = codecContext->frame_size;
    printf("out_nb_samples: %d\n", out_nb_samples);
    //输出的声道数
    out_channels = av_get_channel_layout_nb_channels(out_chn_layout);
    //输出音频的布局
    in_chn_layout = av_get_default_channel_layout(codecContext->channels);

    /** 计算重采样后的实际数据大小,并分配空间 **/
    //计算输出的buffer的大小
    //4608 = 2 * 2B * 1152
    out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);
    printf("out_buffer_size: %d\n", out_buffer_size);
    //分配输出buffer的空间
    //outBuff = (unsigned char *) av_malloc(MAX_AUDIO_FRAME_SIZE * 2); //双声道
    //10个buffer
    bufTotalSize = out_buffer_size * 10;
    outBuff = (unsigned char *) av_malloc(bufTotalSize);
    //初始化SDL中自己想设置的参数
    wantSpec.freq = out_sample_rate;
    wantSpec.format = AUDIO_S16SYS;
    wantSpec.channels = out_channels;
    wantSpec.silence = 0;
    wantSpec.samples = out_nb_samples;
    wantSpec.callback = fill_audio;
    wantSpec.userdata = codecContext;

    //打开音频之后wantSpec的值可能会有改动，返回实际设备的参数值
    if (SDL_OpenAudio(&wantSpec, NULL) < 0) {
        printf("get error %s\n", SDL_GetError());
        cout << "[error] open audio error" << endl;
        return;
    }
    printf("codecSpec: %d %d %d %d %d\n", codecContext->sample_rate, codecContext->sample_fmt, codecContext->channels, wantSpec.silence, codecContext->frame_size);
    printf("wantSpec: %d %d %d %d %d\n", wantSpec.freq, wantSpec.format, wantSpec.channels, wantSpec.silence, wantSpec.samples);
    //初始化重采样器
    auConvertContext = swr_alloc_set_opts(NULL, out_chn_layout, out_sample_fmt, out_sample_rate,
                                          in_chn_layout, codecContext->sample_fmt, codecContext->sample_rate, 0,
                                          NULL);
    //初始化SwResample的Context
    swr_init(auConvertContext);

    //开始播放 调用这个方法硬件才会开始播放
    SDL_PauseAudio(0);

    //循环读取packet并且解码
    int sendcode = 0;
    unsigned char *decodePtr = nullptr;
    int decodeIndex = 0;
    audioPos = outBuff;
    printf("aac header:\n");
    for (int i = 0; i < codecContext->extradata_size; i++){
        printf("%02x ", codecContext->extradata[i]);
    }
    printf("\n");
    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index != audioIndex) continue;
        //接受解码后的音频数据
        for (int i = 0; i < 200; i++){
            printf("%02x ", packet->data[i]);
        }
        printf("\n");
        while (avcodec_receive_frame(codecContext, frame) == 0) {
            /*
            printf("frame:\n");
            for (int i = 0; i < 50 / 2; i+=2){
                printf("Left: %02x %02x ", frame->data[0][i], frame->data[0][i + 1]); //left
                printf("Right: %02x %02x ", frame->data[1][i], frame->data[1][i + 1]); //right
            }
            */
            printf("planar\n");
            while ((decodeIndex + out_buffer_size) % bufTotalSize == playIndex){
                printf("delay\n");
                SDL_Delay(1);
            }
            decodePtr = outBuff + decodeIndex;
            swr_convert(auConvertContext, &decodePtr, out_nb_samples, (const uint8_t **) frame->data,
                    frame->nb_samples);
            //如果没有播放完就等待1ms
            //printf("before audioLen: %d\n", audioLen);
            /*
            while (audioLen >= out_buffer_size * 10){
                printf("=====wait======\n");
                SDL_Delay(1);
            }
            */
           /*
            for (int i = 0; i < 200; i++){
                printf("%02x ", decodePtr[i]);
            }
            printf("normal\n");
            */
            decodeIndex = (decodeIndex + out_buffer_size) % bufTotalSize;
            audioLen += out_buffer_size;
            printf("decodeIndex: %d\n", decodeIndex);
            //printf("after audioLen: %d\n", audioLen);
        }
        //发送解码前的包数据
        sendcode = avcodec_send_packet(codecContext, packet);
        //根据发送的返回值判断状态
        av_packet_unref(packet);
    }


}

void free(){
    if (codecContext != nullptr) avcodec_free_context(&codecContext);
    if (packet != nullptr) av_packet_free(&packet);
    if (frame != nullptr) av_frame_free(&frame);
    if (auConvertContext != nullptr) swr_free(&auConvertContext);
    SDL_CloseAudio();
    SDL_Quit();
}

void DecodeFunc(string filepath, Parser &parser){
    avcodec_register_all();
    codecContext = avcodec_alloc_context3(NULL);
    if (!codecContext) {
        cout << "[error] alloc codec context error!" << endl;
        return;
    }
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        cout << "[error] find decoder error!" << endl;
        return;
    }
    avcodec_open2(codecContext, codec, nullptr);

    evbuffer *buf = evbuffer_new();
    int fd = open(filepath.c_str(), O_RDONLY);
    if (!fd) {
        fprintf(stderr, "Could not open %s\n", filepath.c_str());
        exit(1);
    }
    int ret = 1;
    int index = 0;
    Tag *tag = nullptr;
    AudioTag *curTag = nullptr;
    unsigned char *data = nullptr;
    AVPacket *pkt = nullptr;
    while(ret){
        ret = evbuffer_read(buf, fd, 1024 * 1024);
        if (ret <= 0) break;
        parser.ParseFlv(buf);
        while (parser.flvtags.size() > index){
            tag = parser.flvtags[index];
            if (tag->header->type == 0x08){
                curTag = (AudioTag *)tag;
                if (curTag->getAACData() != nullptr){
                    evbuffer *audio = curTag->getAACData();
                    data = new uint8_t[evbuffer_get_length(audio)];
                    size_t pktsize = evbuffer_get_length(audio);
                    evbuffer_remove(audio, data, evbuffer_get_length(audio));
                    pkt = av_packet_alloc();
                    pkt->data = data;
                    pkt->size = pktsize;
                    Decode(ctx, pkt, m, cv, frames);
                    delete []data; //FIXME av_packet_free没有释放pkt内存,手动释放
                    av_packet_free(&pkt);
                    assert(pkt == nullptr);
                }
                delete curTag;//边解析边释放已经传入解码器的flvtag,但是不清flvtags vector,目的是防止解码过程使用过多内存
                parser.flvtags[index] = nullptr;
                index++;
            } else {
                delete tag;//边解析边释放非视频flvtag
                parser.flvtags[index] = nullptr;
                index++;
            }
        }



    
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    out_nb_samples = codecContext->frame_size;
    out_channels = av_get_channel_layout_nb_channels(out_chn_layout);
    in_chn_layout = av_get_default_channel_layout(codecContext->channels);

    //4608 = 2 * 2B * 1152
    out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);
    printf("out_buffer_size: %d\n", out_buffer_size);
    auConvertContext = swr_alloc_set_opts(NULL, out_chn_layout, out_sample_fmt, out_sample_rate,
                in_chn_layout, codecContext->sample_fmt, codecContext->sample_rate, 0,
                NULL);
    swr_init(auConvertContext);


    int ret = avcodec_send_packet(codecCtx, pkt);
        if (ret < 0) {
            fprintf(stderr, "Error sending a packet for decoding\n");
            //exit(1);
        }
        while (ret >= 0) {
            AVFrame *frame = av_frame_alloc();
            ret = avcodec_receive_frame(ctx->codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                av_frame_free(&frame);
                assert(frame == nullptr);
                return;
            }
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                exit(1);
            }
            uint8_t *buf = new uint8_t[];
            swr_convert(auConvertContext, &buf, out_nb_samples, (const uint8_t **) frame->data,
                    frame->nb_samples);
            std::unique_lock<std::mutex> lock(ctx->m);
           //printf"thread push index %ld\n", ctx->frames.size());
            ctx->frames.push_back(frame);
            ctx->decodeIndex++;
        }


    int sendcode = 0;
    unsigned char *decodePtr = nullptr;
    int decodeIndex = 0;
    while (avcodec_receive_frame(codecContext, frame) == 0) {
        swr_convert(auConvertContext, &decodePtr, out_nb_samples, (const uint8_t **) frame->data,
                frame->nb_samples);
    }
    sendcode = avcodec_send_packet(codecContext, packet);
    av_packet_unref(packet);
}

void Decode(AVCodecContext *ctx, AVPacket *pkt,
        std::mutex &m, std::condition_variable &cv, vector<evbuffer *> &audios, int &out_nb_samples){
    if (out_nb_samples == -1 && ctx->frame_number > 0){
        std::unique_lock<std::mutex> lk(m);
        out_nb_samples = ctx->frame_number 
        cv.notify_one();
    }
    int ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        //exit(1);
    }
    while (ret >= 0) {
        AVFrame *frame = av_frame_alloc();
        ret = avcodec_receive_frame(ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            av_frame_free(&frame);
            assert(frame == nullptr);
            return;
        }
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }
        assert(out_nb_samples != 0);
        int bufsize = av_samples_get_buffer_size(nullptr, 
            av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO), 
                out_nb_samples, AV_SAMPLE_FMT_S16, 1);
        uint8_t *buf = new uint8_t[bufsize];
        swr_convert(auConvertContext, &buf, out_nb_samples, (const uint8_t **) frame->data,
                    frame->nb_samples);
        evbuffer *evbuf = evbuffer_new();
        evbuffer_add(evbuf, buf, bufsize);
        std::unique_lock<std::mutex> lock(m);
        audio.push_back(evbuf);
        delete[] buf;
        av_frame_free(&frame);
    }
}

int main(int argc, char* argv[]){
    Parser parser;
    vector<evbuffer *> audios;
    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = 48000;
    int out_nb_samples = -1;
    int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
    thread decodeThread([&out_nb_samples](){
        
    });
    playAudio(argv[1]);
}