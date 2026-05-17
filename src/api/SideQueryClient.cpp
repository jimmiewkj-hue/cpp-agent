#include "api/SideQueryClient.h"

#include "api/ModelClient.h"

#include <exception>

namespace agent {
namespace api {

SideQueryClient::SideQueryClient(ModelClient& modelClient)
    : modelClient_(modelClient) {}

SideQueryResponse SideQueryClient::Query(
    const SideQueryRequest& request) const {
  SideQueryResponse response;

  try {
    response.messages = modelClient_.SideQuery(
        request.messages, request.systemPrompt, request.model);
    response.ok = true;
  } catch (const std::exception& ex) {
    response.ok = false;
    response.error = ex.what();
  } catch (...) {
    response.ok = false;
    response.error = "unknown side-query failure";
  }

  return response;
}

}  // namespace api
}  // namespace agent
