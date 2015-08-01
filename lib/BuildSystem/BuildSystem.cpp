//===-- BuildSystem.cpp ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildSystem.h"
#include "llbuild/BuildSystem/BuildSystemCommandInterface.h"

#include "llvm/ADT/StringRef.h"

#include "llbuild/Core/BuildEngine.h"
#include "llbuild/BuildSystem/BuildFile.h"

#include <memory>

using namespace llbuild;
using namespace llbuild::core;
using namespace llbuild::buildsystem;

BuildSystemDelegate::~BuildSystemDelegate() {}

BuildSystemCommandInterface::~BuildSystemCommandInterface() {}

#pragma mark - BuildSystem implementation

namespace {

class BuildSystemImpl;

/// The delegate used to load the build file for use by a build system.
class BuildSystemFileDelegate : public BuildFileDelegate {
  BuildSystemImpl& system;

public:
  BuildSystemFileDelegate(BuildSystemImpl& system)
      : BuildFileDelegate(), system(system) {}

  BuildSystemDelegate& getSystemDelegate();

  /// @name Delegate Implementation
  /// @{

  virtual void error(const std::string& filename,
                     const std::string& message) override;

  virtual bool configureClient(const std::string& name,
                               uint32_t version,
                               const property_list_type& properties) override;

  virtual std::unique_ptr<Tool> lookupTool(const std::string& name) override;

  virtual void loadedTarget(const std::string& name,
                            const Target& target) override;

  virtual void loadedCommand(const std::string& name,
                             const Command& target) override;

  virtual std::unique_ptr<Node> lookupNode(const std::string& name,
                                           bool isImplicit=false) override;

  /// @}
};

/// The delegate used to build a loaded build file.
class BuildSystemEngineDelegate : public BuildEngineDelegate {
  BuildSystemImpl& system;

  BuildFile& getBuildFile();

  virtual Rule lookupRule(const KeyType& keyData) override;
  virtual void cycleDetected(const std::vector<Rule*>& items) override;

public:
  BuildSystemEngineDelegate(BuildSystemImpl& system) : system(system) {}

  BuildSystemImpl& getBuildSystem() {
    return system;
  }
};

class BuildSystemImpl : public BuildSystemCommandInterface {
  BuildSystem& buildSystem;

  /// The delegate the BuildSystem was configured with.
  BuildSystemDelegate& delegate;

  /// The name of the main input file.
  std::string mainFilename;

  /// The delegate used for the loading the build file.
  BuildSystemFileDelegate fileDelegate;

  /// The build file the system is building.
  BuildFile buildFile;

  /// The delegate used for building the file contents.
  BuildSystemEngineDelegate engineDelegate;

  /// The build engine.
  BuildEngine buildEngine;

  /// @name BuildSystemCommandInterface Implementation
  /// @{


  virtual void taskNeedsInput(core::Task* task, const KeyType& key,
                              uintptr_t inputID) override {
    return buildEngine.taskNeedsInput(task, key, inputID);
  }

  virtual void taskMustFollow(core::Task* task, const KeyType& key) override {
    return buildEngine.taskMustFollow(task, key);
  }

  virtual void taskDiscoveredDependency(core::Task* task,
                                        const KeyType& key) override {
    return buildEngine.taskDiscoveredDependency(task, key);
  }

  virtual void taskIsComplete(core::Task* task, ValueType&& value,
                              bool forceChange) override {
    return buildEngine.taskIsComplete(task, std::move(value), forceChange);
  }

  /// @}
  
public:
  BuildSystemImpl(class BuildSystem& buildSystem,
                  BuildSystemDelegate& delegate,
                  const std::string& mainFilename)
      : buildSystem(buildSystem), delegate(delegate),
        mainFilename(mainFilename),
        fileDelegate(*this), buildFile(mainFilename, fileDelegate),
        engineDelegate(*this), buildEngine(engineDelegate) {}

  BuildSystem& getBuildSystem() {
    return buildSystem;
  }

  BuildSystemDelegate& getDelegate() {
    return delegate;
  }

  const std::string& getMainFilename() {
    return mainFilename;
  }

  BuildSystemCommandInterface& getCommandInterface() {
    return *this;
  }

  BuildFile& getBuildFile() {
    return buildFile;
  }

  BuildEngine& getBuildEngine() {
    return buildEngine;
  }

  /// @name Client API
  /// @{

  bool enableTracing(const std::string& filename, std::string* error_out) {
    return buildEngine.enableTracing(filename, error_out);
  }

  bool build(const std::string& target);

  /// @}
};

#pragma mark - BuildSystem engine integration

/// The system key defines the helpers for translating to and from the key space
/// used by the BuildSystem when using the core BuildEngine.
struct SystemKey {
  enum class Kind {
    /// A key used to identify a command.
    Command,

    /// A key used to identify a node.
    Node,

    /// A key used to identify a target.
    Target,

    /// An invalid key kind.
    Unknown,
  };

  /// The actual key data.
  KeyType key;

private:
  SystemKey(const KeyType& key) : key(key) {}
  SystemKey(char kindCode, llvm::StringRef str) {
    key.reserve(str.size() + 1);
    key.push_back(kindCode);
    key.append(str.begin(), str.end());
  }

public:
  // Support copy and move.
  SystemKey(SystemKey&& rhs) : key(rhs.key) { }
  void operator=(const SystemKey& rhs) {
    if (this != &rhs)
      key = rhs.key;
  }
  SystemKey& operator=(SystemKey&& rhs) {
    if (this != &rhs)
      key = rhs.key;
    return *this;
  }

  // Allow implicit conversion to the contained key.
  operator const KeyType& () const { return getKeyData(); }

  /// @name Construction Functions
  /// @{

  static SystemKey fromKeyData(const KeyType& key) {
    auto result = SystemKey(key);
    assert(result.getKind() != Kind::Unknown && "invalid key");
    return result;
  }

  static SystemKey makeCommand(llvm::StringRef name) {
    return SystemKey('C', name);
  }

  static SystemKey makeNode(llvm::StringRef name) {
    return SystemKey('N', name);
  }

  static SystemKey makeTarget(llvm::StringRef name) {
    return SystemKey('T', name);
  }

  /// @}
  /// @name Accessors
  /// @{

  const KeyType& getKeyData() const { return key; }

  Kind getKind() const {
    switch (key[0]) {
    case 'C': return Kind::Command;
    case 'N': return Kind::Node;
    case 'T': return Kind::Target;
    default:
      return Kind::Unknown;
    }
  }

  bool isCommand() const { return getKind() == Kind::Command; }
  bool isNode() const { return getKind() == Kind::Node; }
  bool isTarget() const { return getKind() == Kind::Target; }

  llvm::StringRef getCommandName() const {
    return llvm::StringRef(key.data()+1, key.size()-1);
  }

  llvm::StringRef getNodeName() const {
    return llvm::StringRef(key.data()+1, key.size()-1);
  }

  llvm::StringRef getTargetName() const {
    return llvm::StringRef(key.data()+1, key.size()-1);
  }

  /// @}
};

/// This is the task used to "build" a target, it translates between the request
/// for building a target key and the requests for all of its nodes.
class TargetTask : public Task {
  Target& target;

  virtual void start(BuildEngine& engine) override {
    // Request all of the necessary system tasks.
    for (const auto& nodeName: target.getNodeNames()) {
      engine.taskNeedsInput(this, SystemKey::makeNode(nodeName),
                            /*InputID=*/0);
    }
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
    // Do nothing.
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
    // Do nothing.
    //
    // FIXME: We may need to percolate an error status here.
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Complete the task immediately.
    engine.taskIsComplete(this, ValueType());
  }

public:
  TargetTask(Target& target) : target(target) {}
};

/// This is the task to "build" a node. It is responsible for selecting the
/// appropriate producer command to run to produce the ndoe, and for
/// synchronizing any external state the node depends on.
class NodeTask : public Task {
  Node& node;
  
  virtual void start(BuildEngine& engine) override {
    // Request the producer command.
    if (node.getProducers().size() == 1) {
      auto command = node.getProducers()[0];
      engine.taskNeedsInput(this, SystemKey::makeCommand(command->getName()),
                            /*InputID=*/0);
      return;
    }

    // FIXME: Delegate to the client to select the appropriate producer if
    // there are more than one.
    assert(0 && "FIXME: not implemented (support for non-unary producers");
    abort();
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Complete the task immediately.
    engine.taskIsComplete(this, ValueType());
  }

public:
  NodeTask(Node& node) : node(node) {}
};

/// This is the task to actually execute a command.
class CommandTask : public Task {
  Command& command;

  static BuildSystemImpl& getBuildSystem(BuildEngine& engine) {
    return static_cast<BuildSystemEngineDelegate*>(
        engine.getDelegate())->getBuildSystem();
  }
    
  virtual void start(BuildEngine& engine) override {
    command.start(getBuildSystem(engine).getCommandInterface(), this);
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    command.inputsAvailable(getBuildSystem(engine).getCommandInterface(), this);
  }

public:
  CommandTask(Command& command) : command(command) {}
};

#pragma mark - BuildSystemEngineDelegate implementation

BuildFile& BuildSystemEngineDelegate::getBuildFile() {
  return system.getBuildFile();
}

Rule BuildSystemEngineDelegate::lookupRule(const KeyType& keyData) {
  // Decode the key.
  auto key = SystemKey::fromKeyData(keyData);

  switch (key.getKind()) {
  default:
    assert(0 && "invalid key");
    abort();

  case SystemKey::Kind::Command: {
    // Find the comand.
    auto it = getBuildFile().getCommands().find(key.getCommandName());
    if (it == getBuildFile().getCommands().end()) {
      assert(0 && "unexpected request for missing command");
      abort();
    }

    // Create the rule for the command.
    Command* command = it->second.get();
    return Rule{
      key,
      /*Action=*/ [command](BuildEngine& engine) -> Task* {
        return engine.registerTask(new CommandTask(*command));
      }
    };
  }
    
  case SystemKey::Kind::Node: {
    // Find the node.
    auto it = getBuildFile().getNodes().find(key.getNodeName());
    if (it == getBuildFile().getNodes().end()) {
      // FIXME: Unknown node name, should map to a default type (a file
      // generally, although we might want to provide a way to put this under
      // control of the client).
      assert(0 && "FIXME: unknown node");
      abort();
    }

    // Create the rule used to construct this node.
    //
    // We could bypass this level and directly return the rule to run the
    // command, which would reduce the number of tasks in the system. For now we
    // do the uniform thing.
    Node* node = it->second.get();
    return Rule{
      key,
      /*Action=*/ [node](BuildEngine& engine) -> Task* {
        return engine.registerTask(new NodeTask(*node));
      }
      // FIXME: Check node validity.
    };
  }

  case SystemKey::Kind::Target: {
    // Find the target.
    auto it = getBuildFile().getTargets().find(key.getTargetName());
    if (it == getBuildFile().getTargets().end()) {
      // FIXME: Invalid target name, produce an error.
      assert(0 && "FIXME: invalid target");
      abort();
    }

    // Create the rule to construct this target.
    Target* target = it->second.get();
    return Rule{
      key,
        /*Action=*/ [target](BuildEngine& engine) -> Task* {
        return engine.registerTask(new TargetTask(*target));
      }
      // FIXME: Check target validitity, we need to check the target list hasn't
      // changed.
    };
  }
  }
}

void BuildSystemEngineDelegate::cycleDetected(const std::vector<Rule*>& items) {
  system.getDelegate().error(system.getMainFilename(),
                             "cycle detected while building");
}

#pragma mark - BuildSystemImpl implementation

bool BuildSystemImpl::build(const std::string& target) {
  // Load the build file.
  //
  // FIXME: Eventually, we may want to support something fancier where we load
  // the build file in the background so we can immediately start building
  // things as they show up.
  //
  // FIXME: We need to load this only once.
  getBuildFile().load();

  // Build the target.
  getBuildEngine().build(SystemKey::makeTarget(target));

  return false;
}

#pragma mark - BuildNode implementation

// FIXME: Figure out how this is going to be organized.
class BuildNode : public Node {
public:
  using Node::Node;

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    // We don't support any custom attributes.
    return false;
  }
};

#pragma mark - ShellTool implementation

class ShellCommand : public Command {
  BuildSystemImpl& system;
  std::vector<Node*> inputs;
  std::vector<Node*> outputs;
  std::string args;

public:
  ShellCommand(BuildSystemImpl& system, const std::string& name)
      : Command(name), system(system) {}

  virtual void configureInputs(const std::vector<Node*>& value) override {
    inputs = value;
  }

  virtual void configureOutputs(const std::vector<Node*>& value) override {
    outputs = value;
  }

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    if (name == "args") {
      args = value;
    } else {
      system.getDelegate().error(
          system.getMainFilename(),
          "unexpected attribute: '" + name + "'");
      return false;
    }

    return true;
  }

  virtual void start(BuildSystemCommandInterface& system, Task* task) override {
  }

  virtual void inputsAvailable(BuildSystemCommandInterface& system,
                               Task* task) override {
    system.taskIsComplete(task, ValueType());
  }
};

class ShellTool : public Tool {
  BuildSystemImpl& system;

public:
  ShellTool(BuildSystemImpl& system, const std::string& name)
      : Tool(name), system(system) {}

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    system.getDelegate().error(
        system.getMainFilename(),
        "unexpected attribute: '" + name + "'");

    // No supported attributes.
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(
      const std::string& name) override {
    return std::make_unique<ShellCommand>(system, name);
  }
};

#pragma mark - BuildSystemFileDelegate

BuildSystemDelegate& BuildSystemFileDelegate::getSystemDelegate() {
  return system.getDelegate();
}

void BuildSystemFileDelegate::error(const std::string& filename,
                                    const std::string& message) {
  // Delegate to the system delegate.
  getSystemDelegate().error(filename, message);
}

bool
BuildSystemFileDelegate::configureClient(const std::string& name,
                                         uint32_t version,
                                         const property_list_type& properties) {
  // The client must match the configured name of the build system.
  if (name != getSystemDelegate().getName())
    return false;

  // FIXME: Give the client an opportunity to respond to the schema version and
  // configuration the properties.

  return true;
}

std::unique_ptr<Tool>
BuildSystemFileDelegate::lookupTool(const std::string& name) {
  // First, give the client an opportunity to create the tool.
  auto tool = getSystemDelegate().lookupTool(name);
  if (tool)
    return std::move(tool);

  // Otherwise, look for one of the builtin tool definitions.
  if (name == "shell") {
    return std::make_unique<ShellTool>(system, name);
  }

  return nullptr;
}

void BuildSystemFileDelegate::loadedTarget(const std::string& name,
                                           const Target& target) {
}

void BuildSystemFileDelegate::loadedCommand(const std::string& name,
                                            const Command& command) {
}

std::unique_ptr<Node>
BuildSystemFileDelegate::lookupNode(const std::string& name,
                                    bool isImplicit) {
  return std::make_unique<BuildNode>(name);
}

}

#pragma mark - BuildSystem

BuildSystem::BuildSystem(BuildSystemDelegate& delegate,
                         const std::string& mainFilename)
    : impl(new BuildSystemImpl(*this, delegate, mainFilename))
{
}

BuildSystem::~BuildSystem() {
  delete static_cast<BuildSystemImpl*>(impl);
}

BuildSystemDelegate& BuildSystem::getDelegate() {
  return static_cast<BuildSystemImpl*>(impl)->getDelegate();
}

bool BuildSystem::enableTracing(const std::string& path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->enableTracing(path, error_out);
}

bool BuildSystem::build(const std::string& name) {
  return static_cast<BuildSystemImpl*>(impl)->build(name);
}
