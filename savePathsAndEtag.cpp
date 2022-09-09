#include <iostream>
#include <string>
#include <curl/curl.h>
#include <vector>
#include "rapidjson/rapidjson.h"
#include "rapidjson/document.h"
#include "unordered_map"

/*
g++ -std=c++11 savePathsAndEtag.cpp -lpthread -lcurl -luv -o savePathsAndEtag && ./savePathsAndEtag
*/

size_t writeToString(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  char *conts = (char *)contents;
  for (int i = 0; i < nmemb; i++)
  {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

bool headersContainEtag(std::string headers) {

  auto etagLine = headers.find("ETag");
  return (etagLine != std::string::npos);
}

std::string extractETAG(std::string headers)
{
  auto etagLine = headers.find("ETag");

  auto etagStart = headers.find("\"", etagLine)+1;
  auto etagEnd = headers.find("\"", etagStart+1);
  return headers.substr(etagStart, etagEnd - etagStart);
}

void setHandleOptions(CURL* handle, std::string* dst, std::string* headers, std::string* path)
{
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, headers);

  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  curl_easy_setopt(handle, CURLOPT_URL, path->c_str());
}

bool containsAttribute(int ind, std::string attributeName, rapidjson::Document *doc)
{
  return ((*doc)["objects"][ind].FindMember(attributeName.c_str()) != (*doc)["objects"][ind].MemberEnd());
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

std::unordered_map<int, std::string*> createPathsFromMetadata(std::string metadata, std::string baseUrl)
{
  rapidjson::Document doc;
  doc.Parse(metadata.c_str());
  size_t len = countObjects(&doc);
  std::unordered_map<int, std::string*> paths;
  for(int i = 0; i < len; i++) {
    paths[i] = new std::string(baseUrl + "/" + getStringAttribute(i, "path", &doc) + "/" + getLongAttribute(i, "validFrom", &doc));
  }
  return paths;
}

std::unordered_map<int, std::string*> createPaths(std::string queryUrl, std::string serverUrl)
{
  CURL* handle = curl_easy_init();  
  std::string dst;
  curl_easy_setopt(handle, CURLOPT_URL, queryUrl.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &dst);
  curl_easy_perform(handle);
  curl_easy_cleanup(handle);

  return createPathsFromMetadata(dst, serverUrl);
}

size_t fakeWrite(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  return size * nmemb;
}

std::unordered_map<int, std::string*> createEtags(std::unordered_map<int, std::string*> *paths)
{
  CURL* handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, fakeWrite);
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, writeToString);
  
  std::unordered_map<int, std::string*> headers;

  int size = paths->size();

  for(int i = 0; i < size; i++) {
    if (paths->find(i) != paths->end()) {

      curl_easy_setopt(handle, CURLOPT_WRITEDATA, nullptr);
      curl_easy_setopt(handle, CURLOPT_URL, (*paths)[i]->c_str());  
      headers[i] = new std::string();
      curl_easy_setopt(handle, CURLOPT_HEADERDATA, headers[i]);
      curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

      auto curlCode = curl_easy_perform(handle);

      if (curlCode != CURLE_OK && curlCode != CURLE_UNSUPPORTED_PROTOCOL) {
        std::cout << "ERROR. Url code: " << curlCode << "\n";
      }
      
      long httpCode;
      curl_easy_getinfo(handle, CURLINFO_HTTP_CODE, &httpCode);

      if ((httpCode != 200 && httpCode != 303) || !headersContainEtag(*headers[i])) {
        std::cout << "Erased " << *(*paths)[i] << "\n";
        std::cout << "Http code: " << httpCode << "\n";
        std::cout << "Etag received? " << (headersContainEtag(*headers[i]) ? "Yes." : "No.") << "\n\n";
        // std::cout << "Headers: " << *headers[i] << "\n\n";
        paths->erase(i);
        headers.erase(i);
      } else {
        *headers[i] = extractETAG(*headers[i]);
      }
    }
  }
  curl_easy_cleanup(handle);
  return headers;
}

int main()
{
  // std::string serverUrl = "http://ccdb-test.cern.ch:8080";
  // std::string queryUrl = "http://ccdb-test.cern.ch:8080/latest/%5Cw%7B3%7D/.*/1659949694000?Accept=application/json";

  std::string serverUrl = "http://alice-ccdb.cern.ch";
  std::string queryUrl = "http://alice-ccdb.cern.ch/latest/%5Cw%7B3%7D/.*/1659949694000?Accept=application/json";

  curl_global_init(CURL_GLOBAL_ALL);
  auto paths = createPaths(queryUrl, serverUrl);
  int total = paths.size();
  auto etags = createEtags(&paths);
  int omitted = total - etags.size();
  curl_global_cleanup();

  std::string headerString = "std::string etagsCS = \"";
  for(auto etag : etags) headerString += *etag.second + ",";
  headerString.pop_back();
  headerString += "\";";

  std::string pathString = "std::string pathsCS = \"";
  for(auto path : paths) pathString += *path.second + ",";
  pathString.pop_back();
  pathString += "\";";

  std::cout << pathString << "\n\n" << headerString << "\n";

  std::cout.precision(1);
  std::cout << "\nOmitted " << (double)omitted / total << "% etags and paths.\n";

  return 0;
}