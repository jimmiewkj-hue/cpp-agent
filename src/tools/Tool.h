#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"
#include "third_party/nlohmann_json.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// Permission types (aligned with local-ace PermissionResult)
// ============================================================================

// ============================================================================
// Legacy types (backward compatibility with existing code)
// ============================================================================
enum class ToolExecCategory {
  ReadOnly,
  FileWrite,
  ShellCommand,
  SubAgent,
  McpTool,
};

struct ToolSchema {
  std::string name;
  std::string description;
  std::string inputSchemaJson;
  ToolExecCategory category = ToolExecCategory::ReadOnly;
  bool readOnlyHint = false;
  bool destructiveHint = false;
  int maxResultSizeChars = 100000;
};

enum class PermissionBehavior {
  Allow,
  Deny,
  AskUser,
};

struct PermissionResult {
  PermissionBehavior behavior = PermissionBehavior::Allow;
  json updatedInput = json::object();
  std::string reason;
  bool isAutoApproved = true;
};

// ============================================================================
// Validation types (aligned with local-ace ValidationResult)
// ============================================================================
enum class ValidationAction {
  Allow,
  Block,
  Rewrite,
};

struct ValidationIntervention {
  std::string toolUseId;
  ValidationAction action = ValidationAction::Allow;
  std::optional<std::string> correctedName;
  std::optional<json> correctedInput;
  std::string reason;
};

struct ValidationResult {
  bool ok = true;
  std::string errorMessage;
  std::vector<ValidationIntervention> interventions;
};

// ============================================================================
// Tool progress types
// ============================================================================
struct ToolProgress {
  std::string toolUseId;
  std::string status;  // "running", "completed", "error"
  json data = json::object();
};

using ToolProgressCallback = std::function<void(const ToolProgress&)>;

// ============================================================================
// ToolUseContext - dependency injection container (aligned with local-ace)
// ============================================================================
struct ToolUseContext {
  // Tool system
  std::vector<core::ContentBlock> toolUseBlocks;
  const core::Message* parentMessage = nullptr;

  // Permissions
  std::function<bool(const std::string&)> isAlwaysAllowed;
  std::function<void(const std::string&, const std::string&)> recordDenial;

  // File state tracking (mimics local-ace readFileState)
  std::function<bool(const std::string&)> hasFileBeenRead;
  std::function<void(const std::string&, const std::string&)> recordFileRead;

  // Abort signal
  std::function<bool()> isAborted;

  // Workspace
  std::string workspaceRoot;

  // Model info
  std::string mainModel;
  bool isNonInteractive = false;

  // Memory attachment tracking (mimics local-ace nestedMemoryAttachmentTriggers)
  std::vector<std::string> memoryAttachmentTriggers;
};

// ============================================================================
// ToolCallResult
// ============================================================================
struct ToolCallResult {
  bool ok = true;
  std::string content;
  std::string error;
  bool isError = false;
  json metadata = json::object();
};

// ============================================================================
// Tool interface (aligned with local-ace Tool<Input, Output, P>)
// ============================================================================
class Tool {
 public:
  virtual ~Tool() = default;

  // Primary tool execution
  virtual ToolCallResult Call(
      const json& input,
      const ToolUseContext& context,
      ToolProgressCallback onProgress = nullptr) = 0;

  // Dynamic description based on input context
  virtual std::string Description(const json& input) const {
    return userFacingDescription_;
  }

  // Schema
  virtual const std::string& Name() const { return name_; }
  virtual json InputSchema() const { return json::parse(inputSchemaJson_); }
  virtual std::string InputSchemaJson() const { return inputSchemaJson_; }

  // Capabilities
  virtual bool IsEnabled() const { return true; }
  virtual bool IsReadOnly(const json& /*input*/) const { return readOnlyHint_; }
  virtual bool IsConcurrencySafe(const json& /*input*/) const { return false; }
  virtual bool IsDestructive(const json& /*input*/) const { return destructiveHint_; }

  // Validation (called before permission check)
  virtual ValidationResult ValidateInput(const json& input) const {
    return ValidationResult{};
  }

  // Permission check (called after validation)
  virtual PermissionResult CheckPermissions(
      const json& input,
      const ToolUseContext& context) const {
    return PermissionResult{};
  }

  // User-facing metadata
  virtual std::string UserFacingDescription() const { return userFacingDescription_; }
  virtual std::string UserFacingName(const json& /*input*/) const { return name_; }

  // Maximum result size before persistence
  virtual int MaxResultSizeChars() const { return maxResultSizeChars_; }

  // Path extraction (for tools that operate on files)
  virtual std::optional<std::string> GetPath(const json& input) const {
    return std::nullopt;
  }

  // Search hint for tool discovery
  virtual std::string SearchHint() const { return ""; }

  // Aliases for backwards compatibility
  virtual std::vector<std::string> Aliases() const { return aliases_; }

  // Check if two inputs are equivalent (for duplicate detection, aligned with local-ace)
  virtual bool InputsEquivalent(const json& a, const json& b) const {
    return a.dump() == b.dump();
  }

  // Returns rendering hints for the tool (aligned with local-ace isSearchOrRead)
  struct RenderHint {
    bool isSearch = false;
    bool isRead = false;
    bool isList = false;
  };
  virtual RenderHint GetRenderHint(const json& /*input*/) const {
    return RenderHint{};
  }

  // Interrupt behavior when user submits while tool is running (aligned with local-ace)
  enum class InterruptBehavior { Cancel, Block };
  virtual InterruptBehavior GetInterruptBehavior() const {
    return InterruptBehavior::Block;
  }

 protected:
  std::string name_;
  std::string userFacingDescription_;
  std::string inputSchemaJson_ = "{}";
  bool readOnlyHint_ = false;
  bool destructiveHint_ = false;
  int maxResultSizeChars_ = 100000;
  std::vector<std::string> aliases_;
};

// ============================================================================
// ToolDef - partial definition for builder pattern (aligned with local-ace)
// ============================================================================
struct ToolDef {
  std::string name;
  std::string description;
  std::string inputSchemaJson = "{}";
  bool readOnlyHint = false;
  bool destructiveHint = false;
  int maxResultSizeChars = 100000;
  std::vector<std::string> aliases;
  std::string searchHint;

  // Required call function
  std::function<ToolCallResult(const json&, const ToolUseContext&, ToolProgressCallback)> call;

  // Optional overrides
  std::function<bool()> isEnabled;
  std::function<bool(const json&)> isReadOnly;
  std::function<bool(const json&)> isConcurrencySafe;
  std::function<bool(const json&)> isDestructive;
  std::function<ValidationResult(const json&)> validateInput;
  std::function<PermissionResult(const json&, const ToolUseContext&)> checkPermissions;
  std::function<std::string(const json&)> descriptionFn;
  std::function<std::optional<std::string>(const json&)> getPath;

  // Local-ace aligned optional overrides
  std::function<bool(const json&, const json&)> inputsEquivalent;
  std::function<Tool::RenderHint(const json&)> getRenderHint;
  std::function<Tool::InterruptBehavior()> getInterruptBehavior;
};

// ============================================================================
// ConcreteTool - adapter from ToolDef to Tool interface
// ============================================================================
class ConcreteTool : public Tool {
 public:
  explicit ConcreteTool(ToolDef def);

  ToolCallResult Call(
      const json& input,
      const ToolUseContext& context,
      ToolProgressCallback onProgress = nullptr) override;

  std::string Description(const json& input) const override;
  bool IsEnabled() const override;
  bool IsReadOnly(const json& input) const override;
  bool IsConcurrencySafe(const json& input) const override;
  bool IsDestructive(const json& input) const override;
  ValidationResult ValidateInput(const json& input) const override;
  PermissionResult CheckPermissions(
      const json& input,
      const ToolUseContext& context) const override;
  std::string UserFacingName(const json& input) const override;
  std::optional<std::string> GetPath(const json& input) const override;
  std::string SearchHint() const override;
  bool InputsEquivalent(const json& a, const json& b) const override;
  RenderHint GetRenderHint(const json& input) const override;
  InterruptBehavior GetInterruptBehavior() const override;

 private:
  std::function<ToolCallResult(const json&, const ToolUseContext&, ToolProgressCallback)> call_;
  std::function<bool()> isEnabled_;
  std::function<bool(const json&)> isReadOnly_;
  std::function<bool(const json&)> isConcurrencySafe_;
  std::function<bool(const json&)> isDestructive_;
  std::function<ValidationResult(const json&)> validateInput_;
  std::function<PermissionResult(const json&, const ToolUseContext&)> checkPermissions_;
  std::function<std::string(const json&)> descriptionFn_;
  std::function<std::optional<std::string>(const json&)> getPath_;
  std::string searchHint_;

  // Local-ace aligned optional overrides
  std::function<bool(const json&, const json&)> inputsEquivalent_;
  std::function<Tool::RenderHint(const json&)> getRenderHint_;
  std::function<Tool::InterruptBehavior()> getInterruptBehavior_;
};

// ============================================================================
// BuildTool - factory function (aligned with local-ace buildTool)
// ============================================================================
inline std::unique_ptr<Tool> BuildTool(ToolDef def) {
  return std::make_unique<ConcreteTool>(std::move(def));
}

// ============================================================================
// ToolCollection - readonly registry of tools (aligned with local-ace Tools)
// ============================================================================
using ToolCollection = std::vector<std::unique_ptr<Tool>>;

}  // namespace tools
}  // namespace agent