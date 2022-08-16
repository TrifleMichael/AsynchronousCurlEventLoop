#include <curl/curl.h>
#include <iostream>
#include <chrono>
// #include <stdlib.h>
#include <curl/curl.h>
#include <vector>
#include "benchmark.h"

/*
g++ -std=c++11 socketTest.cpp benchmark.cpp -lpthread -lcurl -luv -o socketTest && ./socketTest
*/

void socketCB(CURL *easy,      /* easy handle */
                    curl_socket_t s, /* socket */
                    int what,        /* describes the socket */
                    void *userp,     /* private callback pointer */
                    void *socketp)  /* private socket pointer */
{
  switch (what)
  {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    break;
  case CURL_POLL_REMOVE:
    break;
  default:
    break;
  }
}

int main()
{
  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }

  auto paths = createPaths();
  std::vector<std::string *> results;

  auto start = std::chrono::system_clock::now();

  CURL *handle = curl_easy_init();
  CURLM *multiHandle = curl_multi_init();
  curl_multi_setopt(multiHandle, CURLMOPT_SOCKETFUNCTION, socketCB);
  // curl_multi_setopt(multiHandle, CURLMOPT_TIMERFUNCTION, socketCB);

  for(auto path : paths) {
    std::cout << *path << "\n";
  }

  curl_multi_socket_action(multiHandle, )



  curl_multi_remove_handle(multiHandle, handle);
  curl_easy_cleanup(handle);
  curl_multi_cleanup(multiHandle);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "LINEAR TEST:          download - " << difference << "ms.\n";
  // std::cout << "LINEAR TEST:          download - " << difference << "ms, Check validity - " <<  difference2 << "ms.\n";

  curl_global_cleanup();
  return 0;
}