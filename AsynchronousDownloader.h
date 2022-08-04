#pragma once

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
    uv_loop_t *loop;
    CURLM *curl_handle;
    uv_timer_t timeout;

    typedef struct curl_context_s
{
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
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


std::vector<std::unordered_map<std::string, std::string*>*> urlContentMapQueue;
std::vector<int> queueStatus;
std::vector<int> queueProgress;
std::unordered_map<int, CurlHandleData*> handleDataMap;

curl_context_t* create_curl_context(curl_socket_t sockfd);
static void curl_close_cb(uv_handle_t *handle);
static void destroy_curl_context(curl_context_t *context);
int createCurlHandleIndex();
CurlHandleData* createCurlHandleData(CURL *handle, int queueIndex);
size_t myCallback(void *contents, size_t size, size_t nmemb, std::string *dst);
void startDownload(std::string *dst, std::string url, int queueInd);
void check_multi_info(void);
int start_timeout(CURLM *multi, long timeout_ms, void *userp);
int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp);
void runDownloadsFromMap(std::unordered_map<std::string, std::string*> *urlContentMap, int queueIndex);
std::unordered_map<std::string, std::string*>* urlVectorToUrlContentMap(std::vector<std::string> urlVector);
void printBar();
void printContents(std::unordered_map<std::string, std::string*> *urlContentMap);
int oldMain();
int addDownloadTask(std::vector<std::string> urlVector);
std::unordered_map<std::string, std::string*>* getResponse(int index);
std::string* getResponse(int index, std::string url);


};
