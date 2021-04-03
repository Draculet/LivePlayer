#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <event2/buffer.h>
}

#include "../PlayerFlvParser.h"
#include <thread>
#include <vector>
#include <fcntl.h>
#include <assert.h>
#include <mutex>
#include <condition_variable>
using namespace std;

//g++ AudioCodecTest.cc -o AudioCodecTest -lswresample -lavcodec -lavutil -lSDL2

class AudioContext{
    public:
    AudioContext(){
        parser = new Parser();
    }
    
    void DecodeThreadFunc(string path){
        avcodec_register_all();
        codecCtx = avcodec_alloc_context3(NULL);
        if (!codecCtx) {
            cout << "[error] alloc codec context error!" << endl;
            return;
        }
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (codec == nullptr) {
            cout << "[error] find decoder error!" << endl;
            return;
        }
        //avcodec_open2(codecCtx, codec, nullptr);
        evbuffer *buf = evbuffer_new();
        int fd = open(path.c_str(), O_RDONLY);
        if (!fd) {
            fprintf(stderr, "Could not open %s\n", path.c_str());
            exit(1);
        }
        int ret = 1;
        int index = 0;
        Tag *tag = nullptr;
        AudioTag *curTag = nullptr;
        uint8_t *data = nullptr;
        AVPacket *pkt = nullptr;
        bool once = true;
        while(ret){
            ret = evbuffer_read(buf, fd, 1024 * 1024);
            if (ret <= 0) break;
            parser->ParseFlv(buf);
            while (parser->flvtags.size() > index){
                tag = parser->flvtags[index];
                if (tag->header->type == 0x08){
                    curTag = (AudioTag *)tag;
                    if (curTag->getAACData() != nullptr){
                        evbuffer *audio = curTag->getAACData();
                        if (once){
                            printf("once\n");
                            codecCtx->extradata_size = evbuffer_get_length(audio);
                            codecCtx->extradata = new uint8_t(codecCtx->extradata_size);
                            evbuffer_remove(audio, codecCtx->extradata, evbuffer_get_length(audio));
                            avcodec_open2(codecCtx, codec, nullptr);
                            once = false;
                            delete curTag;
                            parser->flvtags[index] = nullptr;
                            index++;
                            break;
                        }
                        data = new uint8_t[evbuffer_get_length(audio)];
                        size_t pktsize = evbuffer_get_length(audio);
                        evbuffer_remove(audio, data, evbuffer_get_length(audio));
                        pkt = av_packet_alloc();
                        for (int i = 0; i < min(50, (int)pktsize); i++){
                            printf("%02x ", data[i]);
                        }
                        printf("\n");
                        pkt->data = data;
                        pkt->size = pktsize;
                        Decode(pkt);
                        delete []data; //FIXME av_packet_free没有释放pkt内存,手动释放
                        av_packet_free(&pkt);
                        assert(pkt == nullptr);
                    }
                    assert(curTag->getAACData() != nullptr);
                    delete curTag;//边解析边释放已经传入解码器的flvtag,但是不清flvtags vector,目的是防止解码过程使用过多内存
                    parser->flvtags[index] = nullptr;
                    index++;
                } else {
                    delete tag;//边解析边释放非视频flvtag
                    parser->flvtags[index] = nullptr;
                    index++;
                }
            }
        }
    }

    
    void Decode(AVPacket *pkt){
        //printf("%d %d %d %d\n", codecCtx->sample_fmt, codecCtx->sample_rate, codecCtx->frame_size, out_nb_samples);
        int ret = avcodec_send_packet(codecCtx, pkt);
        if (ret < 0) {
            fprintf(stderr, "Error sending a packet for decoding\n");
            //exit(1);
        }
        printf("send packet\n");
        while (ret >= 0) {
            AVFrame *frame = av_frame_alloc();
            ret = avcodec_receive_frame(codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                av_frame_free(&frame);
                assert(frame == nullptr);
                return;
            }
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                exit(1);
            }
            if (out_nb_samples == 0 && codecCtx->frame_size != 0){
                printf("%d %d %d %d\n", codecCtx->sample_fmt, codecCtx->sample_rate, codecCtx->frame_size, out_nb_samples);
                std::unique_lock<std::mutex> lk(m);
                if (out_nb_samples == 0){
                    out_nb_samples = codecCtx->frame_size;
                    out_channel_layout = codecCtx->channel_layout;
                    int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
                    //in_channel_layout = av_get_default_channel_layout(codecCtx->channels);
                    //in_channel_layout = codecCtx->channel_layout;
                    //4608 = 2 * 2B * 1152
                    //int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);
                    swrCtx = swr_alloc_set_opts(nullptr, out_channel_layout, out_sample_fmt, out_sample_rate,
                                codecCtx->channel_layout, codecCtx->sample_fmt, codecCtx->sample_rate, 0,
                                nullptr);
                    swr_init(swrCtx);
                }
                cv.notify_one();
            }
            assert(out_nb_samples > 0);
            int bufsize = av_samples_get_buffer_size(nullptr, 
                av_get_channel_layout_nb_channels(out_channel_layout), 
                    out_nb_samples, out_sample_fmt, 1);
            uint8_t *buf = new uint8_t[bufsize];
            swr_convert(swrCtx, &buf, out_nb_samples, (const uint8_t **)frame->data,
                frame->nb_samples);
            evbuffer *evbuf = evbuffer_new();
            evbuffer_add(evbuf, buf, bufsize);
            {
                std::unique_lock<std::mutex> lock(m);
                audios.push_back(evbuf);
            }
            delete[] buf;
            av_frame_free(&frame);
        }
    }

    void StartPlay(){
        {
            unique_lock<mutex> lk(m);
            cv.wait(lk, [this]{return out_nb_samples > 0;});
        }
        wantSpec.freq = out_sample_rate;
        wantSpec.format = AUDIO_S16SYS;
        wantSpec.channels = av_get_channel_layout_nb_channels(out_channel_layout);
        wantSpec.silence = 0;
        wantSpec.samples = out_nb_samples;
        wantSpec.callback = AudioCallback;
        wantSpec.userdata = this;

        //打开音频之后wantSpec的值可能会有改动，返回实际设备的参数值
        if (SDL_OpenAudio(&wantSpec, nullptr) < 0) {
            printf("get error %s\n", SDL_GetError());
            cout << "[error] open audio error" << endl;
            return;
        }
        SDL_PauseAudio(0);
    }

    static void AudioCallback(void *audioCtx, Uint8 *stream, int len){
        printf("callback\n");
        AudioContext *ctx = (AudioContext *)audioCtx;
        SDL_memset(stream, 0, len);
        if (ctx->audios_local.size() == ctx->playIndex){
            unique_lock<mutex> lk(ctx->m);
            ctx->audios_local.clear();
            ctx->audios_local.swap(ctx->audios);
            ctx->playIndex = 0;
        }
        if (ctx->playIndex < ctx->audios_local.size()){
            evbuffer *cur = ctx->audios_local[ctx->playIndex];
            int audioLen = evbuffer_get_length(cur);
            uint8_t cbuf[audioLen] = {0};
            evbuffer_remove(cur, cbuf, audioLen);
            if (audioLen >= len){
                printf("cb: len: %d audiolen: %d\n", len, audioLen);
                SDL_MixAudio(stream, cbuf, len, SDL_MIX_MAXVOLUME);
            }
            evbuffer_free(cur);
            ctx->audios_local[ctx->playIndex] = nullptr;
            ctx->playIndex++;
        }
    }

    mutex m;
    condition_variable cv;
    int playIndex = 0;
    vector<evbuffer *> audios;
    vector<evbuffer *> audios_local;
    Parser *parser = nullptr;
    AVCodecContext *codecCtx = nullptr;
    AVCodec *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int out_nb_samples = 0;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    ::AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = 48000;
    int64_t in_channel_layout = 0;
    ::AVSampleFormat in_sample_fmt = AV_SAMPLE_FMT_NONE;
    int in_sample_rate = -1;
    SwrContext *swrCtx = nullptr;
    SDL_AudioSpec wantSpec;
};



/*
void free();
//一帧PCM的数据长度
unsigned int audioLen = 0;
int bufTotalSize;
//unsigned char *audioChunk = nullptr;
//当前读取的位置
unsigned char *audioPos = nullptr;
int playIndex = 0;
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
SDL_AudioSpec wantSpec;
SwrContext *auConvertContext;
AVCodecContext *codecContext;
AVCodec *codec;
AVPacket *packet;
AVFrame *frame;
int audioIndex = -1;
*/
/*
void free(){
    if (codecContext != nullptr) avcodec_free_context(&codecContext);
    if (packet != nullptr) av_packet_free(&packet);
    if (frame != nullptr) av_frame_free(&frame);
    if (auConvertContext != nullptr) swr_free(&auConvertContext);
    SDL_CloseAudio();
    SDL_Quit();
}
*/
/*
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
    }
}
*/
/*
void Decode(AVCodecContext *ctx, AVPacket *pkt,
        std::mutex &m, std::condition_variable &cv, vector<evbuffer *> &audios, int &out_nb_samples){
    if (out_nb_samples == -1 && ctx->frame_number > 0){
        std::unique_lock<std::mutex> lk(m);
        out_nb_samples = ctx->frame_number 
        cv.notify_one();
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
*/

int main(int argc, char* argv[]){
    if (argc != 2) exit(-1);
    AudioContext ctx;
    string path = argv[1];
    thread decodeThread([&ctx, path](){
        ctx.DecodeThreadFunc(path);
    });
    ctx.StartPlay();
    decodeThread.join();
    while(true){}
}