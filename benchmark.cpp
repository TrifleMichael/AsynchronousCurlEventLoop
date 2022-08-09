#include <iostream>
#include <curl/curl.h>
#include <vector>
#include "rapidjson/rapidjson.h"
#include "rapidjson/document.h"

#include "benchmark.h"

// g++ benchmark.cpp -lcurl -o benchmark && ./benchmark

void test()
{
  std::cout << "Testing\n";
}

size_t writeToString2(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  char *conts = (char *)contents;
  for (int i = 0; i < nmemb; i++)
  {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

std::string getStringAttribute(int ind, std::string attributeName, rapidjson::Document *doc)
{
  
  const char* attrNameChar = attributeName.c_str();
  return (*doc)["objects"][ind][attrNameChar].GetString();
}

std::string getLongAttribute(int ind, std::string attributeName, rapidjson::Document *doc)
{
  
  const char* attrNameChar = attributeName.c_str();
  return std::to_string((*doc)["objects"][ind][attrNameChar].GetInt64());
}


size_t countObjects(rapidjson::Document *doc)
{
  auto objectsArray = (*doc)["objects"].GetArray();
  return objectsArray.Size();
}

std::vector<std::string> createPathsFromMetadata(std::string metadata, std::string baseUrl)
{
  rapidjson::Document doc;
  doc.Parse(metadata.c_str());
  size_t len = countObjects(&doc);
  std::vector<std::string> paths;
  for(int i = 0; i < len; i++) {
    paths.push_back(baseUrl + "/" + getStringAttribute(i, "path", &doc) + "/" + getLongAttribute(i, "validFrom", &doc) + "/" + getStringAttribute(i, "id", &doc));
  }
  return paths;
}

std::vector<std::string> createPaths()
{
  CURL* handle = curl_easy_init();  
  std::string dst;
  curl_easy_setopt(handle, CURLOPT_URL, "http://ccdb-test.cern.ch:8080/latest/%5Cw%7B3%7D/.*/1659949694000?Accept=application/json");
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString2);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &dst);
  curl_easy_perform(handle);
  curl_easy_cleanup(handle);

  return createPathsFromMetadata(dst, "http://ccdb-test.cern.ch:8080");
}

// int main()
// {
//   if (curl_global_init(CURL_GLOBAL_ALL))
//   {
//     fprintf(stderr, "Could not init curl\n");
//     return 1;
//   }


//   auto paths = createPaths();
//   for(int i = 0; i < paths.size(); i++) {
//     std::cout << paths[i] << "\n";
//   }

//   curl_global_cleanup();
//   return 0;
// }