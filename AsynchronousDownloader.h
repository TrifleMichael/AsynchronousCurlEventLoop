

#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <curl/curl.h>
#include <string>
#include <iostream>
#include <string.h>   //strcpy
#include <thread>     // get_id
#include <vector>
#include <mutex>

#include <chrono>   // time measurement
#include <unistd.h> // time measurement

#ifndef ASYNCHRONOUSDOWNLOADER_H_
#define ASYNCHRONOUSDOWNLOADER_H_

void checkDownloadTasks(uv_timer_t *handle);
void timerCallback(uv_timer_t *handle);
void on_timeout(uv_timer_t *req);
void curl_perform(uv_poll_t *req, int status, int events);
void checkGlobals(uv_timer_t *handle);

class AsynchronousDownloader
{
public:
  uint64_t curlBuffer = 800; // miliseconds durning which handle will be left open after last call
  int loopIterations = 5000;

  bool closeLoop = false;
  uv_loop_t *loop = nullptr;
  CURLM *curlMultiHandle = nullptr;
  uv_timer_t *timeout;
  std::vector<CURL*> handlesToBeAdded;
  std::mutex handlesQueueLock;
  

  std::vector< std::pair<std::thread*, bool*> > threadFlagPairVector;

  typedef struct curl_context_s
  {
    uv_poll_t poll_handle;
    curl_socket_t sockfd;
    AsynchronousDownloader *objPtr = nullptr;
  } curl_context_t;

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
    void (*cbFun)(void*);
    std::thread *cbThread;
    void *cbData;
    bool callback = false;
  } PerformData;

  static curl_context_t *createCurlContext(curl_socket_t sockfd, AsynchronousDownloader *objPtr);
  static void curlCloseCB(uv_handle_t *handle);
  static void destroyCurlContext(curl_context_t *context);
  void checkMultiInfo(void);
  static int startTimeout(CURLM *multi, long timeout_ms, void *userp);
  static int handleSocket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp);
  void asynchLoop();
  bool init();
  CURLcode *blockingPerform(CURL* handle);
  CURLcode *blockingPerformWithCallback(CURL* handle, void (*cbFun)(void*), void* cbData);
  CURLcode *asynchPerform(CURL* handle, bool *completionFlag);
  CURLcode *asynchPerformWithCallback(CURL* handle, bool *completionFlag, void (*cbFun)(void*), void* cbData);
};

#endif