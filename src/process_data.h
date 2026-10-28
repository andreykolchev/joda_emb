#pragma once

// The process this coordinator runs, parsed at boot.
constexpr char PROCESS_JSON[] = R"json({
  "version": "1.0",
  "process": {
    "id": "temperature-monitor",
    "nodes": [
      { "id": "start", "type": "Start" },
      { "id": "temperature", "type": "ExternalTask", "externalTaskType": "measure_temperature", "resultVariable": "temperature" },
      { "id": "wait-1", "type": "InternalTask", "internalTaskType": "Delay", "delayMs": 2000 },
      { "id": "count-1", "type": "InternalTask", "internalTaskType": "Increment", "variable": "count-1", "increment": 1 },
      { "id": "router-1", "type": "Router" },
      { "id": "humidity", "type": "ExternalTask", "externalTaskType": "measure_humidity", "resultVariable": "humidity" },
      { "id": "wait-2", "type": "InternalTask", "internalTaskType": "Delay", "delayMs": 2000 },
      { "id": "count-2", "type": "InternalTask", "internalTaskType": "Increment", "variable": "count-2", "increment": 1 },
      { "id": "router-2", "type": "Router" },
      { "id": "stop", "type": "Stop" }
    ],
    "routes": [
      { "from": "start", "to": "temperature" },
      { "from": "temperature", "to": "wait-1" },
      { "from": "wait-1", "to": "count-1" },
      { "from": "count-1", "to": "router-1" },
      { "from": "router-1", "to": "temperature", "condition": { "variable": "count-1", "op": "<", "value": 5 } },
      { "from": "router-1", "to": "humidity" },
      { "from": "humidity", "to": "wait-2" },
      { "from": "wait-2", "to": "count-2" },
      { "from": "count-2", "to": "router-2" },
      { "from": "router-2", "to": "humidity", "condition": { "variable": "count-2", "op": "<", "value": 5 } },
      { "from": "router-2", "to": "stop" }
    ]
  }
})json";
