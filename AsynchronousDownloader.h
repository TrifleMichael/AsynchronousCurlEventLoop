

#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <curl/curl.h>
#include <string>
#include <iostream>
#include <string.h>   //strcpy
#include <functional> // std::ref
#include <thread>     // get_id
#include <vector>
#include <unordered_map>

#include <chrono> // time measurement
#include <unistd.h> // time measurement

#ifndef ASYNCHRONOUSDOWNLOADER_H_
#define ASYNCHRONOUSDOWNLOADER_H_

void checkDownloadTasks(uv_timer_t *handle);
void timerCallback(uv_timer_t *handle);
void on_timeout(uv_timer_t *req);
void curl_perform(uv_poll_t *req, int status, int events);

class AsynchronousDownloader
{
  public:
    uint64_t curlBuffer = 950; // miliseconds durning which handle will be left open after last call
    int loopIterations = 5000;

    bool closeLoop = false;
    uv_loop_t *loop = nullptr;
    CURLM *curl_handle = nullptr;
    uv_timer_t *timeout;

typedef struct curl_context_s
{
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
  AsynchronousDownloader* objPtr = nullptr;
} curl_context_t;

typedef struct CurlHandleData
{
  bool inUse = true;
  int index;
  CURL* curlHandle;
  uv_timer_t timerHandle;
  int queueIndex;
} CurlHandleData;

typedef struct UvTimerHandleData
{
  bool refresh = true;
  CurlHandleData *curlHandleData = nullptr;
  AsynchronousDownloader *downloaderPtr;
} UvTimerHandleData;

typedef struct DataForSocket
{
  AsynchronousDownloader *objPtr;
  CURLM *curlm;
} DataForSocket;

std::vector<std::unordered_map<std::string, std::string*>*> urlContentMapQueue;
std::vector<int> queueStatus;
std::vector<int> queueProgress;
std::unordered_map<int, CurlHandleData*> handleDataMap;

static curl_context_t* create_curl_context(curl_socket_t sockfd, AsynchronousDownloader* objPtr);
static void curl_close_cb(uv_handle_t *handle);
static void destroy_curl_context(curl_context_t *context);
static int createCurlHandleIndex(std::unordered_map<int, AsynchronousDownloader::CurlHandleData *> *handleDataMap);
static CurlHandleData* createCurlHandleData(CURL *handle, int queueIndex, std::unordered_map<int, AsynchronousDownloader::CurlHandleData *> *handleDataMap, uv_loop_t *loop);
static size_t myCallback(void *contents, size_t size, size_t nmemb, std::string *dst);
static void startDownload(std::string *dst, std::string url, int queueInd, std::unordered_map<int, CurlHandleData*> *handleDataMap, uv_loop_t *loop, CURLM *curl_handle);
void check_multi_info(void);
static int start_timeout(CURLM *multi, long timeout_ms, void *userp);
static int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp);
void runDownloadsFromMap(std::unordered_map<std::string, std::string*> *urlContentMap, int queueIndex);
std::unordered_map<std::string, std::string*>* urlVectorToUrlContentMap(std::vector<std::string> urlVector);
void printBar();
void printContents(std::unordered_map<std::string, std::string*> *urlContentMap);
int oldMain();
int addDownloadTask(std::vector<std::string> urlVector);
std::unordered_map<std::string, std::string*>* getResponse(int index);
std::string* getResponse(int index, std::string url);


};

#endif