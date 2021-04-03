#include <stdio.h>
#include <unistd.h>
extern "C"
{
#include "libavcodec/avcodec.h"
#include <SDL2/SDL.h>
};
#include <arpa/inet.h> 
#include <string>
#include <fcntl.h>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <event2/buffer.h>
#include <assert.h>
#include <atomic>
#include "CircleQueue.h"
#include "PlayerFlvParser.h"


#define LOADPIC_EVENT  (SDL_USEREVENT + 1)
#define BREAK_EVENT  (SDL_USEREVENT + 2)

//可用于播放flv文件

std::atomic<bool> quit(false);

void Decode(int &width, int &height, AVCodecContext *ctx, AVPacket *pkt,
        std::mutex &m, std::condition_variable &cv, std::vector<AVFrame *> &frames){
    if ((width == 0 || height == 0) && ctx->width > 0 && ctx->height > 0){
        std::unique_lock<std::mutex> lk(m);
        width = ctx->width;
        height = ctx->height;
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
        std::unique_lock<std::mutex> lock(m);
        printf("thread push index %ld\n", frames.size());
        frames.push_back(frame);
    }
}

void DecodeThreadFunc(int &width, int &height, std::vector<AVFrame *> &frames, 
            std::mutex &m, std::condition_variable &cv, std::string filepath){
    AVCodecContext *ctx = nullptr;
    const AVCodec *codec;
    int fd;
    avcodec_register_all();
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        printf("avodec_error\n");                           
        exit(1);
    }
	//c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    fd = open(filepath.c_str(), O_RDONLY);
    if (!fd) {
        fprintf(stderr, "Could not open %s\n", filepath.c_str());
        exit(1);
    }
    size_t inputsize;
    uint8_t *sps;
    int spssize = 0;
	int ret = 1;
    AVPacket *pkt = nullptr;
    evbuffer *evbuf = evbuffer_new();
    Parser parser;
    Tag *tag = nullptr;
    VideoTag *curTag = nullptr;
    uint8_t *data = nullptr;
    int pktsize = 0;
    int index = 0;
    while (ret) {
        ret = evbuffer_read(evbuf, fd, 1024 * 1024);
        if (ret <= 0) break;
        parser.ParseFlv(evbuf);
        while (parser.flvtags.size() > index){
            tag = parser.flvtags[index];
            if (tag->header->type == 0x09){
                curTag = (VideoTag *)tag;
                if (curTag->getH264Stream() != nullptr){
                    evbuffer *videoStream = curTag->getH264Stream();
                    data = new uint8_t[evbuffer_get_length(videoStream)];
                    pktsize = evbuffer_get_length(videoStream);
                    evbuffer_remove(videoStream, data, evbuffer_get_length(videoStream));
                    pkt = av_packet_alloc();
                    pkt->data = data;
                    pkt->size = pktsize;
                    printf("packet start: size: %d\n", pkt->size);
                    Decode(width, height, ctx, pkt, m, cv, frames);
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
    //FIXME 该步用于刷新解码器,是否需要刷新解码器
    Decode(width, height, ctx, nullptr, m, cv, frames);
    {
        std::unique_lock<std::mutex> lock(m);
        //标志播放结束
        frames.push_back(nullptr);
    }
    avcodec_close(ctx);
    avcodec_free_context(&ctx);
    evbuffer_free(evbuf);
    assert(parser.flvtags.size() > 0);
    printf("final: parser flvtags size: %ld\n", parser.flvtags.size());
    for (int i = 0; i < parser.flvtags.size(); i++){
        assert(parser.flvtags[i] == nullptr);
    }
}

int main(int argc, char* argv[])
{
	std::string filepath = std::string(argv[1]);
    std::mutex m;
    std::condition_variable cv;
    std::vector<AVFrame *> frames;
    //std::atomic<AVCodecContext *> ctx;
    int width = 0, height = 0;
    std::thread t([&width, &height, &m, &frames, &cv, filepath]{
        DecodeThreadFunc(width, height, frames, m, cv, filepath);
    });
    if(SDL_Init(SDL_INIT_VIDEO)) {  
		printf( "Could not initialize SDL - %s\n", SDL_GetError()); 
		return -1;
	}
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&width, &height]{return width > 0 && height > 0;});
    }
    printf("SDL get width: %d, height: %d\n", width, height);
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
    uint8_t buf[width * height * 3 / 2] = {0};

    //SDL_Thread *refresh_thread = SDL_CreateThread(threadfunc, nullptr, nullptr);
	std::thread t2([]{
        while (!quit) {
            SDL_Event event;
            event.type = LOADPIC_EVENT;
            SDL_PushEvent(&event);
            SDL_Delay(40);
        }
    });
    int index = 0;
    AVFrame *preFrame = nullptr;
    AVFrame *freeFrame = nullptr;
    AVFrame *pFrame = nullptr;
    int windowWidth = width;
    int windowHeight = height;
    std::vector<AVFrame *> decodeFrames;
	while (true) {
        if (freeFrame != nullptr){
            av_frame_free(&freeFrame);
            assert(freeFrame == nullptr);
        }

        if (decodeFrames.size() == index){
            decodeFrames.clear();
            std::unique_lock<std::mutex> lock(m);
            if (!frames.empty()){
                decodeFrames.swap(frames);
                index = 0;
            }
        }
		SDL_WaitEvent(&event);
		if(event.type == LOADPIC_EVENT){
            if (decodeFrames.size() >= index + 1){
                pFrame = decodeFrames[index];
                index++;
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

            SDL_UpdateTexture(sdlTexture, nullptr, buf, pFrame->linesize[0]);
            sdlRect.x = 0;  
            sdlRect.y = 0;
            sdlRect.w = windowWidth;
            sdlRect.h = windowHeight;
            SDL_RenderClear(sdlRenderer);
            SDL_RenderCopy(sdlRenderer, sdlTexture, nullptr, &sdlRect); 
            SDL_RenderPresent(sdlRenderer);
            freeFrame = preFrame;
            preFrame = pFrame;
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
}