#include "tools/Tool.h"

namespace agent {
namespace tools {

ConcreteTool::ConcreteTool(ToolDef def) {
  name_ = std::move(def.name);
  userFacingDescription_ = std::move(def.description);
  inputSchemaJson_ = std::move(def.inputSchemaJson);
  readOnlyHint_ = def.readOnlyHint;
  destructiveHint_ = def.destructiveHint;
  maxResultSizeChars_ = def.maxResultSizeChars;
  aliases_ = std::move(def.aliases);
  searchHint_ = std::move(def.searchHint);

  call_ = std::move(def.call);

  if (def.isEnabled)
    isEnabled_ = std::move(def.isEnabled);
  else
    isEnabled_ = []() { return true; };

  if (def.isReadOnly)
    isReadOnly_ = std::move(def.isReadOnly);
  else
    isReadOnly_ = [hint = readOnlyHint_](const json&) { return hint; };

  if (def.isConcurrencySafe)
    isConcurrencySafe_ = std::move(def.isConcurrencySafe);
  else
    isConcurrencySafe_ = [](const json&) { return false; };

  if (def.isDestructive)
    isDestructive_ = std::move(def.isDestructive);
  else
    isDestructive_ = [hint = destructiveHint_](const json&) { return hint; };

  if (def.validateInput)
    validateInput_ = std::move(def.validateInput);
  else
    validateInput_ = [](const json&) { return ValidationResult{}; };

  if (def.checkPermissions)
    checkPermissions_ = std::move(def.checkPermissions);
  else
    checkPermissions_ = [](const json& input, const ToolUseContext&) {
          PermissionResult r;
          r.updatedInput = input;
          return r;
        };

  if (def.descriptionFn)
    descriptionFn_ = std::move(def.descriptionFn);
  else
    descriptionFn_ = [desc = userFacingDescription_](const json&) { return desc; };

  if (def.getPath)
    getPath_ = std::move(def.getPath);
  else
    getPath_ = [](const json&) -> std::optional<std::string> { return std::nullopt; };
}

ToolCallResult ConcreteTool::Call(
    const json& input,
    const ToolUseContext& context,
    ToolProgressCallback onProgress) {
  if (!IsEnabled()) {
    ToolCallResult r;
    r.ok = false;
    r.isError = true;
    r.error = "Tool is disabled: " + name_;
    return r;
  }

  if (onProgress) {
    ToolProgress p;
    p.toolUseId = "";
    p.status = "running";
    onProgress(p);
  }

  auto result = call_(input, context, onProgress);

  if (onProgress) {
    ToolProgress p;
    p.toolUseId = "";
    p.status = result.ok ? "completed" : "error";
    p.data = result.metadata;
    onProgress(p);
  }

  return result;
}

std::string ConcreteTool::Description(const json& input) const {
  return descriptionFn_(input);
}

bool ConcreteTool::IsEnabled() const {
  return isEnabled_();
}

bool ConcreteTool::IsReadOnly(const json& input) const {
  return isReadOnly_(input);
}

bool ConcreteTool::IsConcurrencySafe(const json& input) const {
  return isConcurrencySafe_(input);
}

bool ConcreteTool::IsDestructive(const json& input) const {
  return isDestructive_(input);
}

ValidationResult ConcreteTool::ValidateInput(const json& input) const {
  return validateInput_(input);
}

PermissionResult ConcreteTool::CheckPermissions(
    const json& input,
    const ToolUseContext& context) const {
  return checkPermissions_(input, context);
}

std::string ConcreteTool::UserFacingName(const json& input) const {
  (void)input;
  return name_;
}

std::optional<std::string> ConcreteTool::GetPath(const json& input) const {
  return getPath_(input);
}

bool ConcreteTool::InputsEquivalent(const json& a, const json& b) const {
  if (inputsEquivalent_) return inputsEquivalent_(a, b);
  return a.dump() == b.dump();
}

Tool::RenderHint ConcreteTool::GetRenderHint(const json& input) const {
  if (getRenderHint_) return getRenderHint_(input);
  return RenderHint{};
}

Tool::InterruptBehavior ConcreteTool::GetInterruptBehavior() const {
  if (getInterruptBehavior_) return getInterruptBehavior_();
  return InterruptBehavior::Block;
}

std::string ConcreteTool::SearchHint() const {
  return searchHint_;
}

}  // namespace tools
}  // namespace agent