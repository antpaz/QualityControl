// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

///
/// \file   InfrastructureGenerator.cxx
/// \author Piotr Konopka
///

#include "QualityControl/InfrastructureGenerator.h"

#include "QualityControl/TaskRunner.h"
#include "QualityControl/TaskRunnerFactory.h"
#include "QualityControl/AggregatorRunnerFactory.h"
#include "QualityControl/CheckRunner.h"
#include "QualityControl/Check.h"
#include "QualityControl/CheckRunnerFactory.h"
#include "QualityControl/PostProcessingDevice.h"
#include "QualityControl/Version.h"
#include "QualityControl/QcInfoLogger.h"
#include "QualityControl/TaskSpec.h"
#include "QualityControl/InfrastructureSpecReader.h"
#include "QualityControl/InfrastructureSpec.h"
#include "QualityControl/RootFileSink.h"
#include "QualityControl/RootFileSource.h"

#include <Configuration/ConfigurationFactory.h>
#include <Framework/DataSpecUtils.h>
#include <Framework/ExternalFairMQDeviceProxy.h>
#include <Framework/DataDescriptorQueryBuilder.h>
#include <Framework/O2ControlLabels.h>
#include <Mergers/MergerInfrastructureBuilder.h>
#include <Mergers/MergerBuilder.h>
#include <DataSampling/DataSampling.h>

#include <algorithm>
#include <set>

using namespace o2::framework;
using namespace o2::configuration;
using namespace o2::mergers;
using namespace o2::utilities;
using namespace o2::quality_control::checker;
using namespace o2::quality_control::postprocessing;
using boost::property_tree::ptree;
using SubSpec = o2::header::DataHeader::SubSpecificationType;

namespace o2::quality_control::core
{

uint16_t defaultPolicyPort = 42349;

struct DataSamplingPolicySpec {
  DataSamplingPolicySpec(std::string name, std::string control) : name(std::move(name)), control(std::move(control)) {}
  bool operator<(const DataSamplingPolicySpec other) const
  {
    return std::tie(name, control) < std::tie(other.name, other.control);
  }
  std::string name;
  std::string control;
};

framework::WorkflowSpec InfrastructureGenerator::generateStandaloneInfrastructure(std::string configurationSource)
{
  printVersion();

  auto config = ConfigurationFactory::getConfiguration(configurationSource);
  auto infrastructureSpec = InfrastructureSpecReader::readInfrastructureSpec(config->getRecursive(), configurationSource);
  // todo: report the number of tasks/checks/etc once all are read there.

  WorkflowSpec workflow;

  for (const auto& taskSpec : infrastructureSpec.tasks) {
    if (taskSpec.active) {
      // The "resetAfterCycles" parameters should be handled differently for standalone/remote and local tasks,
      // thus we should not let TaskRunnerFactory read it and decide by itself, since it might not be aware of
      // the context we run QC.
      auto taskConfig = TaskRunnerFactory::extractConfig(infrastructureSpec.common, taskSpec, 0, taskSpec.resetAfterCycles);
      workflow.emplace_back(TaskRunnerFactory::create(taskConfig));
    }
  }
  auto checkRunnerOutputs = generateCheckRunners(workflow, configurationSource);
  generateAggregator(workflow, configurationSource, checkRunnerOutputs);
  generatePostProcessing(workflow, configurationSource);

  return workflow;
}

void InfrastructureGenerator::generateStandaloneInfrastructure(framework::WorkflowSpec& workflow, std::string configurationSource)
{
  auto qcInfrastructure = InfrastructureGenerator::generateStandaloneInfrastructure(configurationSource);
  workflow.insert(std::end(workflow), std::begin(qcInfrastructure), std::end(qcInfrastructure));
}

WorkflowSpec InfrastructureGenerator::generateLocalInfrastructure(std::string configurationSource, std::string targetHost)
{
  printVersion();

  auto config = ConfigurationFactory::getConfiguration(configurationSource);
  auto infrastructureSpec = InfrastructureSpecReader::readInfrastructureSpec(config->getRecursive(), configurationSource);

  WorkflowSpec workflow;
  std::set<DataSamplingPolicySpec> samplingPoliciesUsed;

  if (infrastructureSpec.tasks.empty()) {
    return workflow;
  }

  for (const auto& taskSpec : infrastructureSpec.tasks) {
    if (!taskSpec.active) {
      ILOG(Info, Devel) << "Task " << taskSpec.taskName << " is disabled, ignoring." << ENDM;
      continue;
    }

    if (taskSpec.location == TaskLocationSpec::Local) {
      if (taskSpec.localMachines.empty()) {
        throw std::runtime_error("No local machines specified for task " + taskSpec.taskName + " in its configuration");
      }

      size_t id = 1;
      for (const auto& machine : taskSpec.localMachines) {
        // We spawn a task and proxy only if we are on the right machine.
        if (machine == targetHost) {
          // If we use delta mergers, then the moving window is implemented by the last Merger layer.
          // The QC Tasks should always send a delta covering one cycle.
          int resetAfterCycles = taskSpec.mergingMode == "delta" ? 1 : (int)taskSpec.resetAfterCycles;
          auto taskConfig = TaskRunnerFactory::extractConfig(infrastructureSpec.common, taskSpec, id, resetAfterCycles);
          // Generate QC Task Runner
          workflow.emplace_back(TaskRunnerFactory::create(taskConfig));
          // Generate an output proxy
          // These should be removed when we are able to declare dangling output in normal DPL devices
          generateLocalTaskLocalProxy(workflow, id, taskSpec.taskName, taskSpec.remoteMachine, std::to_string(taskSpec.remotePort), taskSpec.localControl);
          break;
        }
        id++;
      }
    } else // TaskLocationSpec::Remote
    {
      // Collecting Data Sampling Policies
      switch (taskSpec.dataSource.type) {
        case DataSourceType::DataSamplingPolicy:
          samplingPoliciesUsed.insert({ taskSpec.dataSource.typeSpecificParams.at("name"), taskSpec.localControl });
          break;
        case DataSourceType::Direct:
          throw std::runtime_error("Configuration error: Remote QC tasks such as " + taskSpec.taskName + " cannot use direct data sources");
        default:
          throw std::runtime_error("Configuration error: unsupported dataSource for remote QC Tasks");
      }
    }
  }

  // Creating Data Sampling Policies proxies
  for (const auto& [policyName, control] : samplingPoliciesUsed) {
    // todo: leave only the new way once the return type is changed
    std::string port = std::to_string(std::optional<uint16_t>(DataSampling::PortForPolicy(config.get(), policyName)).value_or(defaultPolicyPort));
    Inputs inputSpecs = DataSampling::InputSpecsForPolicy(config.get(), policyName);

    std::vector<std::string> machines = DataSampling::MachinesForPolicy(config.get(), policyName);
    for (const auto& machine : machines) {
      if (machine == targetHost) {
        generateDataSamplingPolicyLocalProxy(workflow, policyName, inputSpecs, machine, port, control);
      }
    }
  }

  return workflow;
}

void InfrastructureGenerator::generateLocalInfrastructure(framework::WorkflowSpec& workflow, std::string configurationSource, std::string host)
{
  auto qcInfrastructure = InfrastructureGenerator::generateLocalInfrastructure(configurationSource, host);
  workflow.insert(std::end(workflow), std::begin(qcInfrastructure), std::end(qcInfrastructure));
}

o2::framework::WorkflowSpec InfrastructureGenerator::generateRemoteInfrastructure(std::string configurationSource)
{
  printVersion();

  auto config = ConfigurationFactory::getConfiguration(configurationSource);
  auto infrastructureSpec = InfrastructureSpecReader::readInfrastructureSpec(config->getRecursive(), configurationSource);

  WorkflowSpec workflow;
  std::set<DataSamplingPolicySpec> samplingPoliciesUsed;

  for (const auto& taskSpec : infrastructureSpec.tasks) {
    if (!taskSpec.active) {
      ILOG(Info, Devel) << "Task " << taskSpec.taskName << " is disabled, ignoring." << ENDM;
      continue;
    }

    if (taskSpec.location == TaskLocationSpec::Local) {
      // if tasks are LOCAL, generate input proxies + mergers + checkers

      size_t numberOfLocalMachines = taskSpec.localMachines.size() > 1 ? taskSpec.localMachines.size() : 1;
      // Generate an input proxy
      // These should be removed when we are able to declare dangling inputs in normal DPL devices
      generateLocalTaskRemoteProxy(workflow, taskSpec.taskName, numberOfLocalMachines, std::to_string(taskSpec.remotePort), taskSpec.localControl);

      // In "delta" mode Mergers should implement moving window, in "entire" - QC Tasks.
      size_t resetAfterCycles = taskSpec.mergingMode == "delta" ? taskSpec.resetAfterCycles : 0;
      auto cycleDurationSeconds = taskSpec.cycleDurationSeconds * taskSpec.mergerCycleMultiplier;

      generateMergers(workflow, taskSpec.taskName, numberOfLocalMachines, cycleDurationSeconds, taskSpec.mergingMode, resetAfterCycles, infrastructureSpec.common.monitoringUrl);

    } else if (taskSpec.location == TaskLocationSpec::Remote) {

      // -- if tasks are REMOTE, generate dispatcher proxies + tasks + checkers
      // (for the time being we don't foresee parallel tasks on QC servers, so no mergers here)

      // Collecting Data Sampling Policies
      switch (taskSpec.dataSource.type) {
        case DataSourceType::DataSamplingPolicy:
          samplingPoliciesUsed.insert({ taskSpec.dataSource.typeSpecificParams.at("name"), taskSpec.localControl });
          break;
        case DataSourceType::Direct:
          throw std::runtime_error("Configuration error: Remote QC tasks such as " + taskSpec.taskName + " cannot use direct data sources");
        default:
          throw std::runtime_error("Configuration error: unsupported dataSource for remote QC Tasks");
      }

      // Creating the remote task
      auto taskConfig = TaskRunnerFactory::extractConfig(infrastructureSpec.common, taskSpec, 0, taskSpec.resetAfterCycles);
      workflow.emplace_back(TaskRunnerFactory::create(taskConfig));
    }
  }

  // Creating Data Sampling Policies proxies
  for (const auto& [policyName, control] : samplingPoliciesUsed) {
    // todo now we have to generate one proxy per local machine and policy, because of the proxy limitations.
    //  Use one proxy per policy when it is possible.

    // todo: leave only the new way once the return type is changed
    std::string port = std::to_string(std::optional<uint16_t>(DataSampling::PortForPolicy(config.get(), policyName)).value_or(defaultPolicyPort));
    Outputs outputSpecs = DataSampling::OutputSpecsForPolicy(config.get(), policyName);
    std::vector<std::string> machines = DataSampling::MachinesForPolicy(config.get(), policyName);
    for (const auto& machine : machines) {
      generateDataSamplingPolicyRemoteProxy(workflow, policyName, outputSpecs, machine, port, control);
    }
  }

  generateCheckRunners(workflow, configurationSource);
  generatePostProcessing(workflow, configurationSource);

  return workflow;
}

void InfrastructureGenerator::generateRemoteInfrastructure(framework::WorkflowSpec& workflow, std::string configurationSource)
{
  auto qcInfrastructure = InfrastructureGenerator::generateRemoteInfrastructure(configurationSource);
  workflow.insert(std::end(workflow), std::begin(qcInfrastructure), std::end(qcInfrastructure));
}

framework::WorkflowSpec InfrastructureGenerator::generateLocalBatchInfrastructure(std::string configurationSource, std::string sinkFilePath)
{
  printVersion();

  auto config = ConfigurationFactory::getConfiguration(configurationSource);
  auto infrastructureSpec = InfrastructureSpecReader::readInfrastructureSpec(config->getRecursive(), configurationSource);
  std::vector<InputSpec> fileSinkInputs;

  WorkflowSpec workflow;

  for (const auto& taskSpec : infrastructureSpec.tasks) {
    if (taskSpec.active) {

      // We will merge deltas, thus we need to reset after each cycle (resetAfterCycles==1)
      auto taskConfig = TaskRunnerFactory::extractConfig(infrastructureSpec.common, taskSpec, 0, 1);
      workflow.emplace_back(TaskRunnerFactory::create(taskConfig));

      fileSinkInputs.emplace_back(taskSpec.taskName, TaskRunner::createTaskDataOrigin(), TaskRunner::createTaskDataDescription(taskSpec.taskName));
    }
  }

  if (fileSinkInputs.size() > 0) {
    // todo: could be moved to a factory.
    workflow.push_back({ "qc-root-file-sink",
                         std::move(fileSinkInputs),
                         Outputs{},
                         adaptFromTask<RootFileSink>(sinkFilePath),
                         Options{},
                         CommonServices::defaultServices(),
                         { RootFileSink::getLabel() } });
  }

  return workflow;
}

void InfrastructureGenerator::generateLocalBatchInfrastructure(framework::WorkflowSpec& workflow, std::string configurationSource, std::string sinkFilePath)
{
  auto qcInfrastructure = InfrastructureGenerator::generateLocalBatchInfrastructure(std::move(configurationSource), std::move(sinkFilePath));
  workflow.insert(std::end(workflow), std::begin(qcInfrastructure), std::end(qcInfrastructure));
}

framework::WorkflowSpec InfrastructureGenerator::generateRemoteBatchInfrastructure(std::string configurationSource, std::string sourceFilePath)
{
  printVersion();

  auto config = ConfigurationFactory::getConfiguration(configurationSource);
  auto infrastructureSpec = InfrastructureSpecReader::readInfrastructureSpec(config->getRecursive(), configurationSource);

  WorkflowSpec workflow;

  std::vector<OutputSpec> fileSourceOutputs;
  for (const auto& taskSpec : infrastructureSpec.tasks) {
    if (taskSpec.active) {
      auto taskConfig = TaskRunnerFactory::extractConfig(infrastructureSpec.common, taskSpec, 0, 1);
      fileSourceOutputs.push_back(taskConfig.moSpec);
      fileSourceOutputs.back().binding.value = taskSpec.taskName;
    }
  }
  if (fileSourceOutputs.size() > 0) {
    workflow.push_back({ "qc-root-file-source", {}, std::move(fileSourceOutputs), adaptFromTask<RootFileSource>(sourceFilePath) });
  }

  auto checkRunnerOutputs = generateCheckRunners(workflow, configurationSource);
  generateAggregator(workflow, configurationSource, checkRunnerOutputs);
  generatePostProcessing(workflow, configurationSource);

  return workflow;
}

void InfrastructureGenerator::generateRemoteBatchInfrastructure(framework::WorkflowSpec& workflow, std::string configurationSource, std::string sourceFilePath)
{
  auto qcInfrastructure = InfrastructureGenerator::generateRemoteBatchInfrastructure(configurationSource, sourceFilePath);
  workflow.insert(std::end(workflow), std::begin(qcInfrastructure), std::end(qcInfrastructure));
}

void InfrastructureGenerator::customizeInfrastructure(std::vector<framework::CompletionPolicy>& policies)
{
  TaskRunnerFactory::customizeInfrastructure(policies);
  MergerBuilder::customizeInfrastructure(policies);
  CheckRunnerFactory::customizeInfrastructure(policies);
  AggregatorRunnerFactory::customizeInfrastructure(policies);
  RootFileSink::customizeInfrastructure(policies);
}

void InfrastructureGenerator::printVersion()
{
  // Log the version number
  ILOG(Info, Support) << "QC version " << o2::quality_control::core::Version::GetQcVersion().getString() << ENDM;
}

void InfrastructureGenerator::generateDataSamplingPolicyLocalProxy(framework::WorkflowSpec& workflow,
                                                                   const string& policyName,
                                                                   const framework::Inputs& inputSpecs,
                                                                   const std::string& localMachine,
                                                                   const string& localPort,
                                                                   const std::string& control)
{
  std::string proxyName = policyName + "-proxy";
  std::string channelName = policyName + "-" + localMachine;
  std::string channelConfig = "name=" + channelName + ",type=push,method=bind,address=tcp://*:" + localPort +
                              ",rateLogging=60,transport=zeromq";
  auto channelSelector = [channelName](InputSpec const&, const std::unordered_map<std::string, std::vector<FairMQChannel>>&) {
    return channelName;
  };

  workflow.emplace_back(
    specifyFairMQDeviceMultiOutputProxy(
      proxyName.c_str(),
      inputSpecs,
      channelConfig.c_str(),
      channelSelector));
  workflow.back().labels.emplace_back(control == "odc" ? ecs::preserveRawChannelsLabel : ecs::uniqueProxyLabel);
}

void InfrastructureGenerator::generateDataSamplingPolicyRemoteProxy(framework::WorkflowSpec& workflow,
                                                                    const std::string& policyName,
                                                                    const Outputs& outputSpecs,
                                                                    const std::string& localMachine,
                                                                    const std::string& localPort,
                                                                    const std::string& control)
{
  std::string channelName = policyName + "-" + localMachine;
  const std::string& proxyName = channelName; // channel name has to match proxy name

  std::string channelConfig = "name=" + channelName + ",type=pull,method=connect,address=tcp://" +
                              localMachine + ":" + localPort + ",rateLogging=60,transport=zeromq";

  workflow.emplace_back(specifyExternalFairMQDeviceProxy(
    proxyName.c_str(),
    outputSpecs,
    channelConfig.c_str(),
    dplModelAdaptor()));
  workflow.back().labels.emplace_back(control == "odc" ? ecs::preserveRawChannelsLabel : ecs::uniqueProxyLabel);
}

void InfrastructureGenerator::generateLocalTaskLocalProxy(framework::WorkflowSpec& workflow, size_t id,
                                                          std::string taskName, std::string remoteHost,
                                                          std::string remotePort, const std::string& control)
{
  std::string proxyName = taskName + "-proxy-" + std::to_string(id);
  std::string channelName = taskName + "-proxy";
  InputSpec proxyInput{ channelName, TaskRunner::createTaskDataOrigin(), TaskRunner::createTaskDataDescription(taskName), static_cast<SubSpec>(id) };
  std::string channelConfig = "name=" + channelName + ",type=push,method=connect,address=tcp://" +
                              remoteHost + ":" + remotePort + ",rateLogging=60,transport=zeromq";

  workflow.emplace_back(
    specifyFairMQDeviceMultiOutputProxy(
      proxyName.c_str(),
      { proxyInput },
      channelConfig.c_str()));
  workflow.back().labels.emplace_back(control == "odc" ? ecs::preserveRawChannelsLabel : ecs::uniqueProxyLabel);
}

void InfrastructureGenerator::generateLocalTaskRemoteProxy(framework::WorkflowSpec& workflow, std::string taskName,
                                                           size_t numberOfLocalMachines, std::string remotePort, const std::string& control)
{
  std::string proxyName = taskName + "-proxy"; // channel name has to match proxy name
  std::string channelName = taskName + "-proxy";

  Outputs proxyOutputs;
  for (size_t id = 1; id <= numberOfLocalMachines; id++) {
    proxyOutputs.emplace_back(
      OutputSpec{ { channelName }, TaskRunner::createTaskDataOrigin(), TaskRunner::createTaskDataDescription(taskName), static_cast<SubSpec>(id) });
  }

  std::string channelConfig = "name=" + channelName + ",type=pull,method=bind,address=tcp://*:" + remotePort +
                              ",rateLogging=60,transport=zeromq";

  workflow.emplace_back(specifyExternalFairMQDeviceProxy(
    proxyName.c_str(),
    proxyOutputs,
    channelConfig.c_str(),
    dplModelAdaptor()));
  workflow.back().labels.emplace_back(control == "odc" ? ecs::preserveRawChannelsLabel : ecs::uniqueProxyLabel);
}

void InfrastructureGenerator::generateMergers(framework::WorkflowSpec& workflow, std::string taskName,
                                              size_t numberOfLocalMachines, double cycleDurationSeconds,
                                              std::string mergingMode, size_t resetAfterCycles, std::string monitoringUrl)
{
  Inputs mergerInputs;
  for (size_t id = 1; id <= numberOfLocalMachines; id++) {
    mergerInputs.emplace_back(
      InputSpec{ { taskName + std::to_string(id) },
                 TaskRunner::createTaskDataOrigin(),
                 TaskRunner::createTaskDataDescription(taskName),
                 static_cast<SubSpec>(id) });
  }

  MergerInfrastructureBuilder mergersBuilder;
  mergersBuilder.setInfrastructureName(taskName);
  mergersBuilder.setInputSpecs(mergerInputs);
  mergersBuilder.setOutputSpec(
    { { "main" }, TaskRunner::createTaskDataOrigin(), TaskRunner::createTaskDataDescription(taskName), 0 });
  MergerConfig mergerConfig;
  // if we are to change the mode to Full, disable reseting tasks after each cycle.
  mergerConfig.inputObjectTimespan = { (mergingMode.empty() || mergingMode == "delta") ? InputObjectsTimespan::LastDifference : InputObjectsTimespan::FullHistory };
  mergerConfig.publicationDecision = { PublicationDecision::EachNSeconds, cycleDurationSeconds };
  mergerConfig.mergedObjectTimespan = { MergedObjectTimespan::NCycles, (int)resetAfterCycles };
  // for now one merger should be enough, multiple layers to be supported later
  mergerConfig.topologySize = { TopologySize::NumberOfLayers, 1 };
  mergerConfig.monitoringUrl = monitoringUrl;
  mergersBuilder.setConfig(mergerConfig);

  mergersBuilder.generateInfrastructure(workflow);
}

vector<OutputSpec> InfrastructureGenerator::generateCheckRunners(framework::WorkflowSpec& workflow, std::string configurationSource)
{
  // todo have a look if this complex procedure can be simplified.
  // todo also make well defined and scoped functions to make it more readable and clearer.
  typedef std::vector<std::string> InputNames;
  typedef std::vector<Check> Checks;
  std::map<std::string, o2::framework::InputSpec> tasksOutputMap; // all active tasks' output, as inputs, keyed by their label
  std::map<InputNames, Checks> checksMap;                         // all the Checks defined in the config mapped keyed by their sorted inputNames
  std::map<InputNames, InputNames> storeVectorMap;

  auto config = ConfigurationFactory::getConfiguration(configurationSource);

  if (config->getRecursive("qc").count("tasks")) {
    for (const auto& [taskName, taskConfig] : config->getRecursive("qc.tasks")) {
      if (taskConfig.get<bool>("active", true)) {
        InputSpec taskOutput{ taskName, TaskRunner::createTaskDataOrigin(), TaskRunner::createTaskDataDescription(taskName) };
        tasksOutputMap.insert({ DataSpecUtils::label(taskOutput), taskOutput });
      }
    }
  }

  if (config->getRecursive("qc").count("postprocessing")) {
    for (const auto& [ppTaskName, ppTaskConfig] : config->getRecursive("qc.postprocessing")) {
      if (ppTaskConfig.get<bool>("active", true)) {
        InputSpec taskOutput{ ppTaskName, PostProcessingDevice::createPostProcessingDataOrigin(), PostProcessingDevice::createPostProcessingDataDescription(ppTaskName) };
        tasksOutputMap.insert({ DataSpecUtils::label(taskOutput), taskOutput });
      }
    }
  }

  // For each external task prepare the InputSpec to be stored in tasksoutputMap
  if (config->getRecursive("qc").count("externalTasks")) {
    for (const auto& [taskName, taskConfig] : config->getRecursive("qc.externalTasks")) {
      (void)taskName;
      if (taskConfig.get<bool>("active", true)) {
        auto query = taskConfig.get<std::string>("query");
        Inputs inputs = DataDescriptorQueryBuilder::parse(query.c_str());
        for (const auto& taskOutput : inputs) {
          tasksOutputMap.insert({ DataSpecUtils::label(taskOutput), taskOutput });
        }
      }
    }
  }

  // Instantiate Checks based on the configuration and build a map of checks (keyed by their inputs names)
  if (config->getRecursive("qc").count("checks")) {
    // For every check definition, create a Check
    for (const auto& [checkName, checkConfig] : config->getRecursive("qc.checks")) {
      ILOG(Debug, Devel) << ">> Check name : " << checkName << ENDM;
      if (checkConfig.get<bool>("active", true)) {
        auto check = Check(checkName, configurationSource);
        InputNames inputNames;

        for (auto& inputSpec : check.getInputs()) {
          auto name = DataSpecUtils::label(inputSpec);
          inputNames.push_back(name);
        }
        // Create a grouping key - sorted vector of stringified InputSpecs
        std::sort(inputNames.begin(), inputNames.end());
        // Group checks
        checksMap[inputNames].push_back(check);
      }
    }
  }

  // For every Task output, find a Check to store the MOs in the database.
  // If none is found we create a sink device.
  for (auto& [label, inputSpec] : tasksOutputMap) { // for each task output
    (void)inputSpec;
    bool isStored = false;
    // Look for this task as input in the Checks' inputs, if we found it then we are done
    for (auto& [inputNames, checks] : checksMap) { // for each set of inputs
      (void)checks;
      if (std::find(inputNames.begin(), inputNames.end(), label) != inputNames.end() && inputNames.size() == 1) {
        storeVectorMap[inputNames].push_back(label);
        break;
      }
    }
    if (!isStored) { // fixme: statement is always true
      // If there is no Check for a given input, create a candidate for a sink device
      InputNames singleEntry{ label };
      // Init empty Check vector to appear in the next step
      checksMap[singleEntry];
      storeVectorMap[singleEntry].push_back(label);
    }
  }

  // Create CheckRunners: 1 per set of inputs
  CheckRunnerFactory checkRunnerFactory;
  vector<framework::OutputSpec> checkRunnerOutputs; // needed later for the aggregators
  for (auto& [inputNames, checks] : checksMap) {
    //Logging
    ILOG(Info, Devel) << ">> Inputs (" << inputNames.size() << "): ";
    for (const auto& name : inputNames)
      ILOG(Info, Devel) << name << " ";
    ILOG(Info, Devel) << " ; Checks (" << checks.size() << "): ";
    for (auto& check : checks)
      ILOG(Info, Devel) << check.getName() << " ";
    ILOG(Info, Devel) << " ; Stores (" << storeVectorMap[inputNames].size() << "): ";
    for (auto& input : storeVectorMap[inputNames])
      ILOG(Info, Devel) << input << " ";
    ILOG(Info, Devel) << ENDM;

    if (!checks.empty()) { // Create a CheckRunner for the grouped checks
      DataProcessorSpec spec = checkRunnerFactory.create(checks, configurationSource, storeVectorMap[inputNames]);
      workflow.emplace_back(spec);
      checkRunnerOutputs.insert(checkRunnerOutputs.end(), spec.outputs.begin(), spec.outputs.end());
    } else { // If there are no checks, create a sink CheckRunner
      DataProcessorSpec spec = checkRunnerFactory.createSinkDevice(tasksOutputMap.find(inputNames[0])->second, configurationSource);
      workflow.emplace_back(spec);
      checkRunnerOutputs.insert(checkRunnerOutputs.end(), spec.outputs.begin(), spec.outputs.end());
    }
  }

  ILOG(Info) << ">> Outputs (" << checkRunnerOutputs.size() << "): ";
  for (const auto& output : checkRunnerOutputs)
    ILOG(Info) << DataSpecUtils::describe(output) << " ";
  ILOG(Info) << ENDM;

  return checkRunnerOutputs;
}

void InfrastructureGenerator::generateAggregator(WorkflowSpec& workflow, std::string configurationSource, vector<framework::OutputSpec>& checkRunnerOutputs)
{
  // TODO consider whether we should recompute checkRunnerOutputs instead of receiving it all baked.
  auto config = ConfigurationFactory::getConfiguration(configurationSource);
  if (config->getRecursive("qc").count("aggregators") == 0) {
    ILOG(Debug, Devel) << "No \"aggregators\" structure found in the config file. If no quality aggregation is expected, then it is completely fine." << ENDM;
    return;
  }
  DataProcessorSpec spec = AggregatorRunnerFactory::create(checkRunnerOutputs, configurationSource);
  workflow.emplace_back(spec);
}

void InfrastructureGenerator::generatePostProcessing(WorkflowSpec& workflow, std::string configurationSource)
{
  auto config = ConfigurationFactory::getConfiguration(configurationSource);
  if (config->getRecursive("qc").count("postprocessing") == 0) {
    ILOG(Debug, Devel) << "No \"postprocessing\" structure found in the config file. If no postprocessing is expected, then it is completely fine." << ENDM;
    return;
  }
  for (const auto& [ppTaskName, ppTaskConfig] : config->getRecursive("qc.postprocessing")) {
    if (ppTaskConfig.get<bool>("active", true)) {

      PostProcessingDevice ppTask{ ppTaskName, configurationSource };

      DataProcessorSpec ppTaskSpec{
        ppTask.getDeviceName(),
        ppTask.getInputsSpecs(),
        ppTask.getOutputSpecs(),
        {},
        ppTask.getOptions()
      };

      ppTaskSpec.algorithm = adaptFromTask<PostProcessingDevice>(std::move(ppTask));

      workflow.emplace_back(std::move(ppTaskSpec));
    }
  }
}

} // namespace o2::quality_control::core
