# CLP Connector Plugin Architecture

## Table of Contents

1. [Overview](#overview)
2. [Terminology](#terminology)
3. [Architecture](#architecture)
   - [Current Architecture (Static Linking)](#current-architecture-static-linking)
     - [Why This Is Problematic](#why-this-is-problematic)
   - [Proposed Architecture (Plugin Loading)](#proposed-architecture-plugin-loading)
   - [Data Flow](#data-flow)
   - [Component Details](#component-details)
4. [Implementation Plan](#implementation-plan)
   - [File Reference Summary](#file-reference-summary)
   - [Part 1: Plugin Loading Infrastructure (Upstream to Presto)](#part-1-plugin-loading-infrastructure-upstream-to-presto)
   - [Part 2: CLP Connector Plugin (Private Distribution)](#part-2-clp-connector-plugin-private-distribution)
5. [Build Instructions](#build-instructions)
6. [Deployment](#deployment)
7. [Verification](#verification)

---

## Overview

This document describes how to distribute the CLP connector as a **standalone plugin library** that users can load into stock Presto without building Velox from scratch.

### Presto Architecture Background

Presto is a distributed SQL query engine with two main components:

| Component | Language | Role |
|-----------|----------|------|
| **Presto Coordinator** | Java | Receives SQL queries, parses them, creates query plans, and distributes work to workers. Runs as a Java process. |
| **Presto Worker** | Java or C++ | Executes the actual data processing. Can be either Java-based or native (C++) using Velox. |

For native execution, the worker is called `presto_server`:

| Component | Description |
|-----------|-------------|
| **`presto_server`** | The C++ native worker binary built from `presto-native-execution`. Uses Velox as the execution engine. Communicates with the Java Coordinator via HTTP/JSON. |
| **Velox** | Meta's open-source C++ execution engine. Provides the runtime for query execution, including connectors for reading data. |

**Communication flow:**
```
User SQL Query → Presto Coordinator (Java) → presto_server (C++/Velox) → Data Source
```

### Goal

| Current State | Desired State |
|---------------|---------------|
| Users must pull private Velox fork | Users build stock Presto from upstream |
| Users build entire Velox + Presto | YScope distributes `libclp_connector.so` |
| CLP code embedded in `presto_server` | Plugin loaded at runtime via config |

### Problem Summary

Currently, the CLP connector is compiled directly into `presto_server` using CMake OBJECT libraries. This means:
- The CLP source code must be present at build time
- Users must clone the private Velox fork and build everything from scratch
- **You cannot simply distribute a pre-built `presto_server` binary** because users may need different Presto configurations, versions, or custom modifications

See [Current Architecture](#current-architecture-static-linking) for detailed explanation.

### Current State of Presto Plugin Support

> **Important:** As of this writing, Presto does NOT have plugin loading infrastructure for native (C++) connectors.

| Component | Status | Details |
|-----------|--------|---------|
| **RFC-0019** | Design Only | [RFC-0019](https://github.com/prestodb/rfcs/blob/main/RFC-0019-connector-plugins.md) proposes connector plugins but is NOT implemented |
| **PR #26650** | Merged | Adds **binary serialization codecs** for TPCH, NOT plugin loading |
| **PR #26257** | Merged | Adds custom connector-provided serialization, NOT plugin loading |
| **Plugin loading (`dlopen`)** | **NOT IMPLEMENTED** | Must be implemented as Part 1 of this plan |
| **`plugin.dir` config** | **NOT IMPLEMENTED** | Must be implemented as Part 1 of this plan |

### Approach

The implementation has two parts:

| Part | Repository | Purpose | Status |
|------|------------|---------|--------|
| **Part 1** | Upstream Presto (`prestodb/presto`) | Implement plugin loading infrastructure (`dlopen` + `plugin.dir` config) | **Must be implemented and upstreamed** |
| **Part 2** | Private (YScope) | Build CLP connector as `.so` plugin | After Part 1 is merged |

Once Part 1 is merged upstream, any Presto user can:
1. Build `presto_server` from stock Presto
2. Download `libclp_connector.so` from YScope
3. Configure `plugin.dir` and run

---

## Terminology

| Term | Definition |
|------|------------|
| **Velox CLP Connector** | C++ connector code that reads CLP archives/IR files. Located in `velox/connectors/clp/`. |
| **CLP Plugin** | The `.so` shared library containing the Velox CLP Connector + bridge code. Distributed by YScope. |
| **Plugin Loading Infrastructure** | Code in `presto_server` that scans a directory and loads `.so` files via `dlopen()`. |
| **ClpPrestoToVeloxConnector** | Bridge class converting Presto protocol types to Velox connector types. |
| **registerExtensions()** | Entry point function that plugins must export. Called by `presto_server` after `dlopen()`. |

---

## Architecture

### Current Architecture (Static Linking)

Currently, the Velox CLP Connector is statically linked into the `presto_server` binary. Users must build everything from source, including the private Velox fork.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         presto_server binary                                │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                      Registration.cpp                                 │  │
│  │  registerConnectorFactories()                                         │  │
│  │    └── ClpConnectorFactory (Velox connector factory)                  │  │
│  │  registerPrestoToVeloxConnector()                                     │  │
│  │    └── ClpPrestoToVeloxConnector (protocol bridge)                    │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                │                                            │
│                                ▼                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                velox_clp_connector (OBJECT library)                   │  │
│  │  ┌─────────────┐ ┌──────────────┐ ┌───────────────────────────────┐   │  │
│  │  │ClpConnector │ │ClpDataSource │ │        search_lib             │   │  │
│  │  │ClpConfig    │ │ClpTableHandle│ │  ArchiveCursor / IrCursor     │   │  │
│  │  └─────────────┘ └──────────────┘ └───────────────────────────────┘   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                │                                            │
│                                ▼                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                      clp-s libraries (STATIC)                         │  │
│  │    clp_s::archive_reader, clp_s::search, clp_s::search::kql, etc.     │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### Why This Is Problematic

**1. OBJECT Library Cannot Be Pre-built**

`velox_clp_connector` is defined as an **OBJECT library** in CMake:

```cmake
velox_add_library(
  velox_clp_connector
  OBJECT          # <-- This is the problem
  ClpConfig.cpp
  ClpConnector.cpp
  ClpDataSource.cpp
  ClpTableHandle.cpp)
```

An OBJECT library is **not a compiled artifact** (like `.a` or `.so`). It's just a list of source files that CMake compiles during the build. The compiled `.o` files are then linked directly into the final binary. This means:
- You cannot distribute a pre-built OBJECT library
- The source code **must be present** at build time
- CMake must compile these files every time you build `presto_server`

**2. Source Code Lives in Private Fork**

The CLP connector source code is located in the private Velox fork (`y-scope/velox`), not in upstream Presto or upstream Velox:

```
y-scope/velox (private fork)
└── velox/connectors/clp/           # CLP connector source
    ├── ClpConnector.cpp
    ├── ClpDataSource.cpp
    └── search_lib/                 # Depends on clp-s
        ├── archive/
        └── ir/
```

**3. Dependency Chain Requires Full Build**

The CLP connector depends on `clp-s` libraries, which also must be built from source:

```
presto_server
└── velox_clp_connector (OBJECT - must compile from source)
    └── clp-s-search (STATIC)
        ├── clp-s-archive-search (STATIC)
        │   └── clp_s::archive_reader, clp_s::search, etc.
        └── clp-s-ir-search (STATIC)
            └── clp_s::archive_reader, clp_s::search, etc.
```

**4. Why Not Just Distribute a Pre-built `presto_server` Binary?**

You might ask: "Why not just build `presto_server` with CLP support and distribute that binary?"

This doesn't work well because:

| Reason | Explanation |
|--------|-------------|
| **Version coupling** | Users may need a specific Presto version for compatibility with their coordinator or other systems |
| **Custom configurations** | Users may need different build flags, enabled features, or optimizations |
| **Other connectors** | Users may need to build with other custom connectors or modifications |
| **Platform differences** | Binary may not work across different Linux distributions or glibc versions |
| **Security policies** | Many organizations require building from source for security audits |
| **Updates** | Every Presto update would require rebuilding and redistributing the entire binary |

**5. What Users Must Do Today**

To use the CLP connector, users currently must:

```bash
# 1. Clone the private Velox fork (contains CLP connector + clp-s)
git clone https://github.com/y-scope/velox.git

# 2. Set up Velox build environment (install all dependencies)
#    This includes: folly, fmt, boost, protobuf, etc.
./scripts/setup-ubuntu.sh  # or equivalent for their OS

# 3. Clone Presto and point to the private Velox fork
git clone https://github.com/prestodb/presto.git
cd presto/presto-native-execution

# 4. Build everything from scratch (can take 30+ minutes)
make -j$(nproc)
```

This is a significant barrier for users who just want to query CLP data.

#### Alternative: Pre-built Static Library?

You might ask: "Why not change `velox_clp_connector` from OBJECT to STATIC library, pre-build it, and distribute the `.a` file?"

**This is technically possible**, but has significant practical challenges:

**What would be required:**

1. Change `velox_clp_connector` from OBJECT to STATIC library in CMakeLists.txt
2. Build and distribute:
   - `libvelox_clp_connector.a` (the static library)
   - All header files (ClpConnector.h, ClpDataSource.h, etc.)
   - All clp-s static libraries and headers
3. Users would modify their Presto CMakeLists.txt to link against these pre-built libraries

**Why this is problematic:**

| Challenge | Explanation |
|-----------|-------------|
| **ABI Compatibility** | C++ static libraries require exact ABI match. Must be built with same compiler version, same C++ standard, same optimization flags, same Velox version, same Folly version, etc. Any mismatch causes crashes or undefined behavior. |
| **Header Distribution** | Users need all header files to compile. CLP headers → Velox headers → Folly headers → Boost headers. This is a large dependency tree. |
| **Version Coupling** | The static library must match the exact Velox version users are building. If Presto updates its Velox submodule, the pre-built library may become incompatible. |
| **Build System Changes** | Users must modify `presto_cpp/main/CMakeLists.txt` to link the pre-built library. This is error-prone and creates maintenance burden. |
| **Multiple Artifacts** | Must distribute: `.a` files + headers + CMake find modules. More complex than a single `.so` file. |

**Comparison of approaches:**

| Aspect | Static Library (.a) | Shared Plugin (.so) |
|--------|---------------------|---------------------|
| **User build changes** | Must modify CMakeLists.txt | None - just configure `plugin.dir` |
| **ABI compatibility** | Must match Velox/Folly/compiler exactly | More tolerant - plugin boundary is well-defined |
| **Distribution** | Multiple files (.a + headers) | Single `.so` file |
| **Version coupling** | Tight - must match Velox version | Looser - plugin interface is stable |
| **Runtime flexibility** | None - linked at build time | Can swap plugins without rebuild |
| **Presto changes needed** | None | Must implement plugin loading (Part 1) |

**Bottom line:** The static library approach avoids implementing plugin loading, but shifts complexity to users (ABI matching, build system changes, header management). The plugin approach requires upfront work (implementing `dlopen` infrastructure) but provides a cleaner user experience.

### Proposed Architecture (Plugin Loading)

After implementing plugin loading, the CLP connector becomes a separate `.so` file that users download and configure.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Upstream Presto (prestodb/presto)                                          │
│                                                                             │
│  presto_server binary                                                       │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  PrestoServer.cpp                                                     │  │
│  │    └── loadPlugins()  ←── NEW: Scans plugin.dir, calls dlopen()       │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  SystemConfig.h                                                       │  │
│  │    └── pluginDir()    ←── NEW: Returns plugin.dir config value        │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Built-in connectors (Hive, TPCH, etc.) - statically linked           │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ dlopen() at runtime
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  YScope Private Distribution                                                │
│                                                                             │
│  libclp_connector.so                                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  extern "C" registerExtensions()  ←── Entry point                     │  │
│  │    ├── registerConnectorFactory(ClpConnectorFactory)                  │  │
│  │    └── registerPrestoToVeloxConnector(ClpPrestoToVeloxConnector)      │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                │                                            │
│  ┌─────────────────────────────┴─────────────────────────────────────────┐  │
│  │                    ClpPrestoToVeloxConnector                          │  │
│  │  toVeloxSplit(), toVeloxColumnHandle(), toVeloxTableHandle()          │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                │                                            │
│  ┌─────────────────────────────┴─────────────────────────────────────────┐  │
│  │                    Velox CLP Connector                                │  │
│  │  ClpConnector, ClpDataSource, ClpConfig, ClpTableHandle               │  │
│  │  search_lib (ArchiveCursor, IrCursor)                                 │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                │                                            │
│  ┌─────────────────────────────┴─────────────────────────────────────────┐  │
│  │                    clp-s (statically linked into .so)                 │  │
│  │  clp_s::archive_reader, clp_s::search, clp_s::search::kql, etc.       │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘

Benefit: Users build stock Presto, download .so from YScope, configure and run
```

### Data Flow

This diagram shows how data flows from the Presto Coordinator to the Velox CLP Connector:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Presto Coordinator (Java)                            │
│                                                                             │
│  User Query: SELECT * FROM clp.default.logs WHERE level='ERROR'             │
│                                                                             │
│  Presto CLP Connector (Java) generates:                                     │
│  - ClpSplit objects (paths to CLP archives/IR files)                        │
│  - ClpColumnHandle objects (columns to project)                             │
│  - ClpTableLayoutHandle (table metadata + KQL filter)                       │
│                                                                             │
│  Serializes to JSON and sends to workers via HTTP                           │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ HTTP (JSON payload)
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     Presto Worker (C++) - presto_server                     │
│                                                                             │
│  1. STARTUP: Plugin Loading                                                 │
│     ┌─────────────────────────────────────────────────────────────────┐     │
│     │  loadPlugins()                                                  │     │
│     │    ├── Read plugin.dir from config.properties                   │     │
│     │    ├── Scan directory for .so files                             │     │
│     │    ├── dlopen("libclp_connector.so")                            │     │
│     │    ├── dlsym("registerExtensions")                              │     │
│     │    └── Call registerExtensions() which registers:               │     │
│     │        ├── ClpConnectorFactory → Velox connector registry       │     │
│     │        └── ClpPrestoToVeloxConnector → Presto bridge registry   │     │
│     └─────────────────────────────────────────────────────────────────┘     │
│                                                                             │
│  2. QUERY: JSON Deserialization                                             │
│     ┌─────────────────────────────────────────────────────────────────┐     │
│     │  ClpConnectorProtocol deserializes JSON:                        │     │
│     │    ├── JSON → protocol::clp::ClpSplit                           │     │
│     │    ├── JSON → protocol::clp::ClpColumnHandle                    │     │
│     │    └── JSON → protocol::clp::ClpTableLayoutHandle               │     │
│     └─────────────────────────────────────────────────────────────────┘     │
│                                    │                                        │
│                                    ▼                                        │
│  3. CONVERSION: Protocol to Velox Types                                     │
│     ┌─────────────────────────────────────────────────────────────────┐     │
│     │  ClpPrestoToVeloxConnector converts:                            │     │
│     │    ├── protocol::clp::ClpSplit                                  │     │
│     │    │   → velox::connector::clp::ClpConnectorSplit               │     │
│     │    ├── protocol::clp::ClpColumnHandle                           │     │
│     │    │   → velox::connector::clp::ClpColumnHandle                 │     │
│     │    └── protocol::clp::ClpTableLayoutHandle                      │     │
│     │        → velox::connector::clp::ClpTableHandle                  │     │
│     └─────────────────────────────────────────────────────────────────┘     │
│                                    │                                        │
│                                    ▼                                        │
│  4. EXECUTION: Velox CLP Connector                                          │
│     ┌─────────────────────────────────────────────────────────────────┐     │
│     │  ClpConnector::createDataSource(outputType, tableHandle)        │     │
│     │    └── Creates ClpDataSource with output schema                 │     │
│     │                                                                 │     │
│     │  ClpDataSource::addSplit(split)                                 │     │
│     │    └── Creates cursor based on split type:                      │     │
│     │        ├── SplitType::kArchive → ClpArchiveCursor               │     │
│     │        └── SplitType::kIr → ClpIrCursor                         │     │
│     │                                                                 │     │
│     │  ClpDataSource::next(size, output)                              │     │
│     │    ├── cursor->fetchNext() → Fetch row data from clp-s          │     │
│     │    └── cursor->createVector() → Create Velox RowVector          │     │
│     └─────────────────────────────────────────────────────────────────┘     │
│                                    │                                        │
│                                    ▼                                        │
│  5. DATA ACCESS: clp-s Library                                              │
│     ┌─────────────────────────────────────────────────────────────────┐     │
│     │  clp-s (statically linked into plugin)                          │     │
│     │    ├── Reads CLP archive/IR files from filesystem or S3         │     │
│     │    ├── Decompresses log data                                    │     │
│     │    ├── Executes KQL filters (e.g., "level:ERROR")               │     │
│     │    └── Returns matching rows to cursor                          │     │
│     └─────────────────────────────────────────────────────────────────┘     │
│                                    │                                        │
│                                    ▼                                        │
│                    RowVector → Velox Execution Engine → Results             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Component Details

#### Velox CLP Connector Classes

| Class | File | Purpose |
|-------|------|---------|
| `ClpConnector` | `ClpConnector.cpp` | Main connector class. Implements `velox::connector::Connector`. Creates `ClpDataSource` instances. |
| `ClpConnectorFactory` | `ClpConnector.cpp` | Factory that creates `ClpConnector` instances. Registered with Velox connector registry. |
| `ClpDataSource` | `ClpDataSource.cpp` | Reads data from CLP splits. Implements `velox::connector::DataSource`. Orchestrates cursor creation and row fetching. |
| `ClpConnectorSplit` | `ClpConnectorSplit.h` | Describes a unit of work (archive file or IR stream). Contains path, split type, and optional KQL query. |
| `ClpTableHandle` | `ClpTableHandle.h` | Represents the CLP table being queried. Contains connector ID and table name. |
| `ClpColumnHandle` | `ClpColumnHandle.h` | Represents a column in the CLP table. Contains column name, original name, and Velox type. |
| `ClpConfig` | `ClpConfig.cpp` | Configuration for the CLP connector (storage type, S3 settings, etc.). |

#### Search Library Classes

| Class | File | Purpose |
|-------|------|---------|
| `BaseClpCursor` | `search_lib/BaseClpCursor.h` | Abstract base class for cursors. Defines `fetchNext()` and `createVector()` interface. |
| `ClpArchiveCursor` | `search_lib/archive/ClpArchiveCursor.cpp` | Reads data from CLP archive format using `clp_s::ArchiveReader`. |
| `ClpIrCursor` | `search_lib/ir/ClpIrCursor.cpp` | Reads data from CLP IR (Intermediate Representation) format. |
| `ClpArchiveVectorLoader` | `search_lib/archive/ClpArchiveVectorLoader.cpp` | Creates Velox vectors from archive data. |
| `ClpIrVectorLoader` | `search_lib/ir/ClpIrVectorLoader.cpp` | Creates Velox vectors from IR data. |

#### Protocol Bridge Classes

| Class | File | Purpose |
|-------|------|---------|
| `ClpPrestoToVeloxConnector` | `clp_plugin/ClpPrestoToVeloxConnector.cpp` | Converts Presto protocol types to Velox connector types. |
| `ClpConnectorProtocol` | `presto_protocol/connector/clp/ClpConnectorProtocol.h` | JSON serialization/deserialization for CLP-specific types. |
| `protocol::clp::ClpSplit` | `presto_protocol/connector/clp/presto_protocol_clp.h` | C++ protocol type matching Java `ClpSplit`. |
| `protocol::clp::ClpColumnHandle` | `presto_protocol/connector/clp/presto_protocol_clp.h` | C++ protocol type matching Java `ClpColumnHandle`. |
| `protocol::clp::ClpTableLayoutHandle` | `presto_protocol/connector/clp/presto_protocol_clp.h` | C++ protocol type matching Java `ClpTableLayoutHandle`. |

---

## Implementation Plan

### File Reference Summary

#### Part 1: Files to Modify in Upstream Presto

| File | Action | Description |
|------|--------|-------------|
| `presto_cpp/main/common/Configs.h` | Modify | Add `kPluginDir` constant and `pluginDir()` declaration |
| `presto_cpp/main/common/Configs.cpp` | Modify | Add `pluginDir()` implementation |
| `presto_cpp/main/PrestoServer.h` | Modify | Add `loadPlugins()` and `loadPlugin()` declarations |
| `presto_cpp/main/PrestoServer.cpp` | Modify | Add plugin loading implementation, call `loadPlugins()` in `run()` |
| `presto_cpp/main/CMakeLists.txt` | Modify | Add `${CMAKE_DL_LIBS}` for dlopen/dlsym |

#### Part 2: Files for CLP Plugin (Private)

| File | Action | Description |
|------|--------|-------------|
| `presto_cpp/main/connectors/clp_plugin/CMakeLists.txt` | Create | Plugin build configuration |
| `presto_cpp/main/connectors/clp_plugin/ClpPluginEntry.cpp` | Create | `extern "C" registerExtensions()` entry point |
| `presto_cpp/main/connectors/clp_plugin/ClpPrestoToVeloxConnector.h` | Create | Bridge class header |
| `presto_cpp/main/connectors/clp_plugin/ClpPrestoToVeloxConnector.cpp` | Create | Bridge class implementation |
| `presto_cpp/main/connectors/Registration.cpp` | Modify | Remove CLP registration |
| `presto_cpp/main/connectors/PrestoToVeloxConnector.h` | Modify | Remove ClpPrestoToVeloxConnector class |
| `presto_cpp/main/connectors/PrestoToVeloxConnector.cpp` | Modify | Remove ClpPrestoToVeloxConnector implementation |
| `presto_cpp/main/CMakeLists.txt` | Modify | Remove `velox_clp_connector` link |
| `presto_cpp/presto_protocol/presto_protocol.cpp` | Modify | Remove CLP protocol include |
| `presto_cpp/main/connectors/CMakeLists.txt` | Modify | Add `add_subdirectory(clp_plugin)` |

#### Files Referenced (No Changes)

| File | Purpose |
|------|---------|
| `velox/velox/connectors/clp/*` | Velox CLP connector implementation (compiled into plugin) |
| `velox/velox/connectors/clp/search_lib/*` | Archive/IR cursor implementations (compiled into plugin) |
| `presto_cpp/presto_protocol/connector/clp/*` | Protocol serialization types (compiled into plugin) |

---

### Part 1: Plugin Loading Infrastructure (Upstream to Presto)

> **Repository:** `prestodb/presto`
> **Purpose:** Enable `presto_server` to load connector plugins from a configured directory
> **Status:** ⚠️ **THIS CODE DOES NOT EXIST YET** - Must be implemented and submitted as a PR to upstream Presto

This is generic infrastructure that benefits all connector plugin authors, not just CLP. The implementation uses POSIX `dlopen()`/`dlsym()` to dynamically load shared libraries at runtime.

**Why this needs to be upstreamed:**
- RFC-0019 proposed this mechanism but it was never implemented
- PR #26650 and PR #26257 add serialization features, NOT plugin loading
- Without this infrastructure in upstream Presto, users cannot load external connector plugins

#### 1.1 Add `pluginDir` Configuration Property

**File:** `presto_cpp/main/common/Configs.h`

```cpp
// Add to SystemConfig class
/// Path to directory containing connector plugin .so files
static constexpr std::string_view kPluginDir{"plugin.dir"};
```

**File:** `presto_cpp/main/common/Configs.cpp`

```cpp
// Add to SystemConfig
std::string SystemConfig::pluginDir() const {
  return optionalProperty(std::string(kPluginDir)).value_or("");
}
```

**File:** `presto_cpp/main/common/Configs.h` (declaration)

```cpp
// Add to SystemConfig class public section
std::string pluginDir() const;
```

#### 1.2 Add Plugin Loading Functions

**File:** `presto_cpp/main/PrestoServer.h`

```cpp
// Add to PrestoServer class private section
void loadPlugins();
void loadPlugin(const std::string& path);
```

**File:** `presto_cpp/main/PrestoServer.cpp`

Add includes at top:
```cpp
#include <dlfcn.h>
#include <filesystem>
```

Add implementation:
```cpp
void PrestoServer::loadPlugin(const std::string& path) {
  LOG(INFO) << "Loading plugin from " << path;

  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    LOG(ERROR) << "Failed to load plugin " << path << ": " << dlerror();
    return;
  }

  dlerror(); // Clear any existing error
  auto registerFn = reinterpret_cast<void(*)()>(
      dlsym(handle, "registerExtensions"));
  const char* error = dlerror();

  if (error != nullptr) {
    LOG(ERROR) << "Plugin " << path << " missing registerExtensions: " << error;
    dlclose(handle);
    return;
  }

  if (registerFn == nullptr) {
    LOG(ERROR) << "Plugin " << path << " has null registerExtensions symbol";
    dlclose(handle);
    return;
  }

  registerFn();
  LOG(INFO) << "Successfully loaded plugin from " << path;
}

void PrestoServer::loadPlugins() {
  const auto pluginDir = systemConfig_->pluginDir();
  if (pluginDir.empty()) {
    LOG(INFO) << "No plugin directory configured (plugin.dir not set)";
    return;
  }

  if (!std::filesystem::exists(pluginDir)) {
    LOG(WARNING) << "Plugin directory does not exist: " << pluginDir;
    return;
  }

  LOG(INFO) << "Loading plugins from " << pluginDir;

  for (const auto& entry : std::filesystem::directory_iterator(pluginDir)) {
    const auto& path = entry.path();
    if (path.extension() == ".so" || path.extension() == ".dylib") {
      loadPlugin(path.string());
    }
  }
}
```

#### 1.3 Call loadPlugins() During Startup

**File:** `presto_cpp/main/PrestoServer.cpp`

In the `run()` method, add `loadPlugins()` call after connector registration:

```cpp
void PrestoServer::run() {
  // ... existing initialization code ...

  registerConnectors();
  loadPlugins();  // ADD THIS LINE - Load plugins after built-in connectors

  // ... rest of existing code ...
}
```

#### 1.4 Update CMakeLists.txt

**File:** `presto_cpp/main/CMakeLists.txt`

Add `${CMAKE_DL_LIBS}` to link the `dl` library for `dlopen`/`dlsym`:

```cmake
target_link_libraries(
  presto_server_lib
  # ... existing libraries ...
  ${CMAKE_DL_LIBS}  # ADD THIS LINE
)
```

#### 1.5 Summary of Part 1 Changes

| File | Change |
|------|--------|
| `presto_cpp/main/common/Configs.h` | Add `kPluginDir` constant and `pluginDir()` declaration |
| `presto_cpp/main/common/Configs.cpp` | Add `pluginDir()` implementation |
| `presto_cpp/main/PrestoServer.h` | Add `loadPlugins()` and `loadPlugin()` declarations |
| `presto_cpp/main/PrestoServer.cpp` | Add plugin loading implementation, call in `run()` |
| `presto_cpp/main/CMakeLists.txt` | Add `${CMAKE_DL_LIBS}` |

---

### Part 2: CLP Connector Plugin (Private Distribution)

> **Repository:** Private (YScope)
> **Purpose:** Build the CLP connector as a self-contained `.so` file
> **Prerequisite:** ⚠️ Part 1 must be implemented and merged into upstream Presto first

This part stays entirely in your private repository and is never merged upstream. The plugin contains:

1. **Entry Point** (`ClpPluginEntry.cpp`) - `extern "C" registerExtensions()` function called by `presto_server`
2. **Protocol Bridge** (`ClpPrestoToVeloxConnector`) - Converts Presto protocol types to Velox connector types
3. **Velox CLP Connector** - Compiled from `velox/connectors/clp/` (OBJECT library)
4. **CLP Search Library** - Compiled from `velox/connectors/clp/search_lib/`
5. **clp-s** - Statically linked into the plugin

#### 2.1 Create Plugin Directory Structure

```
presto_cpp/main/connectors/clp_plugin/
├── CMakeLists.txt                    # Build configuration for shared library
├── ClpPluginEntry.cpp                # extern "C" registerExtensions() entry point
├── ClpPrestoToVeloxConnector.h       # Bridge class header
└── ClpPrestoToVeloxConnector.cpp     # Bridge class implementation
```

#### 2.2 ClpPrestoToVeloxConnector.h

```cpp
// presto_cpp/main/connectors/clp_plugin/ClpPrestoToVeloxConnector.h

#pragma once

#include "presto_cpp/main/connectors/PrestoToVeloxConnector.h"

namespace facebook::presto {

class ClpPrestoToVeloxConnector final : public PrestoToVeloxConnector {
 public:
  explicit ClpPrestoToVeloxConnector(std::string connectorName)
      : PrestoToVeloxConnector(std::move(connectorName)) {}

  std::unique_ptr<velox::connector::ConnectorSplit> toVeloxSplit(
      const protocol::ConnectorId& catalogId,
      const protocol::ConnectorSplit* connectorSplit,
      const protocol::SplitContext* splitContext) const override;

  std::unique_ptr<velox::connector::ColumnHandle> toVeloxColumnHandle(
      const protocol::ColumnHandle* column,
      const TypeParser& typeParser) const override;

  std::unique_ptr<velox::connector::ConnectorTableHandle> toVeloxTableHandle(
      const protocol::TableHandle& tableHandle,
      const VeloxExprConverter& exprConverter,
      const TypeParser& typeParser,
      std::unordered_map<
          std::string,
          std::shared_ptr<velox::connector::ColumnHandle>>& assignments)
      const override;

  std::unique_ptr<protocol::ConnectorProtocol> createConnectorProtocol()
      const override;
};

} // namespace facebook::presto
```

#### 2.3 ClpPrestoToVeloxConnector.cpp

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

#### 2.4 ClpPluginEntry.cpp

```cpp
// presto_cpp/main/connectors/clp_plugin/ClpPluginEntry.cpp

#include "ClpPrestoToVeloxConnector.h"
#include "presto_cpp/main/connectors/PrestoToVeloxConnector.h"
#include "velox/connectors/clp/ClpConnector.h"

extern "C" {

/// Plugin entry point called by presto_server after dlopen().
void registerExtensions() {
  using namespace facebook::presto;
  using namespace facebook::velox::connector;

  const std::string connectorName =
      clp::ClpConnectorFactory::kClpConnectorName;

  // Register Velox connector factory
  if (!hasConnectorFactory(connectorName)) {
    registerConnectorFactory(std::make_shared<clp::ClpConnectorFactory>());
  }

  // Register Presto-to-Velox protocol bridge
  registerPrestoToVeloxConnector(
      std::make_unique<ClpPrestoToVeloxConnector>(connectorName));
}

} // extern "C"
```

#### 2.5 CMakeLists.txt

The CMakeLists.txt is the most complex part. Here's a detailed breakdown:

**Key Concepts:**

| Concept | Explanation |
|---------|-------------|
| **SHARED library** | Creates a `.so` (Linux) or `.dylib` (macOS) that can be loaded at runtime via `dlopen()` |
| **OBJECT library** | `velox_clp_connector` is an OBJECT library, meaning it compiles to `.o` files but not an archive. Must use `$<TARGET_OBJECTS:>` generator expression to link. |
| **Static linking clp-s** | clp-s libraries are linked statically into the plugin so users don't need to install clp-s separately |
| **Undefined symbols** | The plugin references symbols from `presto_server` (like `registerPrestoToVeloxConnector`). These are resolved at runtime when the plugin is loaded. |

```cmake
# presto_cpp/main/connectors/clp_plugin/CMakeLists.txt

cmake_minimum_required(VERSION 3.20)

# =============================================================================
# CLP Connector Plugin - Shared Library
# =============================================================================

# Create a SHARED library (produces libclp_connector.so)
add_library(clp_connector_plugin SHARED
    ClpPluginEntry.cpp
    ClpPrestoToVeloxConnector.cpp
    # Include CLP protocol source directly because presto_protocol is an OBJECT
    # library that includes connector protocols via #include, not linking.
    # We need to compile the protocol source into our plugin.
    ${CMAKE_SOURCE_DIR}/presto_cpp/presto_protocol/connector/clp/presto_protocol_clp.cpp
)

# -----------------------------------------------------------------------------
# Include Directories
# -----------------------------------------------------------------------------
# These allow the plugin source files to find headers
target_include_directories(clp_connector_plugin
    PRIVATE
        ${CMAKE_SOURCE_DIR}          # For presto_cpp/ includes
        ${CMAKE_SOURCE_DIR}/velox    # For velox/ includes
)

# -----------------------------------------------------------------------------
# Link Dependencies
# -----------------------------------------------------------------------------

# IMPORTANT: velox_clp_connector is an OBJECT library (not STATIC or SHARED).
# OBJECT libraries compile sources to .o files but don't create an archive.
# To link an OBJECT library, you must use the $<TARGET_OBJECTS:> generator
# expression, which expands to the list of .o files.
target_link_libraries(clp_connector_plugin
    PRIVATE
        # Velox CLP connector objects - MUST use $<TARGET_OBJECTS:> because
        # velox_clp_connector is an OBJECT library
        $<TARGET_OBJECTS:velox_clp_connector>

        # CLP search libraries (these are STATIC libraries)
        # Located in velox/connectors/clp/search_lib/
        clp-s-search           # Base cursor classes
        clp-s-archive-search   # Archive format support
        clp-s-ir-search        # IR format support

        # clp-s core dependencies (statically linked into plugin)
        # These are the YScope CLP libraries for reading compressed logs
        clp_s::archive_reader  # Reads CLP archive format
        clp_s::clp_dependencies # Common CLP dependencies
        clp_s::io              # I/O utilities
        clp_s::search          # Search functionality
        clp_s::search::ast     # AST for search queries
        clp_s::search::kql     # KQL (Kibana Query Language) parser

        # Velox dependencies needed by the connector
        velox_connector        # Base connector interfaces
        velox_vector           # Vector/column types
        velox_type             # Type system

        # Common dependencies
        simdjson::simdjson     # JSON parsing (used by protocol)
        Folly::folly           # Facebook's core library
        fmt::fmt               # String formatting
        glog::glog             # Logging
)

# -----------------------------------------------------------------------------
# Linker Options
# -----------------------------------------------------------------------------

# The plugin references symbols that exist in presto_server but are not linked
# into the plugin itself. For example:
#   - registerPrestoToVeloxConnector() from PrestoToVeloxConnector.cpp
#   - registerConnectorFactory() from Velox
#
# These symbols will be resolved at runtime when presto_server loads the plugin
# via dlopen() with RTLD_GLOBAL flag.
#
# Without these linker options, the build would fail with "undefined symbol" errors.
if(APPLE)
    # macOS: Allow undefined symbols, resolve at runtime
    target_link_options(clp_connector_plugin
        PRIVATE "-Wl,-undefined,dynamic_lookup")
else()
    # Linux: Allow shared library to have undefined symbols
    target_link_options(clp_connector_plugin
        PRIVATE "-Wl,--allow-shlib-undefined")
endif()

# -----------------------------------------------------------------------------
# Properties
# -----------------------------------------------------------------------------

# C++20 required for clp-s library
target_compile_features(clp_connector_plugin PRIVATE cxx_std_20)

set_target_properties(clp_connector_plugin PROPERTIES
    # Required for shared libraries - all code must be position independent
    POSITION_INDEPENDENT_CODE ON
    # Output name: libclp_connector.so (without "_plugin" suffix)
    OUTPUT_NAME "clp_connector"
    # Ensure "lib" prefix on all platforms
    PREFIX "lib"
)

# -----------------------------------------------------------------------------
# Installation
# -----------------------------------------------------------------------------

# Install to standard plugin location
install(TARGETS clp_connector_plugin
    LIBRARY DESTINATION lib/presto/plugins
)
```

**Dependency Graph:**

```
libclp_connector.so
├── ClpPluginEntry.cpp (registerExtensions)
├── ClpPrestoToVeloxConnector.cpp (protocol bridge)
├── presto_protocol_clp.cpp (JSON serialization)
├── $<TARGET_OBJECTS:velox_clp_connector>
│   ├── ClpConnector.cpp
│   ├── ClpDataSource.cpp
│   ├── ClpConfig.cpp
│   └── ClpTableHandle.cpp
├── clp-s-search (STATIC)
│   ├── clp-s-archive-search (STATIC)
│   │   └── clp_s::archive_reader, clp_s::search, etc.
│   └── clp-s-ir-search (STATIC)
│       └── clp_s::archive_reader, clp_s::search, etc.
├── velox_connector (resolved at runtime)
├── velox_vector (resolved at runtime)
└── Folly, fmt, glog, simdjson
```

#### 2.6 Remove CLP from Static Build

Remove CLP connector from the static `presto_server` build so it's only loaded as a plugin. This ensures:
- No duplicate registration when plugin loads
- Smaller `presto_server` binary
- Clean separation between built-in and plugin connectors

##### 2.6.1 Modify `presto_cpp/main/connectors/Registration.cpp`

**Remove the CLP include at the top of the file:**
```cpp
// REMOVE this line:
#include "velox/connectors/clp/ClpConnector.h"
```

**Remove CLP factory registration from `registerConnectorFactories()`:**
```cpp
void registerConnectorFactories() {
  // ... keep other connectors (Hive, TPCH, etc.) ...

  // REMOVE this entire block:
  if (!velox::connector::hasConnectorFactory(
          velox::connector::clp::ClpConnectorFactory::kClpConnectorName)) {
    velox::connector::registerConnectorFactory(
        std::make_shared<velox::connector::clp::ClpConnectorFactory>());
  }
}
```

**Remove CLP bridge registration from `registerConnectors()`:**
```cpp
void registerConnectors() {
  registerConnectorFactories();
  // ... keep other connectors ...

  // REMOVE this entire block:
  registerPrestoToVeloxConnector(
      std::make_unique<ClpPrestoToVeloxConnector>(
          velox::connector::clp::ClpConnectorFactory::kClpConnectorName));
}
```

##### 2.6.2 Modify `presto_cpp/main/connectors/PrestoToVeloxConnector.h`

**Remove the `ClpPrestoToVeloxConnector` class declaration (approximately lines 227-252):**
```cpp
// REMOVE this entire class:
class ClpPrestoToVeloxConnector final : public PrestoToVeloxConnector {
 public:
  explicit ClpPrestoToVeloxConnector(std::string connectorName)
      : PrestoToVeloxConnector(std::move(connectorName)) {}

  std::unique_ptr<velox::connector::ConnectorSplit> toVeloxSplit(
      const protocol::ConnectorId& catalogId,
      const protocol::ConnectorSplit* connectorSplit,
      const protocol::SplitContext* splitContext) const override;

  // ... rest of the class ...
};
```

Also remove the CLP-related includes:
```cpp
// REMOVE these includes:
#include "velox/connectors/clp/ClpColumnHandle.h"
#include "velox/connectors/clp/ClpConnectorSplit.h"
#include "velox/connectors/clp/ClpTableHandle.h"
```

##### 2.6.3 Modify `presto_cpp/main/connectors/PrestoToVeloxConnector.cpp`

**Remove the entire `ClpPrestoToVeloxConnector` implementation:**
```cpp
// REMOVE all ClpPrestoToVeloxConnector method implementations:
// - toVeloxSplit()
// - toVeloxColumnHandle()
// - toVeloxTableHandle()
// - createConnectorProtocol()
```

Also remove the CLP-related includes:
```cpp
// REMOVE these includes:
#include "presto_cpp/presto_protocol/connector/clp/ClpConnectorProtocol.h"
#include "presto_cpp/presto_protocol/connector/clp/presto_protocol_clp.h"
#include "velox/connectors/clp/ClpColumnHandle.h"
#include "velox/connectors/clp/ClpConnector.h"
#include "velox/connectors/clp/ClpConnectorSplit.h"
#include "velox/connectors/clp/ClpTableHandle.h"
```

##### 2.6.4 Modify `presto_cpp/main/CMakeLists.txt`

**Remove `velox_clp_connector` from `presto_server_lib` link libraries:**
```cmake
target_link_libraries(
  presto_server_lib
  # ... other libraries ...
  # velox_clp_connector  # REMOVE this line
  # ...
)
```

##### 2.6.5 Modify `presto_cpp/presto_protocol/presto_protocol.cpp`

**Remove the CLP protocol include:**
```cpp
// REMOVE this line (around line 19):
#include "presto_cpp/presto_protocol/connector/clp/presto_protocol_clp.cpp"
```

##### 2.6.6 Modify `presto_cpp/main/connectors/CMakeLists.txt`

**Add the plugin subdirectory at the end of the file:**
```cmake
# Existing content...

# ADD this line at the end:
add_subdirectory(clp_plugin)
```

##### 2.6.7 Summary of Removals

| File | What to Remove |
|------|----------------|
| `Registration.cpp` | CLP include, factory registration, bridge registration |
| `PrestoToVeloxConnector.h` | `ClpPrestoToVeloxConnector` class, CLP includes |
| `PrestoToVeloxConnector.cpp` | `ClpPrestoToVeloxConnector` implementation, CLP includes |
| `presto_cpp/main/CMakeLists.txt` | `velox_clp_connector` from link libraries |
| `presto_protocol.cpp` | CLP protocol include |

---

## Build Instructions

### Building the Plugin

```bash
cd presto-native-execution
mkdir -p _build/release && cd _build/release

cmake ../.. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPRESTO_ENABLE_PARQUET=ON

# Build the plugin
make -j$(nproc) clp_connector_plugin

# Output: _build/release/presto_cpp/main/connectors/clp_plugin/libclp_connector.so
```

### Verify Plugin

```bash
# Check entry point symbol exists
nm -D libclp_connector.so | grep registerExtensions
# Expected: T registerExtensions

# Check clp-s is statically linked (should NOT appear in ldd output)
ldd libclp_connector.so | grep clp
# Expected: no output (clp-s is embedded)
```

---

## Deployment

### For End Users

1. **Build or download `presto_server`** from upstream Presto (with Part 1 merged)

2. **Download `libclp_connector.so`** from YScope

3. **Create plugin directory and copy plugin:**
   ```bash
   mkdir -p /opt/presto/plugins
   cp libclp_connector.so /opt/presto/plugins/
   ```

4. **Configure Presto worker** - add to `config.properties`:
   ```properties
   plugin.dir=/opt/presto/plugins
   ```

5. **Create CLP catalog** - create `clp.properties`:
   ```properties
   connector.name=clp
   clp.storage-type=FS
   ```

6. **Start worker:**
   ```bash
   ./presto_server --etc-dir=/etc/presto
   ```

---

## Verification

### Check Plugin Loaded

Look for log messages during startup:
```
INFO: Loading plugins from /opt/presto/plugins
INFO: Loading plugin from /opt/presto/plugins/libclp_connector.so
INFO: Successfully loaded plugin from /opt/presto/plugins/libclp_connector.so
```

### Test Query

```sql
USE clp.default;
SHOW TABLES;
SELECT * FROM logs LIMIT 10;
```

### Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| "No plugin directory configured" | `plugin.dir` not set | Add `plugin.dir=/path/to/plugins` to config.properties |
| "Failed to load plugin: cannot open shared object" | Missing dependencies | Check `ldd libclp_connector.so` for missing libs |
| "missing registerExtensions" | Wrong symbol name or missing extern "C" | Verify `nm -D libclp_connector.so \| grep registerExtensions` shows `T registerExtensions` |
| Connector not found | Plugin loaded but registration failed | Check logs for errors after "Loading plugin" |

---

## Summary

### Current State

**Plugin loading does NOT exist in Presto today.** You must:
1. Implement Part 1 (plugin loading infrastructure)
2. Submit PR to upstream `prestodb/presto`
3. Wait for it to be merged
4. Then Part 2 (CLP plugin) becomes usable

### Ownership

| What | Where | Who Maintains | Status |
|------|-------|---------------|--------|
| Plugin loading infrastructure | Upstream Presto (`prestodb/presto`) | Presto community (after your PR) | **Must be implemented** |
| `libclp_connector.so` | YScope distribution | YScope (private) | After Part 1 merges |
| Velox CLP Connector source | YScope Velox fork | YScope (private) | Existing |

### End-user Workflow (After Part 1 is Merged)

1. Build `presto_server` from stock upstream Presto
2. Download `libclp_connector.so` from YScope
3. Set `plugin.dir=/opt/presto/plugins` in config
4. Run `./presto_server`
