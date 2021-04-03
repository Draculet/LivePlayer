#include "FlvHeader.h"
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
using namespace std;

int main(int argc, char* argv[]){
    int fd = open(argv[1], O_RDONLY);
    int fd2 = open("test.ph264", O_WRONLY|O_CREAT|O_TRUNC);
    if (fd2 < 0){
        perror("open");
        return -1;
    }
    unsigned char buf[1024] = {0};
    size_t ret = 0;
    evbuffer *evbuf = evbuffer_new();
    Parser parser;
    while (true){
        //printf("ok\n");
        memset(buf, 0, 1024);
        ret = read(fd, buf, 1024);
        if (ret > 0){
            //printf("read ret %ld\n", ret);
            evbuffer_add(evbuf, buf, ret);
        }
        else break;
        /*
        for (int i = 0; i < 100; i++){
            printf("%0x ", buf[i]);
        }
        printf("\n");
        if (ret < 0){
            break;
        }
        printf("ret: %ld\n", ret);
        evbuffer_add(evbuf, buf, 1024);
        char cbuf[1024] = {0};
        evbuffer_remove(evbuf, cbuf, 1024);
        for (int i = 0; i < 100; i++){
            printf("%0x ", cbuf[i]);
        }
        printf("\n");
        return 0;
        */
        //printf("parseflv\n");
        parser.ParseFlv(evbuf);
    }
    parser.flvheader->printfInfo();
    for (auto tag : parser.flvtags){
        if (tag->header->type == 0x09){
            VideoTag *t = (VideoTag *)tag;
            if (t->getH264Stream() != nullptr){
                int totalsize = evbuffer_get_length(t->getH264Stream());
                evbuffer_prepend(t->getH264Stream(), &totalsize, 4);
                evbuffer_write(t->getH264Stream(), fd2);
            }
            ///printf("h264 stream len: %ld\n", evbuffer_get_length(t->getH264Stream()));
        }
        //tag->printfInfo();
    }
    close(fd);
    close(fd2);
    return 0;
}