#include "api/SideQueryClient.h"

#include "api/ModelClient.h"

#include <exception>

namespace agent {
namespace api {

SideQueryClient::SideQueryClient(ModelClient& modelClient)
    : modelClient_(modelClient) {}

// STRENGTHEN-T25
void SideQueryClient::SetValidatorClient(
    std::unique_ptr<HttpLlmClient> client) {
  validatorClient_ = std::move(client);
}

SideQueryResponse SideQueryClient::Query(
    const SideQueryRequest& request) const {
  SideQueryResponse response;

  // STRENGTHEN-T25: route validator queries to the dedicated validator
  // client when one is configured (separate endpoint/key for cloud
  // validators). All other side-queries (classifier, memory, contract)
  // use the shared main-model client.
  ModelClient* target = &modelClient_;
  if (request.querySource == "validator" && validatorClient_) {
    target = validatorClient_.get();
  }

  try {
    response.messages = target->SideQuery(
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
