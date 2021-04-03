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
#include "json.hpp"
using namespace std;
using namespace nlohmann;

vector<evhttp_connection *> conns;
char *requrl;
struct event_base* base;
struct evdns_base* dnsbase;

size_t roomid = 0;

void RequestCallback(struct evhttp_request* req, void* arg);
void ChunkCallback(struct evhttp_request* req, void* arg);
void ErrorCallback(enum evhttp_request_error error, void* arg);
void ConnectionCloseCallback(struct evhttp_connection* connection, void* arg);
int HeaderDoneCallback(struct evhttp_request* req, void* arg);

string ltos(size_t num){
    char ch;
    string s;
    while (num){
        ch = (num % 10) + '0';
        num /= 10;
        s = string(&ch, 1) + s;
    }
    return s;
}

void RequestCallback(struct evhttp_request* req, void* arg)
{
    //GET /xlive/web-room/v1/playUrl/playUrl?cid=23058&qn=10000&platform=web&https_url_req=1&ptype=16 HTTP/1.1
    string url = string("https://api.live.bilibili.com/xlive/web-room/v1/playUrl/playUrl?cid=");
    url += ltos(roomid);
    url += "&qn=10000&platform=web&https_url_req=0&ptype=16";
    cout << url << endl;
    struct evhttp_uri* uri = evhttp_uri_parse(url.c_str());
    struct evhttp_request* request = evhttp_request_new([](evhttp_request *req, void *arg){
        event_base_loopexit((event_base*)arg, nullptr);
    }, base);
    evhttp_request_set_header_cb(request, HeaderDoneCallback);
    evhttp_request_set_chunked_cb(request, [](struct evhttp_request* req, void* arg){
        int len = atoi(evhttp_find_header(evhttp_request_get_input_headers(req), "Content-Length"));
        char buf[len] = {0};
        struct evbuffer* evbuf = evhttp_request_get_input_buffer(req);
        if (evbuffer_get_length(evbuf) >= len){
            evbuffer_remove(evbuf, buf, len);
        }
        cout << string(buf, len) << endl;
        json j = json::parse(string(buf, len));
        for (int i = 0; i < j["data"]["durl"].size(); i++)
            cout << j["data"]["durl"][i]["url"] << endl;
        //cout << string(buf, len) << endl;
        cout << "chunk end" << endl;
    });
    evhttp_request_set_error_cb(request, ErrorCallback);
    const char* host = evhttp_uri_get_host(uri);
    int port = evhttp_uri_get_port(uri);
    if (port < 0) port = 80;
    const char* request_url = url.c_str();
    const char* path = evhttp_uri_get_path(uri);
    printf("host: %s. path: %s. request_url: %s. port: %d\n", host, path, request_url, port);
    //json j;
    //j["cid"] = roomid;
    //j["qn"] = 10000;
    //j["platform"] = string("web");
    //j["https_url_req"] = 1;
    //j["ptype"] = 16;
    //string data = j.dump();
    //cout << "post data: " << data << endl;
    //printf("url:%s host:%s port:%d path:%s request_url:%s\n", url, host, port, path, request_url);
    evhttp_connection *connection = evhttp_connection_base_new(base, dnsbase, host, port);
    conns.push_back(connection);
    //evhttp_connection_free_on_completion(connection);
    
    //cout << "set-cookie: " << string(evhttp_find_header(evhttp_request_get_input_headers(req), "set-cookie")) << endl;
    evhttp_add_header(evhttp_request_get_output_headers(request), "cookie", evhttp_find_header(evhttp_request_get_input_headers(req), "set-cookie"));
    evhttp_add_header(evhttp_request_get_output_headers(request), "Connection", "close");
    evhttp_add_header(evhttp_request_get_output_headers(request), "Host", host);
    evhttp_uri_free(uri);
    //evbuffer_add(evhttp_request_get_output_buffer(request), data.c_str(), data.size());
    evhttp_make_request(connection, request, EVHTTP_REQ_GET, request_url);
} 

int HeaderDoneCallback(struct evhttp_request* req, void* arg)
{
    printf("header callback\n");
    fprintf(stderr, "< HTTP/1.1 %d %s\n", evhttp_request_get_response_code(req), evhttp_request_get_response_code_line(req));
    struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    struct evkeyval* header;
    TAILQ_FOREACH(header, headers, next)
    {
        fprintf(stderr, "< %s: %s\n", header->key, header->value);
    }
    fprintf(stderr, "< \n");
    return 0;
}

void ChunkCallback(struct evhttp_request* req, void* arg)
{
    printf("chunk callback\n");
    printf("chunk input len: %ld\n", evbuffer_get_length(req->input_buffer));
    int len = atoi(evhttp_find_header(evhttp_request_get_input_headers(req), "Content-Length"));
    char buf[len] = {0};
    struct evbuffer* evbuf = evhttp_request_get_input_buffer(req);
    if (evbuffer_get_length(evbuf) >= len){
        evbuffer_remove(evbuf, buf, len);
    }
    cout << string(buf, len) << endl;
    json j = json::parse(string(buf, len));
    roomid = j["data"]["room_id"];
}

void ErrorCallback(enum evhttp_request_error error, void* arg)
{
    printf("error callback\n");
    fprintf(stderr, "request failed\n");
    event_base_loopexit((struct event_base*)arg, NULL);
}

void ConnectionCloseCallback(struct evhttp_connection* connection, void* arg)
{
    printf("connection close callback\n");
}

int main(int argc, char** argv)
{
    string url = string("https://api.live.bilibili.com/room/v1/Room/room_init?id=") + argv[1];
    struct evhttp_uri* uri = evhttp_uri_parse(url.c_str());
    base = event_base_new();
    dnsbase = evdns_base_new(base, 1);
    assert(dnsbase);
    struct evhttp_request* request = evhttp_request_new(RequestCallback, base);
    evhttp_request_set_header_cb(request, HeaderDoneCallback);
    evhttp_request_set_chunked_cb(request, ChunkCallback);
    evhttp_request_set_error_cb(request, ErrorCallback);
    const char* host = evhttp_uri_get_host(uri);
    int port = evhttp_uri_get_port(uri);
    if (port < 0) port = 80;
    const char* request_url = url.c_str();
    const char* path = evhttp_uri_get_path(uri);
    //printf("url:%s host:%s port:%d path:%s request_url:%s\n", url, host, port, path, request_url);
    evhttp_connection* connection =  evhttp_connection_base_new(base, dnsbase, host, port);
    conns.push_back(connection);
    //evhttp_connection_set_closecb(connection, ConnectionCloseCallback, base);
    evhttp_add_header(evhttp_request_get_output_headers(request), "Host", host);
    evhttp_add_header(evhttp_request_get_output_headers(request), "Connection", "close");
    evhttp_uri_free(uri);
    evhttp_make_request(connection, request, EVHTTP_REQ_GET, request_url);

    event_base_dispatch(base);
    for (auto conn: conns)
        evhttp_connection_free(conn);
    printf("event end\n");
    return 0;
}