#include "dashboard.h"

#include <ArduinoJson.h>

#include <map>

#include "web_socket.h"

namespace dashboard {
  namespace {
    // What the page shows for a node the current execution has reached.
    struct NodeRun {
      std::string state;  // "running", "succeeded", "failed" or "cancelled"
      uint32_t runs = 0;
      std::string next;    // id of the node the execution moved on to
      std::string result;  // JSON, for an external task
    };

    const char* processJson_ = nullptr;

    // The current (or last) execution.
    uint32_t executionCounter = 0;
    std::string executionId;  // empty until the first execution
    std::string executionStatus;
    std::string reason;
    std::map<std::string, NodeRun> nodes;  // by node id
    std::string current;                   // id of the node being executed
    JsonDocument variables;

    void send(const JsonDocument& doc) {
      std::string text;
      serializeJson(doc, text);
      web_socket::broadcast(text);
    }

    void describeNode(const JsonObject item, const std::string& id, const NodeRun& run) {
      item["execution_id"] = executionId;
      item["node_id"] = id;
      item["state"] = run.state;
      item["runs"] = run.runs;
      if (!run.next.empty()) {
        item["next"] = run.next;
      }
      if (!run.result.empty()) {
        item["result"] = serialized(run.result);
      }
    }

    void setState(const std::string& id, const char* state) {
      if (id.empty()) {
        return;
      }
      NodeRun& run = nodes[id];
      run.state = state;
      JsonDocument doc;
      const auto item = doc.to<JsonObject>();  // clears doc, so set "type" after
      item["type"] = "node_status";
      describeNode(item, id, run);
      send(doc);
    }

    void sendExecution() {
      JsonDocument doc;
      doc["type"] = "execution_status";
      doc["execution_id"] = executionId;
      doc["status"] = executionStatus;
      doc["reason"] = reason;
      send(doc);
    }

    void sendVariables() {
      JsonDocument doc;
      doc["type"] = "variables";
      doc["variables"] = variables;
      send(doc);
    }
  }  // namespace

  void init(const char* processJson) { processJson_ = processJson; }

  static std::string buildSnapshot() {
    if (!processJson_) {
      return "";
    }
    JsonDocument doc;
    doc["type"] = "snapshot";
    doc["process"] = serialized(processJson_);
    doc["variables"] = variables;
    if (executionId.empty()) {
      doc["execution"] = nullptr;
    } else {
      const auto execution = doc["execution"].to<JsonObject>();
      execution["id"] = executionId;
      execution["status"] = executionStatus;
      execution["reason"] = reason;
      const auto list = execution["nodes"].to<JsonArray>();
      for (const auto& [fst, snd] : nodes) {
        describeNode(list.add<JsonObject>(), fst, snd);
      }
    }
    std::string text;
    serializeJson(doc, text);
    return text;
  }

  void onConnected(const uint8_t client) {
    const auto snapshot = buildSnapshot();
    if (!snapshot.empty()) {
      web_socket::sendText(client, snapshot);
    }
  }

  void ExecutionView::processStarted(const core::Process&) {
    executionId = "exec-" + std::to_string(++executionCounter);
    executionStatus = "running";
    reason.clear();
    nodes.clear();
    current.clear();
    variables.clear();
    sendExecution();
    sendVariables();
  }

  void ExecutionView::nodeActivated(const core::Node& node, const uint32_t runs) {
    if (!current.empty()) {
      nodes[current].next = node.id;
      setState(current, "succeeded");
    }
    current = node.id;
    NodeRun& run = nodes[current];
    run.runs = runs;
    run.next.clear();
    run.result.clear();
    setState(current, "running");
  }

  // Shown with the node's next state change.
  void ExecutionView::taskCompleted(const core::Node& node, const std::string& resultJson) {
    nodes[node.id].result = resultJson;
  }

  void ExecutionView::variableSet(const std::string& name, const std::string& valueJson) {
    variables[name] = serialized(valueJson);
    sendVariables();
  }

  void ExecutionView::processFinished(const core::ProcessStatus status, const std::string& message) {
    switch (status) {
      case core::ProcessStatus::Completed:
        executionStatus = "succeeded";
        setState(current, "succeeded");
        break;
      case core::ProcessStatus::Failed:
        executionStatus = "failed";
        reason = message;
        setState(current, "failed");
        break;
      default:  // Stopped
        executionStatus = "cancelled";
        setState(current, "cancelled");
    }
    sendExecution();
  }

  void ExecutionView::log(const std::string& message) {
    JsonDocument doc;
    doc["type"] = "event";
    doc["message"] = message;
    send(doc);
  }
}  // namespace dashboard
