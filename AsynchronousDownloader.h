

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
#include <condition_variable>

#include <chrono>   // time measurement
#include <unistd.h> // time measurement

#ifndef ASYNCHRONOUSDOWNLOADER_H_
#define ASYNCHRONOUSDOWNLOADER_H_

void curl_perform(uv_poll_t *req, int status, int events);
void checkGlobals(uv_timer_t *handle);
size_t writeToString(void *contents, size_t size, size_t nmemb, std::string *dst);
std::string extractETAG(std::string headers);

class AsynchronousDownloader
{
public:
  int handlesInUse = 0;
  static int const maxHandlesInUse = 1; // static and constant just for testing

  bool closeLoop = false;
  uv_loop_t loop;
  CURLM *curlMultiHandle = nullptr;
  uv_timer_t *timeout;
  std::vector<CURL*> handlesToBeAdded;
  std::mutex handlesQueueLock;
  std::thread *loopThread;  

  std::vector< std::pair<std::thread*, bool*> > threadFlagPairVector;

  typedef struct curl_context_s
  {
    uv_poll_t poll_handle;
    curl_socket_t sockfd = -1;
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
    bool batchRequest = false;
    size_t *requestsLeft;
  } PerformData;

  AsynchronousDownloader();
  ~AsynchronousDownloader();
  static curl_context_t *createCurlContext(curl_socket_t sockfd, AsynchronousDownloader *objPtr);
  static void curlCloseCB(uv_handle_t *handle);
  static void destroyCurlContext(curl_context_t *context);
  void checkMultiInfo(void);
  static int startTimeout(CURLM *multi, long timeout_ms, void *userp);
  static int handleSocket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp);
  void makeLoopCheckQueueAsync();
  void checkHandleQueue();
  void asynchLoop();
  bool init();
  CURLcode *blockingPerform(CURL* handle);
  CURLcode *blockingPerformWithCallback(CURL* handle, void (*cbFun)(void*), void* cbData);
  CURLcode *asynchPerform(CURL* handle, bool *completionFlag);
  CURLcode *asynchPerformWithCallback(CURL* handle, bool *completionFlag, void (*cbFun)(void*), void* cbData);
  std::vector<CURLcode*> batchBlockingPerform(std::vector<CURL*> handleVector);
  std::vector<CURLcode*> batchAsynchPerform(std::vector<CURL*> handleVector, bool *completionFlag);
};

#endif