#include "task_dispatcher.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <algorithm>
#include <vector>

#include "config.h"
#include "web_socket.h"

namespace task_dispatcher {
  namespace {
    struct Worker {
      std::string id;  // empty while the slot is free
      std::string ip;
      std::vector<std::string> capabilities;

      // The current (or last) task, shown on the dashboard.
      bool busy = false;
      std::string status = "Idle";
      std::string taskId, taskName, result;
      bool manual = false;     // run from a dashboard, so not the engine's
      bool cancelled = false;  // the engine stopped; the answer is not reported
      uint32_t deadline = 0;   // millis() when a busy worker is given up on
    };

    core::Engine* engine_ = nullptr;
    const core::Process* process_ = nullptr;

    // Indexed by WebSocket client number, so a client's slot is its worker.
    Worker workers[web_socket::MAX_CLIENTS];

    uint32_t taskCounter = 0;

    // The "state" message: every registered worker.
    std::string stateMessage() {
      JsonDocument doc;
      doc["type"] = "state";
      const auto list = doc["workers"].to<JsonArray>();
      for (const Worker& worker : workers) {
        if (worker.id.empty()) {
          continue;
        }
        auto item = list.add<JsonObject>();
        item["id"] = worker.id;
        item["ip"] = worker.ip;
        item["busy"] = worker.busy;
        item["status"] = worker.status;
        item["task_id"] = worker.taskId;
        item["task_name"] = worker.taskName;
        item["result"] = worker.result.empty() ? "-" : worker.result;
        auto capabilities = item["capabilities"].to<JsonArray>();
        for (const std::string& capability : worker.capabilities) {
          capabilities.add(capability);
        }
      }
      std::string text;
      serializeJson(doc, text);
      return text;
    }

    void broadcastState() { web_socket::broadcast(stateMessage()); }

    // Send a task to an idle worker with this capability (and this id, if given).
    core::DispatchResult workerDispatchTask(const std::string& capability, const std::string& workerId,
                                            const std::string& inputsJson, const uint32_t timeoutMs, const bool manual,
                                            std::string& taskId) {
      bool capable = false;
      for (uint8_t client = 0; client < web_socket::MAX_CLIENTS; client++) {
        Worker& worker = workers[client];
        if (worker.id.empty() || (!workerId.empty() && worker.id != workerId) ||
            std::ranges::find(worker.capabilities, capability) == worker.capabilities.end()) {
          continue;
        }
        capable = true;
        if (worker.busy) {
          continue;
        }

        taskId = "task-" + std::to_string(++taskCounter);
        JsonDocument doc;
        doc["type"] = "task";
        doc["task_id"] = taskId;
        doc["name"] = capability;
        doc["inputs"] = serialized(inputsJson);
        doc["timeout_ms"] = timeoutMs;
        std::string text;
        serializeJson(doc, text);
        // A failed send can disconnect the client inside sendText, which resets
        // this slot, so only claim the worker if it is still there.
        if (!web_socket::sendText(client, text) || worker.id.empty()) {
          continue;
        }

        worker.busy = true;
        worker.manual = manual;
        worker.cancelled = false;
        worker.status = "Busy";
        worker.taskId = taskId;
        worker.taskName = capability;
        worker.result.clear();
        worker.deadline = millis() + timeoutMs;
        broadcastState();

        Serial.printf("   sent %s (%s) to %s\n", taskId.c_str(), capability.c_str(), worker.id.c_str());
        return core::DispatchResult::Sent;
      }
      return capable ? core::DispatchResult::Busy : core::DispatchResult::NoWorker;
    }

    // The reason a dashboard's run_task was refused, or "" if it was sent.
    const char* dispatchTaskError(const core::DispatchResult result) {
      switch (result) {
        case core::DispatchResult::Sent:
          return "";
        case core::DispatchResult::NoWorker:
          return "no worker supports this task";
        case core::DispatchResult::Busy:
          break;
      }
      return "no idle worker for this task";
    }

    // Tell the worker to stop its current task. Sent at most once per task.
    void workerCancelTask(const uint8_t client) {
      Worker& worker = workers[client];
      if (worker.cancelled) {
        return;
      }
      worker.cancelled = true;
      JsonDocument doc;
      doc["type"] = "cancel_task";
      doc["task_id"] = worker.taskId;
      std::string text;
      serializeJson(doc, text);
      web_socket::sendText(client, text);
    }

    // Give up the worker's task because the worker is gone or starting over, so
    // the engine hears of it now rather than at the task's timeout.
    void processCancelTask(Worker& worker, const char* reason) {
      if (!worker.busy) {
        return;
      }
      worker.busy = false;
      worker.status = std::string("Failed: ") + reason;
      if (!worker.manual && !worker.cancelled) {
        engine_->onTaskResult(worker.taskId, false, std::string("{\"error\":\"") + reason + "\"}");
      }
    }

    // Commands from the dashboard. Returns the reason it failed, or "" on success.
    std::string processStartStop(const std::string& command) {
      if (command == "start_process") {
        return engine_->start(*process_) ? "" : "the process could not start";
      }
      return engine_->stop() ? "" : "the process is not running";
    }

    void dashboardCommandResult(const uint8_t client, const char* requestId, const std::string& error) {
      JsonDocument doc;
      doc["type"] = "response";
      doc["request_id"] = requestId;
      doc["ok"] = error.empty();
      const auto errors = doc["errors"].to<JsonArray>();
      if (!error.empty()) {
        const auto item = errors.add<JsonObject>();
        item["code"] = "command_failed";
        item["message"] = error;
      }
      std::string text;
      serializeJson(doc, text);
      web_socket::sendText(client, text);
    }

    void handleMessage(const uint8_t client, uint8_t* payload, size_t length) {
      JsonDocument message;
      if (deserializeJson(message, payload, length)) {
        Serial.println("   ignored a message that is not JSON");
        return;
      }
      const std::string type = message["type"] | "";
      if (type == "register") {
        std::string id = message["worker_id"] | "worker";
        if (id.empty()) {
          id = "worker";  // an empty ID would make the slot look free
        }
        // A worker that reconnects may still have a task on its old connection
        for (uint8_t other = 0; other < web_socket::MAX_CLIENTS; other++) {
          if (other != client && workers[other].id == id) {
            processCancelTask(workers[other], "worker re-registered");
            workers[other] = Worker();
            web_socket::disconnect(other);
          }
        }

        Worker& worker = workers[client];
        processCancelTask(worker, "worker re-registered");
        worker = Worker();
        worker.id = id;
        worker.ip = web_socket::remoteIP(client);
        for (JsonVariantConst capability : message["capabilities"].as<JsonArrayConst>()) {
          worker.capabilities.push_back(capability | "");
        }
        Serial.printf("Worker registered: %s\n", worker.id.c_str());
        broadcastState();
      } else if (type == "task_result") {
        const std::string taskId = message["task_id"] | "";
        Worker& worker = workers[client];
        // Only a registered worker answering the task it was given may report it.
        if (worker.id.empty() || taskId.empty() || taskId != worker.taskId) {
          Serial.printf("   ignored result of %s\n", taskId.c_str());
          return;
        }

        std::string result;
        serializeJson(message["result"], result);
        const bool ok = std::string(message["status"] | "") == "completed";
        // Checked even after a timeout, so a late manual result stays away from
        // the engine.
        const bool manual = worker.manual;
        if (worker.busy) {
          worker.busy = false;
          worker.status = worker.cancelled ? "Cancelled" : ok ? "Completed" : "Failed";
          worker.result = result;
          broadcastState();
        }
        // onTaskResult() does not send tasks; the engine sends the next one on its
        // next tick, so it is safe to call from here.
        if (!manual && !worker.cancelled) {
          engine_->onTaskResult(taskId, ok, result);
        }
      } else if (type == "run_task")  // manual task run command
      {
        std::string taskId;
        const core::DispatchResult sent = workerDispatchTask(message["name"] | "", message["worker_id"] | "", "{}",
                                                             MANUAL_TASK_TIMEOUT_MS, true, taskId);
        dashboardCommandResult(client, message["request_id"] | "", dispatchTaskError(sent));
      } else if (type == "start_process" || type == "stop_process")  // start/stop process command
      {
        dashboardCommandResult(client, message["request_id"] | "", processStartStop(type));
      }
    }
  }  // namespace

  void init(core::Engine& engine, const core::Process& process) {
    engine_ = &engine;
    process_ = &process;
    web_socket::begin();
  }

  void loop() {
    // Give up on a worker that does not answer in time: cancel its task and
    // free it for the next one. The engine times out the task on its own.
    bool changed = false;
    for (uint8_t client = 0; client < web_socket::MAX_CLIENTS; client++) {
      Worker& worker = workers[client];
      if (worker.busy && static_cast<int32_t>(millis() - worker.deadline) >= 0) {
        workerCancelTask(client);
        worker.busy = false;
        worker.status = "Failed: timeout";
        changed = true;
      }
    }
    if (changed) {
      broadcastState();
    }
  }

  // web socket events
  void onConnected(const uint8_t client) { web_socket::sendText(client, stateMessage()); }

  void onText(const uint8_t client, uint8_t* payload, const size_t length) { handleMessage(client, payload, length); }

  void onDisconnected(const uint8_t client) {
    Worker& worker = workers[client];
    if (!worker.id.empty()) {
      Serial.printf("Worker disconnected: %s\n", worker.id.c_str());
      processCancelTask(worker, "worker disconnected");
      worker = Worker();
      broadcastState();
    }
  }

  // helper functions
  bool isWorker(const uint8_t client) { return !workers[client].id.empty(); }

  void printWorkers() {
    int count = 0;
    for (const Worker& worker : workers) {
      if (!worker.id.empty()) {
        count++;
      }
    }
    Serial.printf("Workers registered: %d\n", count);

    for (const Worker& worker : workers) {
      if (worker.id.empty()) {
        continue;
      }

      std::string caps;
      for (const std::string& capability : worker.capabilities) {
        if (!caps.empty()) {
          caps += ", ";
        }
        caps += capability;
      }

      Serial.printf("  %-16s %-15s  %s\n", worker.id.c_str(), worker.ip.c_str(), worker.status.c_str());
      Serial.printf("    capabilities: %s\n", caps.c_str());
      Serial.printf("    last result:  %s\n", worker.result.empty() ? "-" : worker.result.c_str());
    }
  }

  const char* processStatusName() {
    switch (engine_->status()) {
      case core::ProcessStatus::Running:
        return "Running";
      case core::ProcessStatus::Completed:
        return "Completed";
      case core::ProcessStatus::Failed:
        return "Failed";
      case core::ProcessStatus::Stopped:
        return "Stopped";
      default:
        return "Idle";
    }
  }

  // process events
  core::DispatchResult WsTaskDispatcher::sendTask(const core::Node& node, std::string& taskId) {
    return workerDispatchTask(node.externalTaskType, "", node.context, node.timeoutMs, false, taskId);
  }

  // event from process to cancel a dispatched task
  void WsTaskDispatcher::cancelTask(const std::string& taskId) {
    for (uint8_t client = 0; client < web_socket::MAX_CLIENTS; client++) {
      Worker& worker = workers[client];
      if (!worker.busy || worker.taskId != taskId) {
        continue;
      }
      // The worker stays busy until it answers or the deadline passes, so it
      // never gets a second task while it is still running this one.
      workerCancelTask(client);
      worker.status = "Cancelling";
      broadcastState();
      return;
    }
  }
}  // namespace task_dispatcher
