#include <stdio.h>
#include <unistd.h>
extern "C"
{
#include "libavcodec/avcodec.h"
//#include "libswscale/swscale.h"
//#include <libavutil/imgutils.h>
#include <SDL2/SDL.h>
#include <libavutil/imgutils.h>
};
#include <arpa/inet.h> 
#include <string>
#include <fcntl.h>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <event2/buffer.h>

#define LOADPIC_EVENT  (SDL_USEREVENT + 1)
#define BREAK_EVENT  (SDL_USEREVENT + 2)
int w = 1920;
int h = 1080;
bool quit = false;
int threadfunc(void *opaque){
	while (!quit) {
		SDL_Event event;
		event.type = LOADPIC_EVENT;
		SDL_PushEvent(&event);
        SDL_Delay(40);
    }
	return 0;
}

void decode(/*SwsContext *img_convert_ctx,*/ AVCodecContext *dec_ctx, AVPacket *pkt,
        std::mutex &m, std::condition_variable &cv, std::vector<AVFrame *> &frames){
    //if (pkt == nullptr) return;
    //for (int i = 0; i < 200; i++){
    //    printf("%02x ", pkt->data[i]);
    //}
    //printf("\n");
    printf("%d X %d\n", dec_ctx->width, dec_ctx->height);
    printf("pix_fmt: %d\n", dec_ctx->pix_fmt);
    
    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        //exit(1);
    }
    else printf("ok\n");
    bool once = true;
    while (ret >= 0) {
        AVFrame *frame = av_frame_alloc();
        //AVFrame *pFrameYUV = av_frame_alloc();
        //uint8_t *out_buffer=(uint8_t *)av_malloc(avpicture_get_size(AV_PIX_FMT_YUV420P, w, h));
	    //av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, w, h, 1);
        //if (!frame) {
        //    fprintf(stderr, "Could not allocate video frame\n");
        //    exit(1);
        //}
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            //av_frame_free(&frame);
            //printf("recv failed\n");
            return;
        }
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }
        
        //sws_scale(img_convert_ctx, (const uint8_t* const*)frame->data, frame->linesize, 0, h, 
        //        pFrameYUV->data, pFrameYUV->linesize);
        //write(fd2, frame->data[0], h*w);
        //write(fd2, frame->data[1], h*w/4);
        //write(fd2, frame->data[2], h*w/4);
        {
            std::unique_lock<std::mutex> lock(m);
            printf("thread push index %ld\n", frames.size());
            //printf("test2 %d\n",frame->data[0][0]);
            frames.push_back(frame);
        }
        if (once){
            cv.notify_one();
            once = false;
        }
    }
}

void DecodeThreadFunc(AVCodecContext *c, std::vector<AVFrame *> &frames, 
            std::mutex &m, std::condition_variable &cv, std::string filepath){
    
    const AVCodec *codec;
    AVCodecParserContext *parser = nullptr;
    int fd;
    //uint8_t inputbuf[4096];//注意input需要多出AV_INPUT_BUFFER_PADDING_SIZE
    int ret;
    AVPacket *pkt;
    struct SwsContext *img_convert_ctx;

    pkt = av_packet_alloc(); if (!pkt) exit(1);

    /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
    //memset(inputbuf, 0, sizeof(inputbuf));
    avcodec_register_all();
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    //parser = av_parser_init(codec->id);
    //if (!parser) {
    //    fprintf(stderr, "parser not found\n");
    //    exit(1);
    //}
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */
    //if (avcodec_open2(c, codec, NULL) < 0) {
    //    fprintf(stderr, "Could not open codec\n");
    //    exit(1);
    //}

    //img_convert_ctx = sws_getContext(w, h, AV_PIX_FMT_YUVJ420P,
	//	w, h, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

    fd = open(filepath.c_str(), O_RDONLY);
    if (!fd) {
        fprintf(stderr, "Could not open %s\n", filepath.c_str());
        exit(1);
    }
    size_t inputsize;
    //evbuffer *buf = evbuffer_new();
    bool once = true;
    bool sec = false;
    uint8_t *sps;
    int spssize = 0;
    while (true) {
        int size = 0;
        read(fd, &size, 4);
        int pktsize = size;
        printf("pkt size: %d\n", pktsize);
        uint8_t cbuf[pktsize] = {0};
        ret = 0;
        while (size != 0){
            ret = read(fd, cbuf + ret, size);
            if (ret <= 0) break;
            size -= ret;
        }
        if (ret <= 0) break;
        if (once){
            spssize = pktsize;
            //sps = new uint8_t(pktsize);
            spssize = 44;
            uint8_t sps[44] = {0x01, 0x64, 0x00, 0x28, 0xff, 0xe1, 0x00, 0x1b, 0x67,
                 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0x78, 0x02, 0x27, 0xe5, 0xc0, 0x5b,
                    0x20, 0x00, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x07, 0x81, 0xe3,
                         0x06, 0x32, 0xc0, 0x01, 0x00, 0x06, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0};
            c->extradata_size = spssize;
            c->extradata = (uint8_t*)av_malloc(spssize + FF_INPUT_BUFFER_PADDING_SIZE);
            memset(c->extradata, 0, sizeof(c->extradata));
            //memcpy(sps, cbuf, spssize);
            memcpy(c->extradata, sps, spssize);
            once = false;
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(c, codec, nullptr) < 0) {
                printf("avodec_error\n");                           
            }
            printf("pic size: %d x %d\n", c->width, c->height);
            for (int i = 0; i < spssize; i++){
                printf("%02x ", sps[i]);
            }
            printf("\n");
            //continue;
        }
        uint8_t *data = nullptr;
        if (sec){
            //SEI后加sps pps
            for (int i = 5; i < pktsize; i++){
                if (cbuf[i] == 0x00 && cbuf[i+1] == 0x00
                    && cbuf[i + 2] == 0x00 && cbuf[i + 3] == 0x01){
                    printf("found %d\n", i);
                    data = new uint8_t[spssize + pktsize - 1];
                    memcpy(data, cbuf, i);
                    memcpy(data + i, sps, spssize);
                    memcpy(data + i + spssize, cbuf + i + 1, pktsize - i - 1);
                    pktsize = pktsize + spssize - 1;
                    break;
                }
            }
            sec = false;
        }
        //inputsize = read(fd, inputbuf, 4096);
        pkt = av_packet_alloc();
        
        /*for (int i = 0; i < 200; i++){
            printf("%02x ", cbuf[i]);
        }*/
        //av_packet_from_data(pkt, cbuf, pktsize);
        if (data != nullptr)
            pkt->data = data;
        else
            pkt->data = cbuf;
        pkt->size = pktsize;
        printf("packet start: size: %d\n", pkt->size);
        
        /*
        for (int i = 0; i < pkt->size; i++){
            printf("%02x ", pkt->data[i]);
        }
        printf("\n");
        */

        decode(/*img_convert_ctx,*/ c, pkt, m, cv, frames);
    }
    //FIXME 该步用于刷新解码器,是否需要刷新解码器
    decode(/*img_convert_ctx,*/ c, nullptr, m, cv, frames);

    std::unique_lock<std::mutex> lock(m);
    //标志播放结束
    printf("push nullptr\n");
    frames.push_back(nullptr);
}

int main(int argc, char* argv[])
{
	int	i, videoindex;
	int ret;
    //int fd2 = open("example.yuv", O_WRONLY|O_CREAT|O_TRUNC);
	FILE *fd2 = fopen("example.yuv", "wb");
    //struct SwsContext *img_convert_ctx;
	std::string filepath = std::string(argv[1]);
    
    std::mutex m;
    std::condition_variable cv;
    std::vector<AVFrame *> frames;
    AVCodecContext *c = nullptr;
    std::thread t([&c, &m, &frames, &cv, filepath]{
        DecodeThreadFunc(c, frames, m, cv, filepath);
    });
    //t.join();
    //exit(-1);
    //SDL Init
    if(SDL_Init(SDL_INIT_VIDEO)) {  
		printf( "Could not initialize SDL - %s\n", SDL_GetError()); 
		return -1;
	}
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&frames]{return !frames.empty();});
    }
    printf("before\n");
    //printf("width: , height: \n", c->width, c->height);
    //exit(0);
    SDL_Window *screen; 
	//SDL 2.0 Support for multiple windows
	screen = SDL_CreateWindow("SDL2 player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		w, h, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
	if(!screen) {
		printf("SDL: could not create window - exiting:%s\n",SDL_GetError());  
		return -1;
	}
	SDL_Renderer* sdlRenderer = SDL_CreateRenderer(screen, -1, 0);  
	//IYUV: Y + U + V  (3 planes)
	//YV12: Y + V + U  (3 planes)
	SDL_Texture* sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_Rect sdlRect;
	SDL_Event event;
	//pFrameYUV=av_frame_alloc();
    //out_buffer=(uint8_t *)av_malloc(avpicture_get_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height));
	//av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1);
	//packet=(AVPacket *)av_malloc(sizeof(AVPacket));
	
	//img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
	//	pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
    uint8_t buf[w * h * 3 / 2] = {0};
    SDL_Thread *refresh_thread = SDL_CreateThread(threadfunc,NULL,NULL);
	int index = 0;
    AVFrame *pFrame = nullptr;
    //int h,w;
	while (true) {
		SDL_WaitEvent(&event);
		if(event.type == LOADPIC_EVENT){
            printf("enter\n");
            {
                std::unique_lock<std::mutex> lock(m);
                printf("ok\n");
                if (frames.size() >= index + 1){
                    printf("ok2 size %ld index: %d\n", frames.size(), index);
                    pFrame = frames[index];
                    //printf("test %d\n",pFrame->data[0][0]);
                    index++;
                } else {
                    printf("ok3\n");
                    pFrame = frames[index - 1]; //播放上一帧
                }
            }
            if (pFrame == nullptr){
                break;
            }
            //h = c->height, w = c->width;
            printf("test: %dx%d\n", h, w);
            w = 1920;
            h = 1080;
            fwrite(pFrame->data[0], 1, h * w, fd2);
            fwrite(pFrame->data[1], 1, h * w / 4, fd2);
            fwrite(pFrame->data[2], 1, h * w / 4, fd2);
            //write(fd2, pFrame->data[0], h * w);
            //write(fd2, pFrame->data[1], h * w / 4);
            //write(fd2, pFrame->data[2], h * w / 4);
            memcpy(buf, pFrame->data[0], h * w);
            memcpy(buf + w * h, pFrame->data[1], h * w / 4);
            memcpy(buf + w * h * 5 / 4, pFrame->data[2], h * w / 4);

            printf("SDL_UpdateTexture\n");
            SDL_UpdateTexture( sdlTexture, NULL, buf, pFrame->linesize[0]);
            //FIX: If window is resize
            sdlRect.x = 0;  
            sdlRect.y = 0;
            sdlRect.w = w;
            sdlRect.h = h;
            printf("SDL_RenderClear\n");
            SDL_RenderClear( sdlRenderer );
            printf("SDL_RenderCopy\n");
            SDL_RenderCopy( sdlRenderer, sdlTexture, NULL, &sdlRect); 
            printf("SDL_RenderPresent\n");
            SDL_RenderPresent( sdlRenderer );
		} else if(event.type == SDL_WINDOWEVENT){
			//SDL_GetWindowSize(screen, &w, &h);
		} else if(event.type == SDL_QUIT){
			quit = true;
            printf("SDL_QUIT EVENT\n");
            break;
		}
	}
	SDL_Quit();

    t.join();

	//sws_freeContext(img_convert_ctx);
	return 0;
}

