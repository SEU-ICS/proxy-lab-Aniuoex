#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
//cache
#define CACHE_BLOCK 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";



typedef char string[MAXLINE];
typedef struct url_t{
    string host;
    string port;
    string path;
};
typedef struct{
    string url;
    char object[MAX_OBJECT_SIZE];
    int size;
    int timestamp;
}cache_file;
typedef struct{
    int using_cache_num;
    cache_file cache[CACHE_BLOCK];
}cache_t;



void* thread(void *vargp);
void do_get(rio_t* client_rio_p, string url);
int parse_url(string url, struct url_t* url_p);
void build_header(rio_t* client_rio_p, string header_info, string host);

//cache
void init_cache();
int find_cache(rio_t* rio_p, string url);
int write_cache(string url, char * content, int size);
//线程间共享变量
static cache_t cache;
static sem_t cache_mutex, writer;
static int reader_count, timestamp;


int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    //监听套接字  已连接套接字
    int listenfd, *connfd;
    socklen_t client_len;
    string hostname, port;
    struct sockaddr_storage client_addr; //后续写入客户端地址
    pthread_t tid;
    if (argc != 2){
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    listenfd = Open_listenfd(argv[1]);
    init_cache();
    if (listenfd < 0) {
        perror("Open_listenfd error");
        exit(1);
    }
    while(1){
        client_len = sizeof(client_addr);
        connfd = (int *) malloc(sizeof(int));
        *connfd = accept(listenfd, (SA *)& client_addr, &client_len);
        if(*connfd <0)
        {
            fprintf(stderr, "Accept error: %s\n", strerror(errno));
            continue;
        }
        Getnameinfo((SA *)&client_addr, client_len, hostname, MAXLINE, port, MAXLINE,0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        pthread_create(&tid, NULL, thread, connfd);
    }
    close(listenfd);
    return 0;
}

void *thread(void *vargp){
    //分离自身线程
    pthread_detach(pthread_self());
    int clientfd = *((int *)vargp);
    free(vargp);
    rio_t client_rio;
    string buf;
    rio_readinitb(&client_rio, clientfd);
    if(rio_readlineb(&client_rio, buf, MAXLINE) <= 0){
        close(clientfd);
        return NULL;
    }
    string method, url, version;
    if(sscanf(buf, "%s %s %s", method, url, version) != 3){
        fprintf(stderr, "Proxy received a malformed request");
        close(clientfd);
        return NULL;
    }
    if(!strcasecmp(method, "GET"))
    {
        do_get(&client_rio, url);
    }
    else{
        fprintf(stderr, "Proxy does not implement the method");
    }
    close(clientfd);
    return NULL;
}

void do_get(rio_t* client_rio_p, string url){
    if(find_cache(client_rio_p, url) == 1)
        return;

    struct url_t url_info;
    if (parse_url(url, &url_info) < 0) {
        fprintf(stderr, "Parse url error\n");
        return;
    }  
    string header;
    build_header(client_rio_p, header, url_info.host);
    int server_fd = open_clientfd(url_info.host, url_info.port);
    if(server_fd < 0){
        fprintf(stderr, "Connection to server failed\n");
        return;
    }
    rio_t server_rio;
    rio_readinitb(&server_rio, server_fd);
    //向服务器转发请求
    string buf;
    sprintf(buf, "GET %s HTTP/1.0\r\n%s", url_info.path, header);
    if(rio_writen(server_fd, buf, strlen(buf)) != strlen(buf)){
        fprintf(stderr, "Forwarding request to server failed\n");
        close(server_fd);
        return;
    }
    int resp_total = 0 , resp_current = 0;
    char file_cache[MAX_OBJECT_SIZE];
    int client_fd = client_rio_p->rio_fd;

    while((resp_current = rio_readnb(&server_rio, buf, MAXLINE))){
        if(resp_current < 0){
            fprintf(stderr, "Reading response from server failed\n");
            close(server_fd);
            return;
        }
        if(resp_total + resp_current < MAX_OBJECT_SIZE)
            memcpy(file_cache + resp_total, buf, resp_current);
        resp_total += resp_current;
        if(rio_writen(client_fd, buf, resp_current) != resp_current){
            fprintf(stderr, "Writing response to client failed\n");
            close(server_fd);
            return;
        }
    }
    if(resp_total < MAX_OBJECT_SIZE)
        write_cache(url, file_cache, resp_total);
    close(server_fd);
    return;
}

int parse_url(string url, struct url_t* url_p){
    const int http_prefix_len = strlen("http://");
    if(strncasecmp(url, "http://", http_prefix_len) != 0){
        fprintf(stderr, "Only HTTP protocol is supported");
        return -1;
    }
    char* host_start = url + http_prefix_len;
    char* port_start = strchr(host_start, ':');
    char* path_start = strchr(host_start, '/');
    if(path_start ==NULL)
        return -1;
    if(port_start ==NULL)
    {
        *path_start = '\0';
        strcpy(url_p->host, host_start);
        strcpy(url_p->port, "80");
        *path_start = '/';
        strcpy(url_p->path, path_start);
    }
    else{
        *port_start = '\0';
        strcpy(url_p->host, host_start);
        *port_start = ':';
        *path_start = '\0';
        strcpy(url_p->port, port_start + 1);
        *path_start = '/';
        strcpy(url_p->path, path_start);
    }
    return 0;
}

void build_header(rio_t* client_rio_p, string header_info, string host){
    string buf;
    int has_host_flag = 0;
    while (1) {
        rio_readlineb(client_rio_p, buf, MAXLINE);
        // 遇到结束行
        if (strcmp(buf, "\r\n") == 0) {
            break;
        }
        // 如果遇到 Host 头，记录之，后续不再添加 Host 头
        if (!strncasecmp(buf, "Host:", strlen("Host:"))) {
            has_host_flag = 1;
        }
        // 如果遇到 Connection 头、Proxy-Connection 头、User-Agent 头，直接跳过，后续替换为默认值
        if (!strncasecmp(buf, "Connection:", strlen("Connection:"))) {
            continue;
        }
        if (!strncasecmp(buf, "Proxy-Connection:", strlen("Proxy-Connection:"))) {
            continue;
        }
        if (!strncasecmp(buf, "User-Agent:", strlen("User-Agent:"))) {
            continue;
        }
        // 其他头与 Host 头直接添加
        strcat(header_info, buf);
    }
    // 如果没有 Host 头，添加 Host 头
    if (!has_host_flag) {
        sprintf(buf, "Host: %s\r\n", host);
        strcat(header_info, buf);
    }
    // 添加 Connection 头、Proxy-Connection 头、User-Agent 头
    strcat(header_info, "Connection: close\r\n");
    strcat(header_info, "Proxy-Connection: close\r\n");
    strcat(header_info, user_agent_hdr);
    // 添加结束行
    strcat(header_info, "\r\n");
    return;
}



//cache
void init_cache()
{
    timestamp = 0;
    reader_count = 0;
    cache.using_cache_num = 0;
    Sem_init(&cache_mutex, 0, 1);
    Sem_init(&writer, 0, 1);
}

int find_cache(rio_t* rio_p, string url)
{
    P(&cache_mutex);
    reader_count++;
    if(reader_count == 1)
        P(&writer);
    V(&cache_mutex);

    int hit_flag = 0;
    for(int i = 0; i< cache.using_cache_num; i++)
       if(!strcmp(cache.cache[i].url ,url))
       {
        P(&cache_mutex);
        cache.cache[i].timestamp = timestamp++;
        V(&cache_mutex);

        rio_writen(rio_p->rio_fd, cache.cache[i].object, cache.cache[i].size);
        hit_flag = 1;
        break;
       }
    P(&cache_mutex);
    reader_count--;
    if(reader_count == 0)
        V(&writer);
    V(&cache_mutex);
    return hit_flag;
}

int write_cache(string url, char * content, int size)
{
    P(&writer);
    if(cache.using_cache_num == CACHE_BLOCK - 1)
    {
        int oldest_index;
        int oldest_time = timestamp;
        for(int i = 0; i < cache.using_cache_num; i++)
            if(cache.cache[i].timestamp < oldest_time)
            {
                oldest_time = cache.cache[i].timestamp;
                oldest_index = i;   
            }
        
        //替换缓存
        strcpy(cache.cache[oldest_index].url, url);
        memcpy(cache.cache[oldest_index].object, content, size);
        cache.cache[oldest_index].size = size;
        P(&cache_mutex);
        cache.cache[oldest_index].timestamp = timestamp++;
        V(&cache_mutex);
    }
    else{
        strcpy(cache.cache[cache.using_cache_num].url, url);
        memcpy(cache.cache[cache.using_cache_num].object, content, size);
        cache.cache[cache.using_cache_num].size = size;
        P(&cache_mutex);
        cache.cache[cache.using_cache_num].timestamp = timestamp++;
        V(&cache_mutex);
        cache.using_cache_num++;
    }
    V(&writer);
    return 0;
}
