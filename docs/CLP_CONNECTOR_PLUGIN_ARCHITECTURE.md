# CLP Velox Connector Plugin Architecture

## Table of Contents

1. [Repository Ownership Summary](#repository-ownership-summary)
2. [Terminology and Glossary](#terminology-and-glossary)
3. [Overview](#overview)
4. [Motivation](#motivation)
5. [Prerequisites](#prerequisites)
6. [Architecture](#architecture)
   - [Current Architecture](#current-architecture)
   - [Plugin Architecture](#plugin-architecture)
   - [Data Flow](#data-flow)
7. [What PR #26650 Provides](#what-pr-26650-provides)
8. [Implementation Plan](#implementation-plan)
   - [Step 1: Create Plugin Directory Structure [PRESTO]](#step-1-create-plugin-directory-structure-presto)
   - [Step 2: Extract ClpPrestoToVeloxConnector [PRESTO]](#step-2-extract-clpprestotoveloxconnector-presto)
   - [Step 3: Create Plugin Entry Point [PRESTO]](#step-3-create-plugin-entry-point-presto)
   - [Step 4: Create Plugin CMakeLists.txt [PRESTO]](#step-4-create-plugin-cmakeliststxt-presto)
   - [Step 5: Remove CLP from Static Build [PRESTO]](#step-5-remove-clp-from-static-build-presto)
9. [File Reference](#file-reference)
10. [Build Instructions](#build-instructions)
11. [Deployment](#deployment)
12. [Verification](#verification)

---

## Repository Ownership Summary

This section clarifies which changes go to which repository for upstream review.

### Change Classification

| Tag | Repository | Upstream Target | Description |
|-----|------------|-----------------|-------------|
| **[PRESTO]** | `presto-native-execution/presto_cpp/` | `prestodb/presto` | Changes to Presto C++ worker code. Submit for Presto reviewer. |
| **[VELOX]** | `presto-native-execution/velox/` | Private repo (y-scope/velox) | Changes to Velox connector code. Stays in private fork. |

### Summary of Changes by Repository

**[PRESTO] - To be upstreamed to prestodb/presto:**
- `presto_cpp/main/connectors/clp_plugin/` (new directory - Steps 1-4)
- `presto_cpp/main/connectors/Registration.cpp` (remove CLP registration - Step 5.1)
- `presto_cpp/main/connectors/PrestoToVeloxConnector.h` (remove CLP class - Step 5.2)
- `presto_cpp/main/connectors/PrestoToVeloxConnector.cpp` (remove CLP implementation - Step 5.3)
- `presto_cpp/main/connectors/CMakeLists.txt` (remove CLP links - Step 5.4)
- `presto_cpp/main/CMakeLists.txt` (remove velox_clp_connector - Step 5.5)
- `presto_cpp/presto_protocol/presto_protocol.cpp` (remove CLP include - Step 5.6)
- `presto_cpp/presto_protocol/connector/clp/` (CLP protocol types - referenced, no changes)

**[VELOX] - Private repo (no upstream):**
- `velox/velox/connectors/clp/` (Velox CLP connector - referenced, no changes needed)
- `velox/velox/connectors/clp/search_lib/` (CLP search library - referenced, no changes needed)

---

## Terminology and Glossary

This section defines key terms used throughout this document. The term "CLP connector" can be ambiguous because there are components in both Presto (Java) and Velox (C++) sides.

### Core Components

| Term | Definition | Location | Repository |
|------|------------|----------|------------|
| **Presto CLP Connector (Java)** | The Java-side connector that runs in the Presto Coordinator. Handles metadata, split generation, and query planning. | `presto-clp/` module in Presto Java codebase | **[PRESTO]** |
| **Velox CLP Connector (C++)** | The C++ connector that runs in the Presto Worker (Velox). Reads data from CLP archives/IR files. | `velox/velox/connectors/clp/` | **[VELOX]** |
| **CLP Plugin** | The `.so` shared library we are creating. Contains Velox CLP Connector + bridge code. | `presto_cpp/main/connectors/clp_plugin/` | **[PRESTO]** |

### Bridge Components

| Term | Definition | Location |
|------|------------|----------|
| **ClpPrestoToVeloxConnector** | Bridge class that converts Presto protocol types to Velox connector types. | `presto_cpp/main/connectors/PrestoToVeloxConnector.cpp` |
| **ClpConnectorProtocol** | JSON serialization/deserialization for CLP-specific types (Split, ColumnHandle, TableHandle). | `presto_cpp/presto_protocol/connector/clp/ClpConnectorProtocol.h` |
| **presto_protocol_clp** | Generated C++ types matching Java-side CLP connector types for JSON serialization. | `presto_cpp/presto_protocol/connector/clp/presto_protocol_clp.h/cpp` |

### Velox CLP Connector Classes

| Term | Definition | Purpose |
|------|------------|---------|
| **ClpConnector** | Main Velox connector class. Implements `Connector` interface. | Creates `ClpDataSource` instances for query execution |
| **ClpConnectorFactory** | Factory class that creates `ClpConnector` instances. | Registered with Velox connector registry |
| **ClpDataSource** | Reads data from CLP splits. Implements `DataSource` interface. | Orchestrates cursor creation and row fetching |
| **ClpConnectorSplit** | Describes a unit of work (archive file or IR stream). | Contains path, split type, and optional KQL query |
| **ClpTableHandle** | Represents the CLP table being queried. | Contains connector ID and table name |
| **ClpColumnHandle** | Represents a column in the CLP table. | Contains column name, original name, and type |

### Protocol Types (Java ↔ C++ Communication)

| Term | Definition | Side |
|------|------------|------|
| **protocol::clp::ClpSplit** | C++ protocol type matching Java `ClpSplit`. | C++ (deserialized from JSON) |
| **protocol::clp::ClpColumnHandle** | C++ protocol type matching Java `ClpColumnHandle`. | C++ (deserialized from JSON) |
| **protocol::clp::ClpTableLayoutHandle** | C++ protocol type matching Java `ClpTableLayoutHandle`. | C++ (deserialized from JSON) |

### Search Library Components

| Term | Definition | Location |
|------|------------|----------|
| **clp-s** | YScope's CLP library for reading compressed log archives. | External dependency |
| **ClpArchiveCursor** | Reads data from CLP archive format. | `velox/connectors/clp/search_lib/archive/` |
| **ClpIrCursor** | Reads data from CLP IR (Intermediate Representation) format. | `velox/connectors/clp/search_lib/ir/` |
| **BaseClpCursor** | Abstract base class for cursors. Defines query execution interface. | `velox/connectors/clp/search_lib/BaseClpCursor.h` |

### Registration Functions

| Term | Definition |
|------|------------|
| **registerConnectorFactory()** | Velox function to register a `ConnectorFactory` with the Velox connector registry. |
| **registerPrestoToVeloxConnector()** | Presto C++ function to register a `PrestoToVeloxConnector` with the bridge registry. |
| **registerExtensions()** | Plugin entry point function called by `presto_server` after `dlopen()`. |

### CMake Library Types

| Term | Definition |
|------|------------|
| **OBJECT library** | CMake library that compiles to `.o` files but isn't linked into an archive. Must use `$<TARGET_OBJECTS:>` to link. |
| **STATIC library** | CMake library that compiles to a `.a` archive file. |
| **SHARED library** | CMake library that compiles to a `.so` (Linux) or `.dylib` (macOS) shared library. |

---

## Overview

This document describes the architecture and implementation procedure for converting the **Velox CLP Connector** into a dynamically loadable plugin (shared library `.so`). This allows the CLP connector to be distributed and loaded independently without requiring Presto to include the entire Velox CLP connector codebase in its build.

The implementation follows [RFC-0019](https://github.com/prestodb/rfcs/blob/main/RFC-0019-connector-plugins.md) which defines the connector plugin infrastructure for Presto C++ workers.

**What This Document Covers:**
- Converting the C++ side (Velox CLP Connector) to a plugin
- The Java side (Presto CLP Connector) remains unchanged

---

## Motivation

| Problem | Solution |
|---------|----------|
| CLP connector requires Presto rebuild when updated | Plugin can be updated independently |
| CLP dependencies (clp-s) bloat Presto binary | Self-contained plugin with static clp-s linking |
| Tight coupling between CLP and Presto codebase | Clean separation via plugin interface |
| Difficult to distribute CLP connector separately | Single `.so` file deployment |

---

## Prerequisites

### Required: PR #26650 Merged

[PR #26650](https://github.com/prestodb/presto/pull/26650) implements the plugin infrastructure from RFC-0019. This provides:

- Plugin loading mechanism (`dlopen`/`dlsym`)
- `registerExtensions()` entry point convention
- `registerPrestoToVeloxConnector()` registration function
- `plugin.dir` configuration property
- Plugin directory scanning at startup

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| clp-s linking | **Static** | Self-contained plugin, simpler deployment |
| Serialization | **JSON** | Works out-of-box; binary codecs optional future optimization |
| Compatibility | **Plugin-only** | Cleaner architecture, easier maintenance |

---

## Architecture

### Current Architecture (Static Linking)

Currently, the Velox CLP Connector is statically linked into the `presto_server` binary:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         presto_server binary                            │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    Registration.cpp                              │   │
│  │  registerConnectorFactories()                                    │   │
│  │    └── ClpConnectorFactory (Velox connector factory)            │   │
│  │  registerPrestoToVeloxConnector()                                │   │
│  │    └── ClpPrestoToVeloxConnector (protocol bridge)              │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │              velox_clp_connector (OBJECT library)                │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────────┐    │   │
│  │  │ClpConnector │ │ClpDataSource│ │     search_lib          │    │   │
│  │  │ClpConfig    │ │ClpTableHandle│ │ArchiveCursor/IrCursor  │    │   │
│  │  └─────────────┘ └─────────────┘ └─────────────────────────┘    │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    clp-s libraries (STATIC)                      │   │
│  │  clp_s::archive_reader, clp_s::search, clp_s::search::kql, etc. │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

**Components in Current Architecture:**

| Component | Location | Purpose |
|-----------|----------|---------|
| **Velox CLP Connector** | `velox/velox/connectors/clp/` | Core connector implementation (ClpConnector, ClpDataSource, etc.) |
| **CLP Search Library** | `velox/velox/connectors/clp/search_lib/` | Archive/IR cursor implementations using clp-s |
| **ClpPrestoToVeloxConnector** | `presto_cpp/main/connectors/PrestoToVeloxConnector.cpp` | Bridge: converts protocol types to Velox types |
| **CLP Protocol Types** | `presto_cpp/presto_protocol/connector/clp/` | C++ types for JSON serialization (matching Java types) |
| **Registration** | `presto_cpp/main/connectors/Registration.cpp` | Static registration at startup |

### Plugin Architecture (Dynamic Loading)

After converting to a plugin, the CLP connector loads dynamically at runtime:

```
┌────────────────────────────────────────────────────────────────────────────┐
│                           presto_server binary                              │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                       Plugin Loader                                   │  │
│  │  1. Read plugin.dir from config.properties                           │  │
│  │  2. Scan directory for .so files                                     │  │
│  │  3. dlopen() each plugin                                             │  │
│  │  4. dlsym("registerExtensions")                                      │  │
│  │  5. Call registerExtensions()                                        │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                              │                                              │
│                              │ dlopen()                                     │
└──────────────────────────────┼──────────────────────────────────────────────┘
                               │
      ┌────────────────────────┴────────────────────────────────┐
      │                                                         │
      │  ┌───────────────────────────────────────────────────┐  │
      │  │              libclp_connector.so                   │  │
      │  │                                                    │  │
      │  │  extern "C" void registerExtensions() {           │  │
      │  │    // Register Velox connector factory            │  │
      │  │    registerConnectorFactory(ClpConnectorFactory)  │  │
      │  │    // Register protocol bridge                    │  │
      │  │    registerPrestoToVeloxConnector(                │  │
      │  │        ClpPrestoToVeloxConnector)                 │  │
      │  │  }                                                │  │
      │  │                                                    │  │
      │  │  ┌──────────────────────────────────────────────┐ │  │
      │  │  │      ClpPrestoToVeloxConnector               │ │  │
      │  │  │  - toVeloxSplit()                            │ │  │
      │  │  │  - toVeloxColumnHandle()                     │ │  │
      │  │  │  - toVeloxTableHandle()                      │ │  │
      │  │  │  - createConnectorProtocol()                 │ │  │
      │  │  └──────────────────────────────────────────────┘ │  │
      │  │                                                    │  │
      │  │  ┌──────────────────────────────────────────────┐ │  │
      │  │  │      Velox CLP Connector (from velox/)       │ │  │
      │  │  │  ClpConnector, ClpDataSource, ClpConfig      │ │  │
      │  │  │  ClpTableHandle, ClpColumnHandle             │ │  │
      │  │  │  search_lib (Archive/IR cursors)             │ │  │
      │  │  └──────────────────────────────────────────────┘ │  │
      │  │                                                    │  │
      │  │  ┌──────────────────────────────────────────────┐ │  │
      │  │  │      CLP Protocol Serialization              │ │  │
      │  │  │  presto_protocol_clp.h/cpp                   │ │  │
      │  │  │  ClpConnectorProtocol                        │ │  │
      │  │  └──────────────────────────────────────────────┘ │  │
      │  │                                                    │  │
      │  │  ┌──────────────────────────────────────────────┐ │  │
      │  │  │       clp-s (statically linked)              │ │  │
      │  │  └──────────────────────────────────────────────┘ │  │
      │  └───────────────────────────────────────────────────┘  │
      │                    CLP Plugin                           │
      └─────────────────────────────────────────────────────────┘
```

### Data Flow

This diagram shows how data flows from the Presto Coordinator to the Velox CLP Connector:

```
┌─────────────────────────────────────────────────────────────────┐
│              Presto Coordinator (Java)                          │
│                                                                 │
│  User Query: SELECT * FROM clp.default.logs WHERE level='ERROR' │
│                                                                 │
│  Presto CLP Connector (Java) generates:                         │
│  - ClpSplit objects (paths to CLP archives)                     │
│  - ClpColumnHandle objects (columns to project)                 │
│  - ClpTableLayoutHandle (table + KQL filter)                    │
│                                                                 │
│  Serializes to JSON and sends to workers                        │
└───────────────────────────┬─────────────────────────────────────┘
                            │ HTTP (JSON payload)
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              Presto Worker (C++) - presto_server                │
│                                                                 │
│  1. Plugin Loader loads libclp_connector.so                     │
│     - dlopen("libclp_connector.so")                             │
│     - dlsym("registerExtensions")                               │
│     - registerExtensions() called                               │
│                                                                 │
│  2. JSON deserialized via ClpConnectorProtocol                  │
│     - JSON → protocol::clp::ClpSplit                            │
│     - JSON → protocol::clp::ClpColumnHandle                     │
│     - JSON → protocol::clp::ClpTableLayoutHandle                │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              ClpPrestoToVeloxConnector (Bridge)                 │
│                                                                 │
│  Converts protocol types to Velox types:                        │
│  - protocol::clp::ClpSplit → velox::connector::clp::ClpConnectorSplit
│  - protocol::clp::ClpColumnHandle → velox::connector::clp::ClpColumnHandle
│  - protocol::clp::ClpTableLayoutHandle → velox::connector::clp::ClpTableHandle
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              Velox CLP Connector                                │
│                                                                 │
│  ClpConnector::createDataSource()                               │
│    └── Creates ClpDataSource with output schema                 │
│                                                                 │
│  ClpDataSource::addSplit()                                      │
│    └── Creates cursor based on split type:                      │
│        - SplitType::kArchive → ClpArchiveCursor                │
│        - SplitType::kIr → ClpIrCursor                          │
│                                                                 │
│  ClpDataSource::next()                                          │
│    └── cursor->fetchNext() → Row data                          │
│    └── cursor->createVector() → RowVector                      │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              clp-s Library                                      │
│                                                                 │
│  - Reads CLP archive/IR files from filesystem or S3             │
│  - Decompresses log data                                        │
│  - Executes KQL filters (e.g., "level:ERROR")                   │
│  - Returns matching rows                                        │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
              RowVector → Velox Execution Engine → Results

```

---

## What PR #26650 Provides

PR #26650 implements the plugin infrastructure defined in RFC-0019. After it's merged, the following capabilities are available:

### Plugin Loading Infrastructure

```cpp
// In PrestoServer.cpp (provided by PR #26650)
void loadPlugins() {
  auto pluginDir = systemConfig_->pluginDir();
  if (!pluginDir.empty()) {
    for (const auto& entry : fs::directory_iterator(pluginDir)) {
      if (entry.path().extension() == ".so" ||
          entry.path().extension() == ".dylib") {
        loadPlugin(entry.path().string());
      }
    }
  }
}

void loadPlugin(const std::string& path) {
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  auto registerFn = reinterpret_cast<void(*)()>(
      dlsym(handle, "registerExtensions"));
  registerFn();
}
```

### Registration Functions

```cpp
// Available for plugins to use (provided by PR #26650)
void registerPrestoToVeloxConnector(
    std::unique_ptr<const PrestoToVeloxConnector> connector);

// From Velox (already available)
void velox::connector::registerConnectorFactory(
    std::shared_ptr<ConnectorFactory> factory);
```

### Configuration Property

```properties
# config.properties
plugin.dir=/opt/presto/plugins
```

---

## Implementation Plan

### Step 1: Create Plugin Directory Structure **[PRESTO]**

> **Repository:** `prestodb/presto` - Submit for upstream review

Create the following directory structure:

```
presto-native-execution/
└── presto_cpp/
    └── main/
        └── connectors/
            └── clp_plugin/
                ├── CMakeLists.txt
                ├── ClpPluginEntry.cpp
                ├── ClpPrestoToVeloxConnector.h
                └── ClpPrestoToVeloxConnector.cpp
```

**Note**: The plugin will reference the existing Velox CLP connector sources and protocol sources from their current locations. We don't need to copy them.

### Step 2: Extract ClpPrestoToVeloxConnector **[PRESTO]**

> **Repository:** `prestodb/presto` - Submit for upstream review
>
> **Note:** This code lives in `presto_cpp/` and references Velox CLP connector headers via includes.

Extract the `ClpPrestoToVeloxConnector` class from `PrestoToVeloxConnector.cpp` into standalone files.

#### ClpPrestoToVeloxConnector.h

```cpp
// presto_cpp/main/connectors/clp_plugin/ClpPrestoToVeloxConnector.h

#pragma once

#include "presto_cpp/main/connectors/PrestoToVeloxConnector.h"

namespace facebook::presto {

/// Bridge class that converts Presto CLP protocol types to Velox CLP connector types.
/// This enables communication between the Java-side Presto CLP Connector and
/// the C++-side Velox CLP Connector.
class ClpPrestoToVeloxConnector final : public PrestoToVeloxConnector {
 public:
  explicit ClpPrestoToVeloxConnector(std::string connectorName)
      : PrestoToVeloxConnector(std::move(connectorName)) {}

  /// Converts protocol::clp::ClpSplit to velox::connector::clp::ClpConnectorSplit
  std::unique_ptr<velox::connector::ConnectorSplit> toVeloxSplit(
      const protocol::ConnectorId& catalogId,
      const protocol::ConnectorSplit* connectorSplit,
      const protocol::SplitContext* splitContext) const override;

  /// Converts protocol::clp::ClpColumnHandle to velox::connector::clp::ClpColumnHandle
  std::unique_ptr<velox::connector::ColumnHandle> toVeloxColumnHandle(
      const protocol::ColumnHandle* column,
      const TypeParser& typeParser) const override;

  /// Converts protocol::clp::ClpTableLayoutHandle to velox::connector::clp::ClpTableHandle
  std::unique_ptr<velox::connector::ConnectorTableHandle> toVeloxTableHandle(
      const protocol::TableHandle& tableHandle,
      const VeloxExprConverter& exprConverter,
      const TypeParser& typeParser,
      std::unordered_map<
          std::string,
          std::shared_ptr<velox::connector::ColumnHandle>>& assignments)
      const override;

  /// Creates the ClpConnectorProtocol for JSON serialization/deserialization
  std::unique_ptr<protocol::ConnectorProtocol> createConnectorProtocol()
      const override;
};

} // namespace facebook::presto
```

#### ClpPrestoToVeloxConnector.cpp

```cpp
// presto_cpp/main/connectors/clp_plugin/ClpPrestoToVeloxConnector.cpp

#include "ClpPrestoToVeloxConnector.h"

#include "presto_cpp/presto_protocol/connector/clp/ClpConnectorProtocol.h"
#include "presto_cpp/presto_protocol/connector/clp/presto_protocol_clp.h"
#include "velox/connectors/clp/ClpColumnHandle.h"
#include "velox/connectors/clp/ClpConnector.h"
#include "velox/connectors/clp/ClpConnectorSplit.h"
#include "velox/connectors/clp/ClpTableHandle.h"

namespace facebook::presto {

std::unique_ptr<velox::connector::ConnectorSplit>
ClpPrestoToVeloxConnector::toVeloxSplit(
    const protocol::ConnectorId& catalogId,
    const protocol::ConnectorSplit* connectorSplit,
    const protocol::SplitContext* /*splitContext*/) const {
  auto clpSplit = dynamic_cast<const protocol::clp::ClpSplit*>(connectorSplit);
  VELOX_CHECK_NOT_NULL(
      clpSplit, "Unexpected split type {}", connectorSplit->_type);

  velox::connector::clp::ClpConnectorSplit::SplitType type;
  switch (clpSplit->type) {
    case protocol::clp::SplitType::ARCHIVE:
      type = velox::connector::clp::ClpConnectorSplit::SplitType::kArchive;
      break;
    case protocol::clp::SplitType::IR:
      type = velox::connector::clp::ClpConnectorSplit::SplitType::kIr;
      break;
    default:
      VELOX_FAIL("Unknown CLP split type");
  }

  std::optional<std::string> kqlQuery;
  if (clpSplit->kqlQuery) {
    kqlQuery = *clpSplit->kqlQuery;
  }

  return std::make_unique<velox::connector::clp::ClpConnectorSplit>(
      catalogId,
      clpSplit->path,
      static_cast<int>(type),
      kqlQuery);
}

std::unique_ptr<velox::connector::ColumnHandle>
ClpPrestoToVeloxConnector::toVeloxColumnHandle(
    const protocol::ColumnHandle* column,
    const TypeParser& typeParser) const {
  auto clpColumn =
      dynamic_cast<const protocol::clp::ClpColumnHandle*>(column);
  VELOX_CHECK_NOT_NULL(
      clpColumn, "Unexpected column handle type {}", column->_type);
  return std::make_unique<velox::connector::clp::ClpColumnHandle>(
      clpColumn->columnName,
      clpColumn->originalColumnName,
      stringToType(clpColumn->columnType, typeParser));
}

std::unique_ptr<velox::connector::ConnectorTableHandle>
ClpPrestoToVeloxConnector::toVeloxTableHandle(
    const protocol::TableHandle& tableHandle,
    const VeloxExprConverter& /*exprConverter*/,
    const TypeParser& /*typeParser*/,
    std::unordered_map<
        std::string,
        std::shared_ptr<velox::connector::ColumnHandle>>& /*assignments*/)
    const {
  auto clpLayout =
      std::dynamic_pointer_cast<const protocol::clp::ClpTableLayoutHandle>(
          tableHandle.connectorTableLayout);
  VELOX_CHECK_NOT_NULL(clpLayout, "Null Clp table layout handle!");
  return std::make_unique<velox::connector::clp::ClpTableHandle>(
      tableHandle.connectorId,
      clpLayout->table.schemaTableName.tableName);
}

std::unique_ptr<protocol::ConnectorProtocol>
ClpPrestoToVeloxConnector::createConnectorProtocol() const {
  return std::make_unique<protocol::clp::ClpConnectorProtocol>();
}

} // namespace facebook::presto
```

### Step 3: Create Plugin Entry Point **[PRESTO]**

> **Repository:** `prestodb/presto` - Submit for upstream review

#### ClpPluginEntry.cpp

```cpp
// presto_cpp/main/connectors/clp_plugin/ClpPluginEntry.cpp

#include "ClpPrestoToVeloxConnector.h"
#include "presto_cpp/main/connectors/PrestoToVeloxConnector.h"
#include "velox/connectors/clp/ClpConnector.h"

extern "C" {

/// Plugin entry point called by Presto worker after dlopen().
/// This function registers:
/// 1. The Velox CLP Connector factory (ClpConnectorFactory)
/// 2. The protocol bridge (ClpPrestoToVeloxConnector)
void registerExtensions() {
  using namespace facebook::presto;
  using namespace facebook::velox::connector;

  const std::string connectorName =
      clp::ClpConnectorFactory::kClpConnectorName;

  // Register Velox connector factory (if not already registered)
  if (!hasConnectorFactory(connectorName)) {
    registerConnectorFactory(std::make_shared<clp::ClpConnectorFactory>());
  }

  // Register Presto-to-Velox protocol bridge
  registerPrestoToVeloxConnector(
      std::make_unique<ClpPrestoToVeloxConnector>(connectorName));
}

} // extern "C"
```

### Step 4: Create Plugin CMakeLists.txt **[PRESTO]**

> **Repository:** `prestodb/presto` - Submit for upstream review
>
> **Note:** This CMake file references **[VELOX]** targets (`velox_clp_connector`, `clp-s-*`) that must exist in your Velox submodule.

**IMPORTANT**: `velox_clp_connector` is an OBJECT library, not a regular library. This means we need to use `$<TARGET_OBJECTS:>` to link its object files into our shared library.

Similarly, `presto_protocol` is an OBJECT library, and the CLP protocol sources are included via `#include` in `presto_protocol.cpp`. Since we're creating a standalone plugin, we need to include the protocol source directly.

```cmake
# presto_cpp/main/connectors/clp_plugin/CMakeLists.txt

cmake_minimum_required(VERSION 3.20)

# =============================================================================
# CLP Connector Plugin - Shared Library
# =============================================================================

add_library(clp_connector_plugin SHARED
    ClpPluginEntry.cpp
    ClpPrestoToVeloxConnector.cpp
)

# Include CLP protocol source directly
# (presto_protocol is an OBJECT library and includes connector protocols via #include)
target_sources(clp_connector_plugin
    PRIVATE
        ${CMAKE_SOURCE_DIR}/presto_cpp/presto_protocol/connector/clp/presto_protocol_clp.cpp
)

# -----------------------------------------------------------------------------
# Include Directories
# -----------------------------------------------------------------------------
target_include_directories(clp_connector_plugin
    PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/velox
        ${CMAKE_SOURCE_DIR}/presto_cpp
)

# -----------------------------------------------------------------------------
# Link Dependencies
# -----------------------------------------------------------------------------

# IMPORTANT: velox_clp_connector is an OBJECT library
# We must use $<TARGET_OBJECTS:> to include its compiled object files
target_link_libraries(clp_connector_plugin
    PRIVATE
        # Velox CLP connector objects (OBJECT library)
        $<TARGET_OBJECTS:velox_clp_connector>

        # CLP search libraries (STATIC - bundled into plugin)
        clp-s-search
        clp-s-archive-search
        clp-s-ir-search

        # clp-s dependencies
        clp_s::archive_reader
        clp_s::clp_dependencies
        clp_s::io
        clp_s::search
        clp_s::search::ast
        clp_s::search::kql

        # Velox dependencies
        velox_connector
        velox_vector
        velox_type

        # Common dependencies
        simdjson::simdjson
        Folly::folly
        fmt::fmt
        glog::glog
)

# -----------------------------------------------------------------------------
# Linker Options
# -----------------------------------------------------------------------------

# Allow undefined symbols - they will be resolved at runtime by presto_server
# This is necessary because we depend on presto_server symbols like
# registerPrestoToVeloxConnector() which are not linked into the plugin
if(APPLE)
    target_link_options(clp_connector_plugin
        PRIVATE "-Wl,-undefined,dynamic_lookup")
else()
    target_link_options(clp_connector_plugin
        PRIVATE "-Wl,--allow-shlib-undefined")
endif()

# -----------------------------------------------------------------------------
# Compile Features and Properties
# -----------------------------------------------------------------------------

# C++20 required for clp-s
target_compile_features(clp_connector_plugin PRIVATE cxx_std_20)

# Position independent code (required for shared libraries)
set_target_properties(clp_connector_plugin PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "clp_connector"
    PREFIX "lib"
)

# -----------------------------------------------------------------------------
# Installation
# -----------------------------------------------------------------------------

install(TARGETS clp_connector_plugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/presto/plugins
)
```

### Step 5: Remove CLP from Static Build **[PRESTO]**

> **Repository:** `prestodb/presto` - All sub-steps in this section are Presto changes for upstream review.

#### 5.1 Modify Registration.cpp **[PRESTO]**

Remove CLP-related includes and registration:

```cpp
// presto_cpp/main/connectors/Registration.cpp

// REMOVE this include:
// #include "velox/connectors/clp/ClpConnector.h"

void registerConnectorFactories() {
  // ... keep other connectors ...

  // REMOVE these lines (CLP now loaded as plugin):
  // if (!velox::connector::hasConnectorFactory(
  //         velox::connector::clp::ClpConnectorFactory::kClpConnectorName)) {
  //   velox::connector::registerConnectorFactory(
  //       std::make_shared<velox::connector::clp::ClpConnectorFactory>());
  // }
}

void registerConnectors() {
  registerConnectorFactories();
  // ... keep other connectors ...

  // REMOVE these lines (CLP now loaded as plugin):
  // registerPrestoToVeloxConnector(
  //     std::make_unique<ClpPrestoToVeloxConnector>(
  //         velox::connector::clp::ClpConnectorFactory::kClpConnectorName));
}
```

#### 5.2 Modify PrestoToVeloxConnector.h **[PRESTO]**

Remove the `ClpPrestoToVeloxConnector` class declaration (lines 227-252 in current file).

#### 5.3 Modify PrestoToVeloxConnector.cpp **[PRESTO]**

Remove the `ClpPrestoToVeloxConnector` implementation.

#### 5.4 Modify presto_cpp/main/connectors/CMakeLists.txt **[PRESTO]**

```cmake
# BEFORE:
target_link_libraries(presto_connectors presto_velox_expr_conversion
                      velox_clp_connector velox_type_fbhive)

# AFTER (remove velox_clp_connector):
target_link_libraries(presto_connectors presto_velox_expr_conversion
                      velox_type_fbhive)

# Add plugin subdirectory at the end:
add_subdirectory(clp_plugin)
```

#### 5.5 Modify presto_cpp/main/CMakeLists.txt **[PRESTO]**

```cmake
# Remove velox_clp_connector from presto_server_lib dependencies:
target_link_libraries(
  presto_server_lib
  # ... other libraries ...
  # velox_clp_connector  # REMOVE this line
  # ...
)
```

#### 5.6 Modify presto_cpp/presto_protocol/presto_protocol.cpp **[PRESTO]**

```cpp
// REMOVE this include (CLP protocol now built into plugin):
// #include "presto_cpp/presto_protocol/connector/clp/presto_protocol_clp.cpp"
```

---

## File Reference

### Files to Create **[PRESTO]**

> **All files below go to `prestodb/presto` for upstream review.**

| File | Purpose | Repository |
|------|---------|------------|
| `presto_cpp/main/connectors/clp_plugin/CMakeLists.txt` | Plugin build configuration | **[PRESTO]** |
| `presto_cpp/main/connectors/clp_plugin/ClpPluginEntry.cpp` | `extern "C" registerExtensions()` entry point | **[PRESTO]** |
| `presto_cpp/main/connectors/clp_plugin/ClpPrestoToVeloxConnector.h` | Bridge class header | **[PRESTO]** |
| `presto_cpp/main/connectors/clp_plugin/ClpPrestoToVeloxConnector.cpp` | Bridge class implementation | **[PRESTO]** |

### Files to Modify **[PRESTO]**

> **All files below go to `prestodb/presto` for upstream review.**

| File | Change | Repository |
|------|--------|------------|
| `presto_cpp/main/connectors/Registration.cpp` | Remove CLP registration and includes | **[PRESTO]** |
| `presto_cpp/main/connectors/PrestoToVeloxConnector.h` | Remove ClpPrestoToVeloxConnector class | **[PRESTO]** |
| `presto_cpp/main/connectors/PrestoToVeloxConnector.cpp` | Remove ClpPrestoToVeloxConnector implementation | **[PRESTO]** |
| `presto_cpp/main/connectors/CMakeLists.txt` | Remove CLP links, add plugin subdirectory | **[PRESTO]** |
| `presto_cpp/main/CMakeLists.txt` | Remove velox_clp_connector link | **[PRESTO]** |
| `presto_cpp/presto_protocol/presto_protocol.cpp` | Remove CLP protocol include | **[PRESTO]** |

### Files Referenced by Plugin (No Changes Needed)

> **These files are referenced/included by the plugin but do not need modification.**

| File | Purpose | Repository |
|------|---------|------------|
| `velox/velox/connectors/clp/*` | Velox CLP connector implementation | **[VELOX]** - Private repo |
| `velox/velox/connectors/clp/search_lib/*` | Archive/IR cursor implementations | **[VELOX]** - Private repo |
| `presto_cpp/presto_protocol/connector/clp/*` | Protocol serialization types | **[PRESTO]** - Existing code |
| `presto_cpp/main/connectors/PrestoToVeloxConnector.h` | Base class (symbols resolved at runtime) | **[PRESTO]** - Existing code |

---

## Build Instructions

### Build Plugin Only

```bash
cd presto-native-execution
mkdir -p _build/release
cd _build/release

cmake ../.. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPRESTO_ENABLE_PARQUET=ON \
    -DVELOX_ENABLE_HDFS=ON

# Build only the CLP plugin
make -j$(nproc) clp_connector_plugin
```

### Build Full Presto + Plugin

```bash
cd presto-native-execution
mkdir -p _build/release
cd _build/release

cmake ../.. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPRESTO_ENABLE_PARQUET=ON \
    -DVELOX_ENABLE_HDFS=ON

make -j$(nproc) presto_server
make -j$(nproc) clp_connector_plugin
```

### Verify Build Output

```bash
# Check plugin exists
ls -la _build/release/presto_cpp/main/connectors/clp_plugin/libclp_connector.so

# Verify entry point symbol (must show "T registerExtensions")
nm -D _build/release/presto_cpp/main/connectors/clp_plugin/libclp_connector.so | grep registerExtensions
# Expected output: T registerExtensions

# Check dependencies
ldd _build/release/presto_cpp/main/connectors/clp_plugin/libclp_connector.so
# clp-s should NOT appear in output (it's statically linked into the plugin)
# Velox/Presto symbols show as "not found" (resolved at runtime by presto_server)
```

---

## Deployment

### 1. Create Plugin Directory

```bash
mkdir -p /opt/presto/plugins
```

### 2. Copy Plugin

```bash
cp _build/release/presto_cpp/main/connectors/clp_plugin/libclp_connector.so \
   /opt/presto/plugins/
```

### 3. Configure Presto Worker

Add to `config.properties`:

```properties
plugin.dir=/opt/presto/plugins
```

### 4. Configure CLP Catalog

Create `clp.properties` (same as before - no changes needed):

```properties
connector.name=clp
clp.storage-type=FS
# clp.s3-auth-provider=CLP_PACKAGE  # For S3 storage
```

### 5. Start Worker

```bash
./presto_server --etc-dir=/etc/presto
```

---

## Verification

### Check Plugin Loaded

Look for log messages during startup:

```
INFO: Loading plugin from /opt/presto/plugins/libclp_connector.so
INFO: Registered connector: clp
```

### Test Query

```sql
-- Connect to Presto
presto-cli --server localhost:8080

-- Use CLP catalog
USE clp.default;

-- List tables
SHOW TABLES;

-- Query data
SELECT * FROM logs LIMIT 10;

-- Test KQL filter pushdown
SELECT * FROM logs WHERE level = 'ERROR' LIMIT 10;
```

### Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| Plugin not loaded | Wrong path | Check `plugin.dir` configuration |
| Symbol not found | ABI mismatch | Rebuild plugin with same compiler as presto_server |
| `registerExtensions` not found | Missing extern "C" | Verify function has `extern "C"` linkage |
| clp-s errors | Missing static link | Verify clp-s is statically linked (check `ldd` output) |
| Type conversion errors | Protocol mismatch | Ensure Java and C++ protocol types match |

---

## Summary

Converting the Velox CLP Connector to a plugin involves:

| Step | Description | Repository |
|------|-------------|------------|
| 1 | **Create** plugin directory with entry point and CMakeLists | **[PRESTO]** |
| 2 | **Extract** `ClpPrestoToVeloxConnector` bridge class to standalone files | **[PRESTO]** |
| 3 | **Build** shared library with static clp-s linking and OBJECT library handling | **[PRESTO]** (references **[VELOX]**) |
| 4 | **Remove** CLP from static registration and linking in presto_server | **[PRESTO]** |
| 5 | **Deploy** `.so` file to plugin directory | Deployment |
| 6 | **Configure** `plugin.dir` in Presto worker config | Deployment |

### Repository Breakdown

**[PRESTO] Changes (Submit to `prestodb/presto` for review):**
- All new files in `presto_cpp/main/connectors/clp_plugin/`
- Modifications to `Registration.cpp`, `PrestoToVeloxConnector.h/cpp`, `CMakeLists.txt` files
- Modifications to `presto_protocol.cpp`

**[VELOX] Dependencies (Private repo - no upstream changes needed):**
- `velox/velox/connectors/clp/` - Velox CLP connector (compiled into plugin)
- `velox/velox/connectors/clp/search_lib/` - CLP search library (compiled into plugin)
- clp-s libraries (statically linked into plugin)

### Plugin Architecture

The plugin is self-contained:
- clp-s is statically linked into the plugin
- Velox CLP connector code is compiled into the plugin
- Protocol serialization code is compiled into the plugin
- Velox/Presto symbols (e.g., `registerPrestoToVeloxConnector()`) are resolved at runtime when loaded by `presto_server`
