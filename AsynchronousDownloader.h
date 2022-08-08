

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

#include <chrono>   // time measurement
#include <unistd.h> // time measurement

#ifndef ASYNCHRONOUSDOWNLOADER_H_
#define ASYNCHRONOUSDOWNLOADER_H_

void checkDownloadTasks(uv_timer_t *handle);
void timerCallback(uv_timer_t *handle);
void on_timeout(uv_timer_t *req);
void curl_perform(uv_poll_t *req, int status, int events);
void checkForClosing(uv_timer_t *handle);

class AsynchronousDownloader
{
public:
  uint64_t curlBuffer = 800; // miliseconds durning which handle will be left open after last call
  int loopIterations = 5000;

  bool closeLoop = false;
  uv_loop_t *loop = nullptr;
  CURLM *curlMultiHandle = nullptr;
  uv_timer_t *timeout;

  typedef struct curl_context_s
  {
    uv_poll_t poll_handle;
    curl_socket_t sockfd;
    AsynchronousDownloader *objPtr = nullptr;
  } curl_context_t;

  typedef struct CurlHandleData
  {
    bool inUse = true;
    int index;
    CURL *curlHandle;
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

  typedef struct PerformData
  {
    bool asynchronous;
    std::condition_variable *cv;
    bool *completionFlag;
    CURLcode *codeDestination;
  } PerformData;

  static curl_context_t *createCurlContext(curl_socket_t sockfd, AsynchronousDownloader *objPtr);
  static void curlCloseCB(uv_handle_t *handle);
  static void destroyCurlContext(curl_context_t *context);
  static int createCurlHandleIndex(std::unordered_map<int, AsynchronousDownloader::CurlHandleData *> *handleDataMap);
  static CurlHandleData *createCurlHandleData(CURL *handle, int queueIndex, std::unordered_map<int, AsynchronousDownloader::CurlHandleData *> *handleDataMap, uv_loop_t *loop);
  static size_t curlCallback(void *contents, size_t size, size_t nmemb, std::string *dst);
  void checkMultiInfo(void);
  static int startTimeout(CURLM *multi, long timeout_ms, void *userp);
  static int handleSocket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp);
  void printBar();
  void printContents(std::unordered_map<std::string, std::string *> *urlContentMap);
  void asynchLoop();
  int addDownloadTask(std::vector<std::string> urlVector);
  std::unordered_map<std::string, std::string *> *getResponse(int index);
  std::string *getResponse(int index, std::string url);
  bool init();
  CURLcode *blockingPerform(CURL* handle);
  CURLcode *asynchPerform(CURL* handle, bool *completionFlag);
};

#endif