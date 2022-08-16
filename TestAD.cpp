#include <iostream>
#include "AsynchronousDownloader.h"
#include <curl/curl.h>
#include "resources.h"
#include <unordered_map>

/*
g++ -std=c++11 TestAD.cpp AsynchronousDownloader.cpp benchmark.cpp -lpthread -lcurl -luv -o TestAD && ./TestAD
*/

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

void cleanAllHandles(std::vector<CURL*> handles)
{
  for(auto handle : handles)
    curl_easy_cleanup(handle);
}

void setHandleOptions(CURL* handle, std::string* dst, std::string* headers, std::string* path)
{
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, headers);

  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  curl_easy_setopt(handle, CURLOPT_URL, path->c_str());
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

void blockingBatchTest(int pathLimit = 0)
{
  // Preparing for downloading
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;
  
  AsynchronousDownloader AD;

  auto start = std::chrono::system_clock::now();

  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    handles.push_back(curl_easy_init());
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handles.back(), results.back(), headers.back(), &paths[i]);
  }

  // Downloading objects
  AD.batchBlockingPerform(handles);
  cleanAllHandles(handles);
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  std::vector<CURL*> handles2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    auto handle = curl_easy_init();
    results2.push_back(new std::string());
    handles2.push_back(handle);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + urlETagMap[paths[i]] + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
  }

  // Checking objects validity

  AD.batchBlockingPerform(handles2);
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    long code;
    curl_easy_getinfo(handles2[i], CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      char* url;
      curl_easy_getinfo(handles2[i], CURLINFO_EFFECTIVE_URL, &url);
      std::cout << "INVALID CODE: " << code << ", URL: " << url << "\n";
    }
  }

  // Clean handles and print time
  cleanAllHandles(handles2);
  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "BLOCKING BATCH TEST:  download - " << difference << "ms, Check validity - " <<  difference2 << "ms.\n";
}

void asynchBatchTest(int pathLimit = 0)
{
  // Preparing urls and objects to write into
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;
  
  // Preparing downloader
  AsynchronousDownloader AD;

  auto start = std::chrono::system_clock::now();

  // Preparing handles
  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    handles.push_back(curl_easy_init());
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handles.back(), results.back(), headers.back(), &paths[i]);
  }

  // Performing requests
  bool requestFinished = false;
  AD.batchAsynchPerform(handles, &requestFinished);
  while (!requestFinished) sleep(0.05);

  // Cleanup and print time
  cleanAllHandles(handles);
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  std::vector<CURL*> handles2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    auto handle = curl_easy_init();
    results2.push_back(new std::string());
    handles2.push_back(handle);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + extractETAG(urlETagMap[paths[i]]) + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
  }

  // Checking objects validity
  bool requestFinished2 = false;
  AD.batchAsynchPerform(handles2, &requestFinished2);
  while (!requestFinished2) sleep(0.001);
  cleanAllHandles(handles2);

  // Checking if objects did not change
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    long code;
    curl_easy_getinfo(handles2[i], CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
  }

  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "ASYNC BATCH TEST:     download - " << difference << "ms, Check validity - " <<  difference2 << "ms.\n";
}

void linearTest(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;

  auto start = std::chrono::system_clock::now();
  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handle, results.back(), headers.back(), &paths[i]);
    curl_easy_perform(handle);
  }

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  
  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    results2.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + extractETAG(urlETagMap[paths[i]]) + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
    
    curl_easy_perform(handle);
    long code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
  }

  curl_easy_cleanup(handle);
  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "LINEAR TEST:          download - " << difference << "ms, Check validity - " <<  difference2 << "ms.\n";

}

void linearTestNoReuse(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;

  auto start = std::chrono::system_clock::now();

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL* handle = curl_easy_init();
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handle, results.back(), headers.back(), &paths[i]);
    curl_easy_perform(handle);
    curl_easy_cleanup(handle);

  }
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL* handle = curl_easy_init();
    results2.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + extractETAG(urlETagMap[paths[i]]) + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
    
    curl_easy_perform(handle);
    long code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
    curl_easy_cleanup(handle);
  }

  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "LINEAR NO REUSE TEST: download - " << difference << "ms, Check validity - " <<  difference2 << "ms.\n";

}

int main()
{
  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }

  int testSize = 100;

  if (testSize != 0)
    std::cout << "-------------- Testing for " << testSize << " objects with " << AsynchronousDownloader::maxHandlesInUse << " parallel connections. -----------\n";
  else
    std::cout << "-------------- Testing for all objects with " << AsynchronousDownloader::maxHandlesInUse << " parallel connections. -----------\n";

  blockingBatchTest(testSize);
  asynchBatchTest(testSize);
  linearTest(testSize);
  linearTestNoReuse(testSize);

  curl_global_cleanup();

  return 0;
}
