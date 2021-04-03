#include "event2/http.h"
#include "event2/http_struct.h"
#include "event2/event.h"
#include "event2/buffer.h"
#include "event2/dns.h"
#include "event2/thread.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/queue.h>
#include <event.h>
#include <unistd.h>
#include <string>
#include <iostream>
#include <functional>
#include "json.hpp"
using namespace std;
using namespace nlohmann;

class ReqContext{
    public:
    ~ReqContext(){
        for (auto conn: conns){
            evhttp_connection_free(conn);
        }
        if (base) event_base_free(base);
        if (dnsbase) evdns_base_free(dnsbase, 0);
        conns.clear();
    }

    vector<string> RequestForUrl(string roomid, string pf){
        if (!(pf == "web" || pf == "h5") || roomid.size() == 0){
            printf("platform error or roomid error\n");
            return vector<string>();
        }
        platform = pf;
        string url = string("http://api.live.bilibili.com/room/v1/Room/room_init?id=") + roomid;
        struct evhttp_uri* uri = evhttp_uri_parse(url.c_str());
        if (!base)
            base = event_base_new();
        if (!dnsbase)
            dnsbase = evdns_base_new(base, 1);
        assert(dnsbase);
        //function<void(struct evhttp_request*, void*)> func = bind(&ReqContext::RequestForRoomMember, this, placeholders::_1, placeholders::_2);
        struct evhttp_request* request = evhttp_request_new(RequestForRoom, this);
        evhttp_request_set_chunked_cb(request, [](evhttp_request *req, void *arg){
            ReqContext *ctx = (ReqContext *)arg;
            int len = atoi(evhttp_find_header(evhttp_request_get_input_headers(req), "Content-Length"));
            char buf[len] = {0};
            struct evbuffer* evbuf = evhttp_request_get_input_buffer(req);
            if (evbuffer_get_length(evbuf) >= len){
                evbuffer_remove(evbuf, buf, len);
            }
            json j = json::parse(string(buf, len));
            if (j["data"]["live_status"] != 1){
                printf("no live\n");
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
            printf("error: room_id is zero\n");
            event_base_loopbreak(ctx->base);
        }
        string url = string("http://api.live.bilibili.com/xlive/web-room/v1/playUrl/playUrl?cid=");
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
            char buf[len] = {0};
            struct evbuffer* evbuf = evhttp_request_get_input_buffer(req);
            if (evbuffer_get_length(evbuf) >= len){
                evbuffer_remove(evbuf, buf, len);
            }
            json j = json::parse(string(buf, len));
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

    void StartPullStream(string url){
        struct evhttp_uri* uri = evhttp_uri_parse(url.c_str());
        if (!base)
            base = event_base_new();
        if (!dnsbase)
            dnsbase = evdns_base_new(base, 1);
        assert(dnsbase);
        //function<void(struct evhttp_request*, void*)> func = bind(&ReqContext::RequestForRoomMember, this, placeholders::_1, placeholders::_2);
        struct evhttp_request* request = evhttp_request_new(RequestForRoom, this);
        evhttp_request_set_chunked_cb(request, [](evhttp_request *req, void *arg){
            struct evbuffer* evbuf = evhttp_request_get_input_buffer(req);
            int len = evbuffer_get_length(evbuf);
            uint8_t buf[len] = {0};
            evbuffer_remove(evbuf, buf, len);
            /*for (int i = 0; i < len; i++){
                printf("%02x ", buf[i]);
            }*/
            printf("\n");
        });
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
        const char* request_url = url.c_str();
        const char* path = evhttp_uri_get_path(uri);
        evhttp_connection* connection =  evhttp_connection_base_new(base, dnsbase, host, port);
        conns.push_back(connection);
        evhttp_add_header(evhttp_request_get_output_headers(request), "Host", host);
        evhttp_add_header(evhttp_request_get_output_headers(request), "Connection", "close");
        evhttp_uri_free(uri);
        evhttp_make_request(connection, request, EVHTTP_REQ_GET, request_url);

        event_base_dispatch(base);
    }

    static string ltos(size_t num){
        char ch;
        string s;
        while (num){
            ch = (num % 10) + '0';
            num /= 10;
            s = string(&ch, 1) + s;
        }
        return s;
    }
    
    size_t room_id = 0;
    string platform;
    vector<evhttp_connection *> conns;
    vector<string> urls;
    event_base *base = nullptr;
    evdns_base* dnsbase = nullptr;
};

int main(int argc, char** argv)
{
    ReqContext ctx;
    auto urls = ctx.RequestForUrl(argv[1], argv[2]);
    for (auto url : urls)
        printf("%s\n", url.c_str());
    ctx.StartPullStream(urls[0]);
    return 0;
}