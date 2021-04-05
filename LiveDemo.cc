#include <stdio.h>
#include <unistd.h>
extern "C"
{
#include "libavcodec/avcodec.h"
#include <SDL2/SDL.h>
#include <libswresample/swresample.h>
};
#include <arpa/inet.h> 
#include <string>
#include <fcntl.h>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/thread.h>
#include <event2/event.h>
#include <event2/dns.h>
#include <assert.h>
#include <event.h>
#include <atomic>
#include <sys/queue.h>
//#include "CircleQueue.h"
#include "PlayerFlvParser.h"
#include "json.hpp"

#define LOADPIC_EVENT  (SDL_USEREVENT + 1)
#define BREAK_EVENT  (SDL_USEREVENT + 2)

std::atomic<bool> quit(false);

//g++ LiveDemo.cc -o LiveDemo -lavcodec -lavutil -levent -lSDL2 -lswresample -lpthread

class ReqContext{
    public:
    ReqContext(event_base *b, evdns_base *dnsb)
        :base(b), dnsbase(dnsb) {}
    
    ~ReqContext(){
        for (auto conn: conns){
            evhttp_connection_free(conn);
        }
        conns.clear();
    }

    std::vector<std::string> RequestForUrl(std::string roomid, std::string pf){
        if (!(pf == "web" || pf == "h5") || roomid.size() == 0){
           //printf"platform error or roomid error\n");
            return std::vector<std::string>();
        }
        platform = pf;
        std::string url = std::string("http://api.live.bilibili.com/room/v1/Room/room_init?id=") + roomid;
        struct evhttp_uri* uri = evhttp_uri_parse(url.c_str());
        assert(dnsbase);
        //function<void(struct evhttp_request*, void*)> func = bind(&ReqContext::RequestForRoomMember, this, placeholders::_1, placeholders::_2);
        struct evhttp_request* request = evhttp_request_new(RequestForRoom, this);
        evhttp_request_set_chunked_cb(request, [](evhttp_request *req, void *arg){
            ReqContext *ctx = (ReqContext *)arg;
            int len = atoi(evhttp_find_header(evhttp_request_get_input_headers(req), "Content-Length"));
            char buf[len];
            memset(buf, 0, len);
            struct evbuffer* evbuf = evhttp_request_get_input_buffer(req);
            if (evbuffer_get_length(evbuf) >= len){
                evbuffer_remove(evbuf, buf, len);
            }
            nlohmann::json j = nlohmann::json::parse(std::string(buf, len));
            if (j["data"]["live_status"] != 1){
               //printf"no live\n");
                event_base_loopbreak(ctx->base);
            }
            if (j.find("data") != j.end())
                if (j["data"].find("room_id") != j["data"].end())
                    ctx->room_id = j["data"]["room_id"];
            
        });
        const char* host = evhttp_uri_get_host(uri);
        int port = evhttp_uri_get_port(uri);
        if (port < 0) port = 80;
        const char* request_url = url.c_str();
        const char* path = evhttp_uri_get_path(uri);
        evhttp_connection* connection =  evhttp_connection_base_new(base, dnsbase, host, port);
        conns.push_back(connection);
        evhttp_add_header(evhttp_request_get_output_headers(request), "Host", host);
        evhttp_add_header(evhttp_request_get_output_headers(request), "Connection", "close");
        evhttp_uri_free(uri);
        evhttp_make_request(connection, request, EVHTTP_REQ_GET, request_url);

        event_base_dispatch(base);
        
        return urls;
    }

    static void RequestForRoom(struct evhttp_request* req, void* arg){
        ReqContext *ctx = (ReqContext *)arg;
        if (ctx->room_id <= 0){
           //printf"error: room_id is zero\n");
            event_base_loopbreak(ctx->base);
        }
        std::string url = std::string("http://api.live.bilibili.com/xlive/web-room/v1/playUrl/playUrl?cid=");
        url += ltos(ctx->room_id);
        url += "&qn=10000&platform=";
        url += ctx->platform;
        url += "&https_url_req=0&ptype=16";
        struct evhttp_uri* uri = evhttp_uri_parse(url.c_str());
        struct evhttp_request* request = evhttp_request_new([](evhttp_request *req, void *arg){
            ReqContext *ctx = (ReqContext *)arg;
            event_base_loopbreak(ctx->base);
        }, ctx);
        evhttp_request_set_chunked_cb(request, [](struct evhttp_request* req, void* arg){
            ReqContext *ctx = (ReqContext *)arg;
            int len = atoi(evhttp_find_header(evhttp_request_get_input_headers(req), "Content-Length"));
            char buf[len];
            memset(buf, 0, len);
            struct evbuffer* evbuf = evhttp_request_get_input_buffer(req);
            if (evbuffer_get_length(evbuf) >= len){
                evbuffer_remove(evbuf, buf, len);
            }
            nlohmann::json j = nlohmann::json::parse(std::string(buf, len));
            for (int i = 0; i < j["data"]["durl"].size(); i++){
                ctx->urls.push_back(j["data"]["durl"][i]["url"]);
            }
        });
        const char* host = evhttp_uri_get_host(uri);
        int port = evhttp_uri_get_port(uri);
        if (port < 0) port = 80;
        const char* request_url = url.c_str();
        const char* path = evhttp_uri_get_path(uri);
        evhttp_connection *connection = evhttp_connection_base_new(ctx->base, ctx->dnsbase, host, port);
        ctx->conns.push_back(connection);
        evhttp_add_header(evhttp_request_get_output_headers(request), "cookie",
            evhttp_find_header(evhttp_request_get_input_headers(req), "set-cookie"));
        evhttp_add_header(evhttp_request_get_output_headers(request), "Connection", "close");
        evhttp_add_header(evhttp_request_get_output_headers(request), "Host", host);
        evhttp_uri_free(uri);
        evhttp_make_request(connection, request, EVHTTP_REQ_GET, request_url);
    }

    static std::string ltos(size_t num){
        char ch;
        std::string s;
        while (num){
            ch = (num % 10) + '0';
            num /= 10;
            s = std::string(&ch, 1) + s;
        }
        return s;
    }
    
    size_t room_id = 0;
    std::string platform;
    std::vector<evhttp_connection *> conns;
    std::vector<std::string> urls;
    event_base *base = nullptr;
    evdns_base* dnsbase = nullptr;
};


class DecodeContext{
    //TODO 注意: 当视频退出,需要断连接等操作....
    public:
    DecodeContext(int &w, int &h)
        :width(w),
        height(h)
    {
        parser = new Parser();
        InitCodec();
        input = evbuffer_new();
        base = event_base_new();
        dnsbase = evdns_base_new(base, 1);
        reqCtx = new ReqContext(base, dnsbase);
    }

    ~DecodeContext(){
        avcodec_close(videoCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        for (auto frame : frames){
            if (frame)
                av_frame_free(&frame);
        }
        evbuffer_free(input);
        delete parser;
        delete reqCtx;
    }

    int InitCodec(){
        avcodec_register_all();
        videoCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!videoCodec) {
            fprintf(stderr, "Codec not found\n");
            return -1;
        }
        videoCodecCtx = avcodec_alloc_context3(videoCodec);
        if (videoCodecCtx == nullptr) {
            fprintf(stderr, "Could not allocate video codec context\n");
            return -1;
        }

        if (avcodec_open2(videoCodecCtx, videoCodec, nullptr) < 0) {
            fprintf(stderr, "avodec_error\n");
            return -1;
        }
        audioCodecCtx = avcodec_alloc_context3(NULL);
        if (!audioCodecCtx) {
            fprintf(stderr, "[error] alloc codec context error!\n");
            return -1;
        }
        audioCodec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (audioCodec == nullptr) {
            fprintf(stderr, "[error] find decoder error!\n");
            return -1;
        }
    }

    static void RequestFinishCallback(struct evhttp_request* req, void* arg){
        DecodeContext *ctx = (DecodeContext *)arg;
        //清洗解码器
        //FIXME 尚未编写音频解码器清洗
        VideoDecode(nullptr, ctx);
        assert(ctx->parser->flvtags.size() > 0);
        for (int i = 0; i < ctx->parser->flvtags.size(); i++){
            assert(ctx->parser->flvtags[i] == nullptr);
        }
        std::unique_lock<std::mutex> lock(ctx->m);
        //标志播放结束
        ctx->frames.push_back(nullptr);
        event_base_loopbreak(ctx->base);
    }

    static void ChunkDecodeCallback(struct evhttp_request* req, void* arg){
       //printf"chunk input len: %ld\n", evbuffer_get_length(evhttp_request_get_input_buffer(req)));
        DecodeContext *ctx = (DecodeContext *)arg;
        if (::quit){
            //清洗视频解码器
            int ret = avcodec_send_packet(ctx->videoCodecCtx, nullptr);
            while (ret >= 0) {
                AVFrame *frame = av_frame_alloc();
                ret = avcodec_receive_frame(ctx->videoCodecCtx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                    av_frame_free(&frame);
                    assert(frame == nullptr);
                    return;
                }
                av_frame_free(&frame);
            }
            event_base_loopbreak(ctx->base);
        }

        AVPacket *pkt = nullptr;
        Tag *tag = nullptr;
        uint8_t *data = nullptr;
        int pktsize = 0;

        evbuffer_add_buffer(ctx->input, evhttp_request_get_input_buffer(req));
        ctx->parser->ParseFlv(ctx->input);
        while (ctx->parser->flvtags.size() > ctx->tagIndex){
            tag = ctx->parser->flvtags[ctx->tagIndex];
            if (tag->header->type == 0x09){
                printf("video frame\n");
                VideoTag *curTag = (VideoTag *)tag;
                if (curTag->getH264Stream() != nullptr){
                    evbuffer *videoStream = curTag->getH264Stream();
                    data = new uint8_t[evbuffer_get_length(videoStream)];
                    pktsize = evbuffer_get_length(videoStream);
                    evbuffer_remove(videoStream, data, evbuffer_get_length(videoStream));
                    pkt = av_packet_alloc();
                    pkt->data = data;
                    pkt->size = pktsize;
                   //printf"packet start: size: %d\n", pkt->size);
                    VideoDecode(pkt, ctx);
                    delete []data; //FIXME av_packet_free没有释放pkt内存,手动释放
                    av_packet_free(&pkt);
                    assert(pkt == nullptr);
                }
                delete curTag;//边解析边释放已经传入解码器的flvtag,但是不清flvtags vector,目的是防止解码过程使用过多内存
                ctx->parser->flvtags[ctx->tagIndex] = nullptr;
                ctx->tagIndex++;
            } else if (tag->header->type == 0x08){
                printf("audio frame\n");
                AudioTag *curTag = (AudioTag *)tag;
                if (curTag->getAACData() != nullptr){
                    evbuffer *audio = curTag->getAACData();
                    if (!ctx->getAudioConfig){
                        printf("once\n");
                        ctx->audioCodecCtx->extradata_size = evbuffer_get_length(audio);
                        ctx->audioCodecCtx->extradata = new uint8_t(ctx->audioCodecCtx->extradata_size);
                        evbuffer_remove(audio, ctx->audioCodecCtx->extradata, evbuffer_get_length(audio));
                        avcodec_open2(ctx->audioCodecCtx, ctx->audioCodec, nullptr);
                        ctx->getAudioConfig = true;
                        delete curTag;
                        ctx->parser->flvtags[ctx->tagIndex] = nullptr;
                        ctx->tagIndex++;
                        continue;
                    }
                    printf("decode audio\n");
                    data = new uint8_t[evbuffer_get_length(audio)];
                    size_t pktsize = evbuffer_get_length(audio);
                    evbuffer_remove(audio, data, evbuffer_get_length(audio));
                    pkt = av_packet_alloc();
                    /*
                    for (int i = 0; i < min(50, (int)pktsize); i++){
                        printf("%02x ", data[i]);
                    }
                    printf("\n");
                    */
                    pkt->data = data;
                    pkt->size = pktsize;
                    ctx->AudioDecode(pkt);
                    delete []data; //FIXME av_packet_free没有释放pkt内存,手动释放
                    av_packet_free(&pkt);
                    assert(pkt == nullptr);
                }
                assert(curTag->getAACData() != nullptr);
                delete curTag;//边解析边释放已经传入解码器的flvtag,但是不清flvtags vector,目的是防止解码过程使用过多内存
                ctx->parser->flvtags[ctx->tagIndex] = nullptr;
                ctx->tagIndex++;
            } else {
                delete tag;//边解析边释放非视频flvtag
                ctx->parser->flvtags[ctx->tagIndex] = nullptr;
                ctx->tagIndex++;
            }
        }
    }

    void AudioDecode(AVPacket *pkt){
        //printf("%d %d %d %d\n", codecCtx->sample_fmt, codecCtx->sample_rate, codecCtx->frame_size, out_nb_samples);
        int ret = avcodec_send_packet(audioCodecCtx, pkt);
        if (ret < 0) {
            fprintf(stderr, "Error sending a packet for decoding\n");
            //exit(1);
        }
        //printf("send packet\n");
        while (ret >= 0) {
            AVFrame *frame = av_frame_alloc();
            ret = avcodec_receive_frame(audioCodecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                av_frame_free(&frame);
                assert(frame == nullptr);
                return;
            }
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                exit(1);
            }
            if (out_nb_samples == 0 && audioCodecCtx->frame_size != 0){
                printf("auido imformation: %d %d %d %d\n", audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate, audioCodecCtx->frame_size, out_nb_samples);
                std::unique_lock<std::mutex> lk(atm);
                if (out_nb_samples == 0){
                    out_nb_samples = audioCodecCtx->frame_size;
                    out_channel_layout = audioCodecCtx->channel_layout;
                    int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
                    //4608 = 2 * 2B * 1152
                    swrCtx = swr_alloc_set_opts(nullptr, out_channel_layout, out_sample_fmt, out_sample_rate,
                                audioCodecCtx->channel_layout, audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate, 0,
                                nullptr);
                    swr_init(swrCtx);
                }
                atcv.notify_one();
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
                std::unique_lock<std::mutex> lock(atm);
                printf("push audios size: %d\n", audios.size());
                audios.push_back(evbuf);
            }
            delete[] buf;
            av_frame_free(&frame);
        }
    }

    static void VideoDecode(AVPacket *pkt, DecodeContext* ctx){
        if ((ctx->width == 0 || ctx->height == 0) && ctx->videoCodecCtx->width > 0 && ctx->videoCodecCtx->height > 0){
            std::unique_lock<std::mutex> lk(ctx->m);
            ctx->width = ctx->videoCodecCtx->width;
            ctx->height = ctx->videoCodecCtx->height;
            ctx->cv.notify_one();
        }
        int ret = avcodec_send_packet(ctx->videoCodecCtx, pkt);
        if (ret < 0) {
            fprintf(stderr, "Error sending a packet for decoding\n");
            //exit(1);
        }
        while (ret >= 0) {
            AVFrame *frame = av_frame_alloc();
            ret = avcodec_receive_frame(ctx->videoCodecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                av_frame_free(&frame);
                assert(frame == nullptr);
                return;
            }
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                exit(1);
            }
            std::unique_lock<std::mutex> lock(ctx->m);
           //printf"thread push index %ld\n", ctx->frames.size());
            ctx->frames.push_back(frame);
            ctx->videoDecodeIndex++;
        }
    }

    void StartPullStream(std::string url){
        fprintf(stderr, "pull url: %s\n", url.c_str());
        struct evhttp_uri* uri = evhttp_uri_parse(url.c_str());
        struct evhttp_request* request = evhttp_request_new(DecodeContext::RequestFinishCallback, this);
        evhttp_request_set_chunked_cb(request, DecodeContext::ChunkDecodeCallback);
        evhttp_request_set_header_cb(request, [](evhttp_request *req, void *arg)->int{
            fprintf(stderr, "< HTTP/1.1 %d %s\n", evhttp_request_get_response_code(req), evhttp_request_get_response_code_line(req));
            struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
            struct evkeyval* header;
            TAILQ_FOREACH(header, headers, next)
            {
                fprintf(stderr, "< %s: %s\n", header->key, header->value);
            }
            fprintf(stderr, "< \n");
        });
        const char* host = evhttp_uri_get_host(uri);
        int port = evhttp_uri_get_port(uri);
        if (port < 0) port = 80;
        
        const char* path = evhttp_uri_get_path(uri);
        std::string request_url = std::string(path) + "?" + std::string(evhttp_uri_get_query(uri));
       //printf"host: %s path: %s query: %s scheme: %s\n", host, path, evhttp_uri_get_query(uri), evhttp_uri_get_scheme(uri));
        /*
            GET /live-bvc/323724/live_3056970_6030479.flv?cdn=cn-gotcha04&expires=1616820781&len=0&oi=1965086769&pt=web&qn=10000&trid=6f6c302264314ec89145037fcd3db8ae&sigparams=cdn,expires,len,oi,pt,qn,trid&sign=56d0596191126d7084a013da2c99b2b9&ptype=0&src=8&sl=1&order=1 HTTP/1.1
            User-Agent: Lavf/57.83.100
            Accept: *\/*
            Range: bytes=0-
            Connection: close
            Host: d1--cn-gotcha04.bilivideo.com
            Icy-MetaData: 1
        */
        pullconn =  evhttp_connection_base_new(base, dnsbase, host, port);
        evhttp_add_header(evhttp_request_get_output_headers(request), "User-Agent", "Lavf/57.83.100");
        evhttp_add_header(evhttp_request_get_output_headers(request), "Range", "bytes=0-");
        evhttp_add_header(evhttp_request_get_output_headers(request), "Accept", "*/*");
        evhttp_add_header(evhttp_request_get_output_headers(request), "Connection", "close");
        evhttp_add_header(evhttp_request_get_output_headers(request), "Host", host);
        evhttp_add_header(evhttp_request_get_output_headers(request), "Icy-MetaData", "1");
        evhttp_uri_free(uri);
        evhttp_make_request(pullconn, request, EVHTTP_REQ_GET, request_url.c_str());
        auto ev = event_new(base, -1, EV_PERSIST, [](evutil_socket_t ,short ,void *arg){
            DecodeContext *ctx = (DecodeContext *)arg;
            if (::quit){
                event_base_loopbreak(ctx->base);
            }
            printf("test: step: %ld\n", ctx->videoDecodeIndex - ctx->videoPlayIndex);
            if (ctx->videoDecodeIndex - ctx->videoPlayIndex >= 40 && ctx->videoDecodeIndex - ctx->videoPlayIndex <= 80)
                return;
            auto bufev = evhttp_connection_get_bufferevent(ctx->pullconn);
            if (ctx->videoDecodeIndex - ctx->videoPlayIndex > 80){
                if (bufferevent_get_enabled(bufev) & EV_READ){
                    bufferevent_disable(bufev, EV_READ);
                   printf("test: disable read\n");
                }
            }
            if (ctx->videoDecodeIndex - ctx->videoPlayIndex < 40){
                if (!(bufferevent_get_enabled(bufev) & EV_READ)){
                    printf("test: enable read\n");
                    bufferevent_enable(bufev, EV_READ);
                }
            }
        }, this);
        struct timeval timeout = {0, 10000};
        event_add(ev, &timeout);
        event_base_dispatch(base);
    }

    void StartPlayAudio(){
        {
            std::unique_lock<std::mutex> lk(atm);
            atcv.wait(lk, [this]{return out_nb_samples > 0;});
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
            return;
        }
        SDL_PauseAudio(0);
    }

    static void AudioCallback(void *arg, Uint8 *stream, int len){
        printf("AudioCallback\n");
        DecodeContext *ctx = (DecodeContext *)arg;
        SDL_memset(stream, 0, len);
        if (ctx->audios_local.size() == ctx->audioPlayIndex){
            printf("swap: audios size: %d audio_local size: %d\n", ctx->audios.size(), ctx->audios_local.size());
            std::unique_lock<std::mutex> lk(ctx->m);
            ctx->audios_local.clear();
            ctx->audios_local.swap(ctx->audios);
            ctx->audioPlayIndex = 0;
        }
        printf("local audios index: %d localsize: %d\n", ctx->audioPlayIndex, ctx->audios_local.size());
        if (ctx->audioPlayIndex < ctx->audios_local.size()){
            evbuffer *cur = ctx->audios_local[ctx->audioPlayIndex];
            int audioLen = evbuffer_get_length(cur);
            uint8_t cbuf[audioLen] = {0};
            evbuffer_remove(cur, cbuf, audioLen);
            printf("audioLen: %d len: %d\n", audioLen, len);
            if (audioLen >= len){
                SDL_MixAudio(stream, cbuf, len, SDL_MIX_MAXVOLUME);
            }
            evbuffer_free(cur);
            ctx->audios_local[ctx->audioPlayIndex] = nullptr;
            ctx->audioPlayIndex++;
        }
    }

    //TODO 视频相关的参数,可以抽出VideoDecodeContext
    int &width;
    int &height;
    //TODO 音频相关的参数，可以抽出到AudioDecodeContext
    /* 音频解码和播放相关参数 */
    SwrContext *swrCtx = nullptr;
    SDL_AudioSpec wantSpec;//音频SDL2播放参数
    int out_nb_samples = 0;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    ::AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = 48000;
    int audioPlayIndex = 0; //用于音频线程指示播放位置
    std::vector<evbuffer *> audios_local;//用于音频线程播放
    bool getAudioConfig = false;//标识是否解析了音频config


    std::atomic<size_t> videoPlayIndex = {0};//用于控制视频流量,当播放和解码相隔太远会disable read
    std::atomic<size_t> videoDecodeIndex = {0};//用于控制视频流量

    evhttp_connection *pullconn = nullptr; //拉流连接
    evbuffer *input; //用于缓存chunk数据,交给parser,有状态

    std::mutex m;//用于视频线程
    std::condition_variable cv;//用于视频线程

    std::mutex atm;//用于音频线程
    std::condition_variable atcv;//用于音频线程

    std::vector<AVFrame *> frames;
    std::vector<evbuffer *> audios;
    size_t tagIndex = 0; 
    //用于指示parser中的flvtags的下标,不请空flvtags但是借助该下标手动释放内存

    //TODO 可以抽出到VideoContext
    AVCodecContext *videoCodecCtx = nullptr;
    const AVCodec *videoCodec = nullptr;
    //TODO 可以抽出到AudioContext
    AVCodecContext *audioCodecCtx = nullptr;
    AVCodec *audioCodec = nullptr;

    Parser *parser = nullptr;
    ReqContext *reqCtx = nullptr;
    event_base *base;
    evdns_base *dnsbase;
};

int main(int argc, char* argv[])
{
    //std::atomic<AVCodecContext *> ctx;
    if (argc != 3){
       //printf"Usager: %s [room_id] [platform]", argv[0]);
        return -1;
    }
    int width = 0, height = 0;
    DecodeContext ctx(width, height);
    std::string roomid = argv[1];
    std::string platform = argv[2];
    std::thread t([&ctx, &roomid, &platform]{
        auto urls = ctx.reqCtx->RequestForUrl(roomid, platform);
        if (urls.empty())
            return;
        ctx.StartPullStream(urls[0]);
    });
    if(SDL_Init(SDL_INIT_VIDEO)) {  
		printf( "Could not initialize SDL - %s\n", SDL_GetError()); 
		return -1;
	}
    {
        std::unique_lock<std::mutex> lk(ctx.m);
        ctx.cv.wait(lk, [&width, &height]{return width > 0 && height > 0;});
    }
   //printf"SDL get width: %d, height: %d\n", width, height);
    assert(width > 0 && height > 0);
    SDL_Window *screen; 
	screen = SDL_CreateWindow("SDL2 player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width, height, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
	if(!screen) {
		printf("SDL: could not create window - exiting:%s\n",SDL_GetError());  
		return -1;
	}
	SDL_Renderer* sdlRenderer = SDL_CreateRenderer(screen, -1, 0);  
	//IYUV: Y + U + V  (3 planes)
	//YV12: Y + V + U  (3 planes)
	SDL_Texture* sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV,
         SDL_TEXTUREACCESS_STREAMING, width, height);
    SDL_Rect sdlRect;
	SDL_Event event;
    uint8_t buf[width * height * 3 / 2];
    memset(buf, 0, width * height * 3 / 2);

    //SDL_Thread *refresh_thread = SDL_CreateThread(threadfunc, nullptr, nullptr);
	std::thread t2([]{
        while (!quit) {
            SDL_Event event;
            event.type = LOADPIC_EVENT;
            SDL_PushEvent(&event);
            SDL_Delay(15);
        }
    });
    std::thread t3([&ctx]{
        ctx.StartPlayAudio();
        while (!quit){}
    });
    int index = 0;
    AVFrame *preFrame = nullptr;
    AVFrame *freeFrame = nullptr;
    AVFrame *pFrame = nullptr;
    int windowWidth = width;
    int windowHeight = height;
    std::vector<AVFrame *> decodedFrames;
	while (true) {
        if (freeFrame != nullptr){
           //printf"free frame %p\n", freeFrame);
            av_frame_free(&freeFrame);
            assert(freeFrame == nullptr);
        }

        if (decodedFrames.size() <= index){
            decodedFrames.clear();
            std::unique_lock<std::mutex> lock(ctx.m);
            decodedFrames.swap(ctx.frames);
            index = 0;
        }
		SDL_WaitEvent(&event);
		if(event.type == LOADPIC_EVENT){
            printf("decodedFrames_size: %ld, localindex: %d\n", decodedFrames.size(), index);
            if (decodedFrames.size() >= index + 1){
                pFrame = decodedFrames[index];
                index++;
                ctx.videoPlayIndex++;
            } else {
                if (preFrame)
                    pFrame = preFrame; //播放上一帧
                else continue;
            }
            if (pFrame == nullptr){
                //播放完成
                break;
            }
            int h = height, w = width;
            memcpy(buf, pFrame->data[0], h * w);
            memcpy(buf + w * h, pFrame->data[1], h * w / 4);
            memcpy(buf + w * h * 5 / 4, pFrame->data[2], h * w / 4);
           //printf"UpdateTexture\n");
            SDL_UpdateTexture(sdlTexture, nullptr, buf, pFrame->linesize[0]);
            sdlRect.x = 0;  
            sdlRect.y = 0;
            sdlRect.w = windowWidth;
            sdlRect.h = windowHeight;
            SDL_RenderClear(sdlRenderer);
            SDL_RenderCopy(sdlRenderer, sdlTexture, nullptr, &sdlRect); 
            SDL_RenderPresent(sdlRenderer);
            if (preFrame != pFrame){
                freeFrame = preFrame;
                preFrame = pFrame;
            }
		} else if(event.type == SDL_WINDOWEVENT){
			SDL_GetWindowSize(screen, &windowWidth, &windowHeight);
		} else if(event.type == SDL_QUIT){
			quit = true;
            break;
		}
	}
    quit = true;
    if (preFrame) av_frame_free(&preFrame);
    if (freeFrame) av_frame_free(&freeFrame);
    //sleep(10);
    t.join();
    t2.join();
    SDL_DestroyWindow(screen);
    SDL_DestroyTexture(sdlTexture);
    SDL_DestroyRenderer(sdlRenderer);
	return 0;
    //SUMMARY: AddressSanitizer: 4799 byte(s) leaked in 43 allocation(s).

    /*
    * Indirect leak of 581090826 byte(s) in 3399 object(s) allocated from:
    * #0 0x7fbc20453790 in posix_memalign (/usr/lib/x86_64-linux-gnu/libasan.so.4+0xdf790)
    * #1 0x7fbc1e9f6692 in av_malloc (/usr/lib/x86_64-linux-gnu/libavutil.so.55+0x31692)
    */

    //g++ LiveDemo.cc -o LiveDemo -lavcodec -lavutil -levent -lSDL2 --sanitize=address
}