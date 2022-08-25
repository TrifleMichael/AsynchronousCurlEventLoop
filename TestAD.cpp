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

std::vector<std::string*> createEtagsFromCS()
{
  std::vector<std::string*> vec;
  std::string *tmp = new std::string();
  for(int i = 0; i < etagsCS.size(); i++) {
    if (etagsCS[i] == ',') {
      vec.push_back(tmp);
      tmp = new std::string();
    } else {
      (*tmp) += etagsCS[i];
    }
  }
  vec.push_back(tmp);
  return vec;
}

int64_t blockingBatchTest(int pathLimit = 0, bool printResult = false)
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
  if (printResult)
    std::cout << "BLOCKING BATCH TEST:  Download - " <<  difference << "ms.\n";

  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }
  return difference;
}

int64_t blockingBatchTestValidity(int pathLimit = 0, bool printResult = false)
{
  // Preparing for checking objects validity
  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  auto start = std::chrono::system_clock::now();

  std::vector<std::string*> results;
  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    auto handle = curl_easy_init();
    results.push_back(new std::string());
    handles.push_back(handle);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + *etags[i] + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
  }

  // Checking objects validity

  AsynchronousDownloader AD;
  AD.batchBlockingPerform(handles);

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    long code;
    curl_easy_getinfo(handles[i], CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      char* url;
      curl_easy_getinfo(handles[i], CURLINFO_EFFECTIVE_URL, &url);
      std::cout << "INVALID CODE: " << code << ", URL: " << url << "\n";
    }
  }

  // Clean handles and print time
  cleanAllHandles(handles);
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  if (printResult)
    std::cout << "BLOCKING BATCH TEST:  Check validity - " <<  difference << "ms.\n";
  return difference;
}

int64_t asynchBatchTest(int pathLimit = 0, bool printResult = false)
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
  if (printResult)
    std::cout << "ASYNC BATCH TEST:     download - " << difference << "ms.\n";

  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }
  return difference;
}

int64_t asynchBatchTestValidity(int pathLimit = 0, bool printResult = false)
{
  // Preparing for checking objects validity
  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  auto start = std::chrono::system_clock::now();
  AsynchronousDownloader AD;

  std::vector<std::string*> results;
  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    auto handle = curl_easy_init();
    results.push_back(new std::string());
    handles.push_back(handle);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + *etags[i] + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
  }

  // Checking objects validity
  bool requestFinished = false;
  AD.batchAsynchPerform(handles, &requestFinished);
  while (!requestFinished) sleep(0.001);
  cleanAllHandles(handles);

  // Checking if objects did not change
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    long code;
    curl_easy_getinfo(handles[i], CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
  }

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  if (printResult)
    std::cout << "ASYNC BATCH TEST:     Check validity - " <<  difference << "ms.\n";
  return difference;
}

int64_t linearTest(int pathLimit = 0, bool printResult = false)
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

  curl_easy_cleanup(handle);
  
  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }
  if (printResult)
    std::cout << "LINEAR TEST:          download - " << difference << "ms.\n";
  return difference;
}

int64_t linearTestValidity(int pathLimit = 0, bool printResult = false)
{
  // Preparing for checking objects validity
  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  auto start = std::chrono::system_clock::now();

  std::vector<std::string*> results;
  CURL* handle = curl_easy_init();
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + *etags[i] + "\"";
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
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  if (printResult)
    std::cout << "LINEAR TEST:          Check validity - " <<  difference << "ms.\n";
  return difference;
}

int64_t linearTestNoReuse(int pathLimit = 0, bool printResult = false)
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
  if (printResult)
    std::cout << "LINEAR TEST no reuse:      download - " <<  difference << "ms.\n";

  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }
  return difference;
}

int64_t linearTestNoReuseValidity(int pathLimit = 0, bool printResult = false)
{
  // Preparing for checking objects validity
  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  auto start = std::chrono::system_clock::now();

  std::vector<std::string*> results;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL* handle = curl_easy_init();
    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + *etags[i] + "\"";
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

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  if (printResult)
    std::cout << "LINEAR NO REUSE TEST: Check validity - " <<  difference << "ms.\n";
  return difference;
}

int64_t countAverageTime(int64_t (*function)(int, bool), int arg, int repeats)
{
  int64_t sum = 0;
  for(int i = 0; i < repeats; i++) {
    sum += function(arg, false);
  }
  return sum / repeats;
}

int main()
{
  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }

  int testSize = 0;

  if (testSize != 0)
    std::cout << "-------------- Testing for " << testSize << " objects with " << AsynchronousDownloader::maxHandlesInUse << " parallel connections. -----------\n";
  else
    std::cout << "-------------- Testing for all objects with " << AsynchronousDownloader::maxHandlesInUse << " parallel connections. -----------\n";

  // blockingBatchTest(testSize);
  // asynchBatchTest(testSize);
  // linearTest(testSize);
  // linearTestNoReuse(testSize);

  // blockingBatchTestValidity(testSize);
  // asynchBatchTestValidity(testSize);
  // linearTestValidity(testSize);
  // linearTestNoReuseValidity(testSize);

  int repeats = 20;


  // std::cout << "--------------------------------------------------------------------------------------------\n";

  std::cout << "Blocking perform: " << countAverageTime(blockingBatchTestValidity, testSize, repeats) << "ms.\n";
  std::cout << "Async    perform: " << countAverageTime(asynchBatchTestValidity, testSize, repeats) << "ms.\n";
  std::cout << "Single   handle : " << countAverageTime(linearTestValidity, testSize, repeats) << "ms.\n";
  std::cout << "Signle no reuse : " << countAverageTime(linearTestNoReuseValidity, testSize, repeats) << "ms.\n";

  curl_global_cleanup();

  return 0;
}