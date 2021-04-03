#ifndef __FLV_PARSER_H__
#define __FLV_PARSER_H__
#include <event2/buffer.h>
#include <vector>
#include <stdio.h>
#include <assert.h>

#define FLV_EAGIN -2
#define FLV_ERROR -1

//进行大小端转换并取值
unsigned int getU32(evbuffer *buf) {
    unsigned char pbuf[4] = {0}; //注意unsigned char才是回环左移
    evbuffer_remove(buf, pbuf, 4);
    return (pbuf[0] << 24) | (pbuf[1] << 16) | (pbuf[2] << 8) | pbuf[3];
}

unsigned int getU32(unsigned char *pbuf) {
    return (pbuf[0] << 24) | (pbuf[1] << 16) | (pbuf[2] << 8) | pbuf[3];
}

unsigned int getU24(evbuffer *buf) {
    unsigned char pbuf[3] = {0};
    evbuffer_remove(buf, pbuf, 3);
    return (pbuf[0] << 16) | (pbuf[1] << 8) | (pbuf[2]); 
}

unsigned int getU24(unsigned char *pbuf) {
    return (pbuf[0] << 16) | (pbuf[1] << 8) | (pbuf[2]); 
}

unsigned int getU16(evbuffer *buf) {
    unsigned char pbuf[2] = {0};
    evbuffer_remove(buf, pbuf, 2);
    return (pbuf[0] << 8) | (pbuf[1]); 
}

unsigned int getU16(unsigned char *pbuf) {
    return (pbuf[0] << 8) | (pbuf[1]); 
}

unsigned int getU8(evbuffer *buf) {
    unsigned char pbuf[1] = {0};
    evbuffer_remove(buf, pbuf, 1);
    return (pbuf[0]); 
}

unsigned int getU8(unsigned char *pbuf) {
    return (pbuf[0]); 
}

bool ensure(evbuffer *buf, int len){
    return evbuffer_get_length(buf) >= len;
}

struct TagHeader{
    int type = -1;
    int dataSize = -1;
    int timeStamp = -1;
    int ext = -1;
    unsigned int wholePts = 0; //timestamp和ext
    int streamId = -1;
};

class Tag {
    public:
    Tag()
      :header(new TagHeader()){
       tagData = evbuffer_new();            
    }

    //需保证tagData已经有数据
    virtual void parseTag(){
        //TODO
    }

    void printfInfo(){
        printf("tag header info. type: %d. datasize: %d. timestamp: %d. ext: %d. whilepts: %d. streamId = %d. tagdata len: %ld.\n",
            header->type, header->dataSize, header->timeStamp, header->ext, header->wholePts, header->streamId, evbuffer_get_length(tagData));
    }

    TagHeader *header = nullptr;
    evbuffer *tagData = nullptr;
    int preTagLen = -1;
};

class ScriptTag : public Tag {
    public:
    virtual void parseTag() {
        //TODO
        Tag::parseTag();
    }
};

class VideoTag : public Tag {
    public:
    VideoTag(){}
    virtual ~VideoTag(){}

    evbuffer *getH264Stream(){
        if (h264Stream && evbuffer_get_length(tagData) == 0)
            return h264Stream;
        else
            return nullptr;
    }

    //private:
    virtual void parseTag() {
        if (evbuffer_get_length(tagData) == header->dataSize && header->type == 0x09){
            char cbuf[2] = {0};
            evbuffer_remove(tagData, cbuf, 2);
            frameType = (cbuf[0] & 0xf0) >> 4;
            codecId = cbuf[0] & 0x0f;
            packetType = cbuf[1];
            compositionTime = getU24(tagData);
            if (codecId == 0x07){
                h264Stream = evbuffer_new();
                parseH264Stream();
            }
        }
    }

    void parseH264Stream(){
        if (evbuffer_get_length(tagData) == header->dataSize - 5){
            if (packetType == 0x00){
                unsigned char cbuf[6] = {0};
                //略过了version profile compatibility level字段
                evbuffer_remove(tagData, cbuf, 6);
                //FIXME nalu_len为4时正确,其他情况尚未测试
                VideoTag::nalu_length = (cbuf[4] & 0x03) + 1;
                //FIXME sps_num > 1 的情况尚未测试
                int sps_num = cbuf[5] & 0x03;
                for (int i = 0; i < sps_num; i++){
                    evbuffer_add(h264Stream, (void*)&h264StartCode, 4);
                    int sps_size = getU16(tagData);
                    printf("sps size: %d\n", sps_size);
                    evbuffer_remove_buffer(tagData, h264Stream, sps_size);
                }
                char tmpbuf[1];
                evbuffer_remove(tagData, tmpbuf, 1);
                int pps_num = tmpbuf[0] & 0x03;
                //FIXME pps_num > 1 的情况尚未测试
                for (int i = 0; i < sps_num; i++){
                    evbuffer_add(h264Stream, (void*)&h264StartCode, 4);
                    int pps_size = getU16(tagData);
                    evbuffer_remove_buffer(tagData, h264Stream, pps_size);
                }
                //FIXME 到这个位置,有些flv文件可能有多余的data,其并不是sps或pps
                evbuffer_drain(tagData, evbuffer_get_length(tagData));
                //assert(evbuffer_get_length(tagData) == 0);
            } else if(packetType == 0x01){
                while (evbuffer_get_length(tagData) != 0){
                    int nalu_size = 0;
                    if (VideoTag::nalu_length == 4)
                        nalu_size = getU32(tagData);
                    else if (VideoTag::nalu_length == 3)
                        nalu_size = getU24(tagData);
                    else if (VideoTag::nalu_length == 2)
                        nalu_size = getU16(tagData);
                    else if (VideoTag::nalu_length == 1)
                        nalu_size = getU8(tagData);
                    int ret = 0;
                    evbuffer_add(h264Stream, (void*)&h264StartCode, 4);
                    ret = evbuffer_remove_buffer(tagData, h264Stream, nalu_size);
                    //TODO 0x06 0x05开头可以读取SEI信息
                }
                assert(evbuffer_get_length(tagData) == 0);
            }
        }
    }

    int frameType = -1;
    int codecId = -1;
    int packetType = -1;
    int compositionTime = -1;
    static int nalu_length;
    evbuffer *h264Stream = nullptr;
    const int h264StartCode = 0x01000000;
};

int VideoTag::nalu_length = 0;

class AudioTag : public Tag{
    virtual void parseTag() {
        //TODO
        Tag::parseTag();
    }
};

struct FlvHeader {
    //FLV signature == "FLV"
    FlvHeader()
        :version(-1),
        videoFlag(-1),
        audioFlag(-1),
        dataOffset(-1)
    {}

    FlvHeader(int ver, int videoflag, int audioflag, int offset)
        :version(ver),
        videoFlag(videoflag),
        audioFlag(audioflag),
        dataOffset(offset)
    {}
    
    void printfInfo(){
        printf("flvheader info: version: %d. videoFlag: %d. audioFlag: %d. dataoffset: %d.\n",
        version, videoFlag, audioFlag, dataOffset);    
    }

    int version;
    int videoFlag; 
    int audioFlag;
    int dataOffset;
    //previous tag len == 0
};

class Parser{
private:
    Tag *curTag = nullptr;
public:
    Parser(/* args */){}
    ~ Parser(){}
    std::vector<Tag *> flvtags;
    FlvHeader *flvheader = nullptr;

    int ParseFlv(evbuffer *buf){
        if (!flvheader){
            if (!ensure(buf, 9 + 4)) return FLV_EAGIN;
            unsigned char cbuf[13] = {0};
            evbuffer_remove(buf, cbuf, 9 + 4);            
            flvheader = new FlvHeader(static_cast<int>(cbuf[3]),
                static_cast<int>((cbuf[4] >> 2) & 0x01),
                    static_cast<int>((cbuf[4] >> 0) & 0x01),
                        getU32(cbuf + 5));
            flvheader->printfInfo();
        }
        //parse flv tag
        while (true){
            if (!curTag){
                if (!ensure(buf, 11)) return FLV_EAGIN;
                unsigned char test[11] = {0};
                evbuffer_copyout(buf, test, 11);
                int type = getU8(buf);
                int datasize = getU24(buf);
                int timestamp = getU24(buf);
                int ext = getU8(buf);
                int streanid = getU24(buf);
                unsigned int wholepts = (ext << 24) + timestamp;
                switch (type)
                {
                case 0x09:
                    curTag = new VideoTag();
                    break;
                case 0x08:
                    curTag = new AudioTag();
                    break;
                case 0x12:
                    curTag = new ScriptTag();
                    break;
                default:
                    curTag = new Tag();
                    break;
                }
                curTag->header->type = type;
                curTag->header->dataSize = datasize;
                curTag->header->timeStamp = timestamp;
                curTag->header->ext = ext;
                curTag->header->streamId = streanid;
                curTag->header->wholePts = wholepts;
            }
            
            if (curTag->header->dataSize + 4 >= evbuffer_get_length(buf)){
                return FLV_EAGIN;
            } else {
                evbuffer_remove_buffer(buf, curTag->tagData, curTag->header->dataSize);
                curTag->printfInfo();
                curTag->parseTag();
                curTag->preTagLen = getU32(buf);
                flvtags.push_back(curTag);
                curTag = nullptr;
            }
        }/* end loop */
    }/* end praseflv */
};

#endif