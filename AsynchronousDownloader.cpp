
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <curl/curl.h>
#include <string>
#include <iostream>
#include <string.h>   //strcpy
#include <thread>     // get_id
#include <vector>
#include <condition_variable>
#include <mutex>

#include <chrono>   // time measurement
#include <unistd.h> // time measurement

#include "AsynchronousDownloader.h"
#include "benchmark.h"
#include "resources.h"

/*
g++ -std=c++11 AsynchronousDownloader.cpp -lpthread -lcurl -luv -o main && ./main
g++ -std=c++11 AsynchronousDownloader.cpp benchmark.cpp -lpthread -lcurl -luv -o main && ./main
*/

/*
TODO:

- change name "checkGlobals"
- pooling threads only when they exist
- adding locks to all operations

- reusing socket errors can happen - handle by trying new socket, and pass error as last resort
- multiple uv loop threads

Questions:

Information:

- Curl multi handle automatically reuses connections. Source: https://everything.curl.dev/libcurl/connectionreuse

*/

void onTimeout(uv_timer_t *req)
{
  // std::cout << "onTimeout\n";
  auto AD = (AsynchronousDownloader *)req->data;
  int running_handles;
  curl_multi_socket_action(AD->curlMultiHandle, CURL_SOCKET_TIMEOUT, 0,
                           &running_handles);
  AD->checkMultiInfo();
}

// Is used to react to polling file descriptors in poll_handle
// Calls handle_socket indirectly for further reading*
// If call is finished closes handle indirectly by check multi info
void curl_perform(uv_poll_t *req, int status, int events)
{
  // std::cout << "curl_perform\n";

  int running_handles;
  int flags = 0;
  if (events & UV_READABLE)
    flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE)
    flags |= CURL_CSELECT_OUT;

  auto context = (AsynchronousDownloader::curl_context_t *)req->data;

  curl_multi_socket_action(context->objPtr->curlMultiHandle, context->sockfd, flags,
                           &running_handles);
  context->objPtr->checkMultiInfo();
}

// Initializes a handle using a socket and passes it to context
AsynchronousDownloader::curl_context_t *AsynchronousDownloader::createCurlContext(curl_socket_t sockfd, AsynchronousDownloader *objPtr)
{
  // std::cout << "createCurlContext\n";
  curl_context_t *context;

  context = (curl_context_t *)malloc(sizeof(*context));
  context->objPtr = objPtr;
  context->sockfd = sockfd;

  uv_poll_init_socket(objPtr->loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;

  return context;
}

// Frees data from curl handle inside uv_handle*
void AsynchronousDownloader::curlCloseCB(uv_handle_t *handle)
{
  // std::cout << "curlCloseCB\n";
  curl_context_t *context = (curl_context_t *)handle->data;
  free(context);
}

// Makes an asynchronious call to free curl context*
void AsynchronousDownloader::destroyCurlContext(curl_context_t *context)
{
  // std::cout << "destroyCurlContext\n";
  uv_close((uv_handle_t *)&context->poll_handle, curlCloseCB);
}

void callbackWrappingFunction(void (*cbFun)(void*), void* data, bool* completionFlag)
{
  // std::cout << "callbackWrappingFunction\n";
  cbFun(data);
  *completionFlag = true;
}

// Removes used easy handles from multihandle
void AsynchronousDownloader::checkMultiInfo(void)
{
  // std::cout << "checkMultiInfo\n";

  char *done_url;
  CURLMsg *message;
  int pending;
  CURL *easy_handle;

  while ((message = curl_multi_info_read(curlMultiHandle, &pending)))
  {
    switch (message->msg)
    {
    case CURLMSG_DONE:
    {
      /* Do not use message data after calling curl_multi_remove_handle() and
        curl_easy_cleanup(). As per curl_multi_info_read() docs:
        "WARNING: The data the returned pointer points to will not survive
        calling curl_multi_cleanup, curl_multi_remove_handle or
        curl_easy_cleanup." */
      
      handlesInUse--;
      easy_handle = message->easy_handle;
      curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &done_url);
      // printf("%s DONE\n", done_url);

      PerformData *data;
      curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &data); 
      curl_multi_remove_handle(curlMultiHandle, easy_handle);

      curl_easy_getinfo(easy_handle,  CURLINFO_RESPONSE_CODE, data->codeDestination);

      if (data->callback)
      {
        bool *cbFlag = (bool*)malloc(sizeof(bool));
        *cbFlag = false;
        auto cbThread = new std::thread(&callbackWrappingFunction, data->cbFun, data->cbData, cbFlag);
        threadFlagPairVector.emplace_back(cbThread, cbFlag);
      }
      // Blocking
      if (!data->asynchronous)
      {
        // Batch request
        if (data->batchRequest)
        {
          *data->requestsLeft -= 1;
          if (*data->requestsLeft == 0)
          {
            data->cv->notify_all();
          }
        }
        // Single request
        else {
          data->cv->notify_all();
        }
      }
      // Asynchronous
      else
      {
        // Single request
        if (!data->batchRequest)
        {
          *(data->completionFlag) = true;
          free(data);
        }
        // Batch request
        else {
          *(data->requestsLeft) -= 1;
          if (*data->requestsLeft == 0) {
            *data->completionFlag = true;
            free(data);
          }
        }
      }
      curl_easy_cleanup(easy_handle);
    }
    break;

    default:
      fprintf(stderr, "CURLMSG default\n");
      break;
    }
  }
}

// Connects curl timer with uv timer
int AsynchronousDownloader::startTimeout(CURLM *multi, long timeout_ms, void *userp)
{
  // std::cout << "startTimeout\n";
  auto timeout = (uv_timer_t *)userp;

  if (timeout_ms < 0)
  {
    uv_timer_stop(timeout);
  }
  else
  {
    if (timeout_ms == 0)
      timeout_ms = 1; /* 0 means directly call socket_action, but we will do it
                        in a bit */
    uv_timer_start(timeout, onTimeout, timeout_ms, 0);
  }
  return 0;
}

// Is used to react to curl_multi_socket_action
// If INOUT then assigns socket to multi handle and starts polling file descriptors in poll_handle by callback
int AsynchronousDownloader::handleSocket(CURL *easy, curl_socket_t s, int action, void *userp,
                                         void *socketp)
{
  // std::cout << "handleSocket\n";
  auto socketData = (DataForSocket *)userp;
  curl_context_t *curl_context;
  int events = 0;

  switch (action)
  {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    curl_context = socketp ? (curl_context_t *)socketp : createCurlContext(s, socketData->objPtr);
    curl_multi_assign(socketData->curlm, s, (void *)curl_context);

    if (action != CURL_POLL_IN)
      events |= UV_WRITABLE;
    if (action != CURL_POLL_OUT)
      events |= UV_READABLE;

    uv_poll_start(&curl_context->poll_handle, events, curl_perform);
    break;
  case CURL_POLL_REMOVE:
    if (socketp)
    {
      uv_poll_stop(&((curl_context_t *)socketp)->poll_handle);
      destroyCurlContext((curl_context_t *)socketp);
      curl_multi_assign(socketData->curlm, s, NULL);
    }
    break;
  default:
    abort();
  }

  return 0;
}

void checkGlobals(uv_timer_t *handle)
{
  // std::cout << "checkGlobals\n";

  // Check for closing signal
  auto AD = (AsynchronousDownloader*)handle->data;
  if(AD->closeLoop) {
    uv_timer_stop(handle);
  }

  // Check if any handles in queue
  if (AD->handlesToBeAdded.size() > 0)
  {
    AD->handlesQueueLock.lock();
    // Add handles without going over the limit
    while(AD->handlesToBeAdded.size() > 0 && AD->handlesInUse < AD->maxHandlesInUse) {
      curl_multi_add_handle(AD->curlMultiHandle, AD->handlesToBeAdded.front());
      AD->handlesInUse++;
      AD->handlesToBeAdded.erase(AD->handlesToBeAdded.begin());
    }
    AD->handlesQueueLock.unlock();
  }

  // Join and erase threads that finished running callback functions
  for (int i = 0; i < AD->threadFlagPairVector.size(); i++)
  {
    if (*(AD->threadFlagPairVector[i].second))
    {
      AD->threadFlagPairVector[i].first->join();
      delete (AD->threadFlagPairVector[i].first);
      delete (AD->threadFlagPairVector[i].second);
      AD->threadFlagPairVector.erase(AD->threadFlagPairVector.begin() + i);
    }
  }
}

bool AsynchronousDownloader::init()
{
  // std::cout << "init\n";

  // Preparing loop timer
  timeout = new uv_timer_t();
  timeout->data = this;
  loop = uv_default_loop();
  uv_timer_init(loop, timeout);

  // Preparing curl handle
  curlMultiHandle = curl_multi_init();
  curl_multi_setopt(curlMultiHandle, CURLMOPT_SOCKETFUNCTION, handleSocket);
  auto socketData = new DataForSocket();
  socketData->curlm = curlMultiHandle;
  socketData->objPtr = this;
  curl_multi_setopt(curlMultiHandle, CURLMOPT_SOCKETDATA, socketData);
  curl_multi_setopt(curlMultiHandle, CURLMOPT_TIMERFUNCTION, startTimeout);
  curl_multi_setopt(curlMultiHandle, CURLMOPT_TIMERDATA, timeout);

  // Preparing queue checking timer
  auto timerCheckQueueHandle = new uv_timer_t();
  timerCheckQueueHandle->data = this;
  uv_timer_init(loop, timerCheckQueueHandle);
  uv_timer_start(timerCheckQueueHandle, checkGlobals, 100, 100);

  return true;
}

void AsynchronousDownloader::asynchLoop()
{
  // std::cout << "asynchLoop\n";
  uv_run(loop, UV_RUN_DEFAULT);

  curl_multi_cleanup(curlMultiHandle);

  // // Cleaning UV loop
  // std::cout << "\nIs loop alive? " << uv_loop_alive(loop) << "\n";
  // uv_stop(loop);
  // std::cout << "Is loop alive? " << uv_loop_alive(loop) << "\n";
  // std::cout << "Loop closed properly? " << (uv_loop_close(loop) != UV_EBUSY)  << "\n";
  // free(loop);
}

std::vector<CURLcode*> AsynchronousDownloader::batchAsynchPerform(std::vector<CURL*> handleVector, bool *completionFlag)
{
  std::vector<CURLcode*> codeVector;
  size_t *requestsLeft = new size_t();
  *requestsLeft = handleVector.size();

  handlesQueueLock.lock();
  for(int i = 0; i < handleVector.size(); i++)
  {
    auto *data = new AsynchronousDownloader::PerformData();
    codeVector.push_back(new CURLcode());
    data->codeDestination = codeVector.back();
    data->asynchronous = true;

    data->batchRequest = true;
    data->requestsLeft = requestsLeft;
    data->completionFlag = completionFlag;

    curl_easy_setopt(handleVector[i], CURLOPT_PRIVATE, data);
    handlesToBeAdded.push_back(handleVector[i]); // protected before and after for
  }
  handlesQueueLock.unlock();
  return codeVector;
}

std::vector<CURLcode*> AsynchronousDownloader::batchBlockingPerform(std::vector<CURL*> handleVector)
{
  // std::cout << "batchBlockingPerform\n";
  std::condition_variable cv;
  std::mutex cv_m;
  std::unique_lock<std::mutex> lk(cv_m);

  std::vector<CURLcode*> codeVector;
  size_t requestsLeft = handleVector.size();

  handlesQueueLock.lock();
  for(int i = 0; i < handleVector.size(); i++)
  {
    auto *data = new AsynchronousDownloader::PerformData();
    codeVector.push_back(new CURLcode());
    data->codeDestination = codeVector.back();
    data->asynchronous = false;
    data->cv = &cv;

    data->batchRequest = true;
    data->requestsLeft = &requestsLeft;

    curl_easy_setopt(handleVector[i], CURLOPT_PRIVATE, data);
    handlesToBeAdded.push_back(handleVector[i]); // protected before and after for
  }
  handlesQueueLock.unlock();

  cv.wait(lk);
  return codeVector;
}

CURLcode *AsynchronousDownloader::blockingPerform(CURL* handle)
{
  // std::cout << "blockingPerform\n";
  std::condition_variable cv;
  std::mutex cv_m;
  std::unique_lock<std::mutex> lk(cv_m);

  AsynchronousDownloader::PerformData data;
  auto code = new CURLcode();
  data.codeDestination = code;
  data.asynchronous = false;
  data.cv = &cv;

  curl_easy_setopt(handle, CURLOPT_PRIVATE, &data);

  handlesQueueLock.lock();
  handlesToBeAdded.push_back(handle);
  handlesQueueLock.unlock();

  cv.wait(lk);
  return code;
}

CURLcode *AsynchronousDownloader::blockingPerformWithCallback(CURL* handle, void (*cbFun)(void*), void* cbData)
{
  // std::cout << "blockingPerformWithCallback\n";
  std::condition_variable cv;
  std::mutex cv_m;
  std::unique_lock<std::mutex> lk(cv_m);

  AsynchronousDownloader::PerformData data;
  auto code = new CURLcode();
  data.codeDestination = code;
  data.asynchronous = false;
  data.cv = &cv;
  
  curl_easy_setopt(handle, CURLOPT_PRIVATE, &data);

  handlesQueueLock.lock();
  handlesToBeAdded.push_back(handle);
  handlesQueueLock.unlock();

  cv.wait(lk);
  cbFun(cbData);
  return code;
}

CURLcode *AsynchronousDownloader::asynchPerform(CURL* handle, bool *completionFlag)
{
  // std::cout << "asynchPerform\n";
  auto data = new AsynchronousDownloader::PerformData();
  auto code = new CURLcode();
  data->asynchronous = true;
  data->completionFlag = completionFlag;
  data->codeDestination = code;

  curl_easy_setopt(handle, CURLOPT_PRIVATE, data);

  handlesQueueLock.lock();
  handlesToBeAdded.push_back(handle);
  handlesQueueLock.unlock();

  return code;
}

CURLcode *AsynchronousDownloader::asynchPerformWithCallback(CURL* handle, bool *completionFlag, void (*cbFun)(void*), void* cbData)
{
  // std::cout << "asynchPerformWithCallback\n";
  auto data = new AsynchronousDownloader::PerformData();
  auto code = new CURLcode();
  data->asynchronous = true;
  data->completionFlag = completionFlag;
  data->codeDestination = code;

  data->cbFun = cbFun;
  data->cbData = cbData;
  data->callback = true;

  curl_easy_setopt(handle, CURLOPT_PRIVATE, data);

  handlesQueueLock.lock();
  handlesToBeAdded.push_back(handle);
  handlesQueueLock.unlock();

  return code;
}

size_t writeToString(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  // std::cout << "writeToString\n";
  char *conts = (char *)contents;
  for (int i = 0; i < nmemb; i++)
  {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

CURL* prepareTestHandle(std::string* dst)
{
  // std::cout << "prepareTestHandle\n";
  CURL* handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_URL, "http://alice-ccdb.cern.ch/latest/TPC/.*");
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  return handle;
}

void testCallback(void* data)
{
  // std::cout << "testCallback\n";
  std::cout << "Callback works, data: " << *((std::string*)data) << "\n";
}

std::vector<std::string> createPathsFromCS()
{
  std::vector<std::string> vec;
  std::string temp = "";
  for(int i = 0; i < pathsCS.size(); i++)
  {
    if (pathsCS[i] == ',') {
      vec.push_back(temp);
      temp = "";
    }
    else {
      (temp.push_back(pathsCS[i]));
    }
  }
  return vec;
}

void blockingBatchTest()
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  
  std::vector<CURL*> handles;
  for (auto path : paths) {
    CURL* handle = curl_easy_init();
    curl_easy_setopt(handle, CURLOPT_URL, path.c_str());

    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    handles.push_back(handle);
  }


  AsynchronousDownloader AD;
  AD.init();
  std::thread t(&AsynchronousDownloader::asynchLoop, &AD);

  auto start = std::chrono::system_clock::now();

  AD.batchBlockingPerform(handles);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "BLOCKING BATCH TEST - execution time: " << difference << "ms.\n";
  AD.closeLoop = true;
  t.join();
}

void asynchBatchTest()
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  
  std::vector<CURL*> handles;
  for (auto path : paths) {
    CURL* handle = curl_easy_init();
    curl_easy_setopt(handle, CURLOPT_URL, path.c_str());

    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    handles.push_back(handle);
  }


  AsynchronousDownloader AD;
  AD.init();
  std::thread t(&AsynchronousDownloader::asynchLoop, &AD);

  auto start = std::chrono::system_clock::now();

  bool requestFinished = false;
  AD.batchAsynchPerform(handles, &requestFinished);

  while (!requestFinished) sleep(0.05);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "ASYNCH BATCH TEST - execution time: " << difference << "ms.\n";
  AD.closeLoop = true;
  t.join();
}


int countDataReceived(std::vector<bool*> flags)
{
  int counter = 0;
  for(int i = 0; i < flags.size(); i++)
  {
    if (*flags[i]) {
      counter++;
    }
  }
  return counter;
}

void benchmarkTest()
{
  // std::cout << "benchmarkTest\n";
  // auto paths = createPaths();
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<CURL*> handles;
  std::vector<bool*> flags;

  AsynchronousDownloader AD;
  AD.init();
  std::thread t(&AsynchronousDownloader::asynchLoop, &AD);
  auto start = std::chrono::system_clock::now();

  // paths.size()
  for(int i = 0; i < paths.size(); i++) {
    handles.push_back(new CURL*);
    handles[i] = curl_easy_init();
    auto handle = handles[i];

    flags.push_back(new bool(false));
    results.push_back(new std::string());

    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results[i]);
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    AD.asynchPerform(handle, flags[i]);
  }

  std::cout << "Order placed, waiting for results\n";
  int oldCounter = countDataReceived(flags);
  int counter = countDataReceived(flags);
  while(counter < flags.size()) {
    counter = countDataReceived(flags);
    if (oldCounter != counter) {
      std::cout << flags.size() - counter << " files left\n";
    }
    oldCounter = counter;
    sleep(0.05);
  }

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Measured time is: " << difference << "ms.\n";
  AD.closeLoop = true;
  t.join();
  std::cout << "Thread joined\n";
  for(int i = 0; i < handles.size(); i++) {
    curl_easy_cleanup(handles[i]);
  }
}

void linearTest()
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;

  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  auto start = std::chrono::system_clock::now();

  for (int i = 0; i < paths.size(); i++)
  {
    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results[i]);
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());
    curl_easy_perform(handle);
  }
  curl_easy_cleanup(handle);
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "LINEAR TEST - execution time: " << difference << "ms.\n";

}

void printUpdatedPaths()
{
  auto paths = createPaths();
  std::cout << "Done\n\n\n\n";
  for(auto path : paths) {
    std::cout << *path;
  }
  std::cout << "\n\n\n\nDone\n";
}

int main()
{
  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }

  linearTest();
  blockingBatchTest();
  asynchBatchTest();

  curl_global_cleanup();
  return 0;
}