/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "folly/Executor.h"
#include "velox/connectors/Connector.h"

// Forward declaration for ConnectorFactory.
namespace facebook::velox::connector {
class ConnectorFactory;
} // namespace facebook::velox::connector

namespace facebook::presto {
using facebook::velox::connector::ConnectorFactory;

namespace detail {

/// Returns the singleton map of connector factories.
/// NOT inline — defined in Registration.cpp so that dlopen'd plugins
/// share the same map as the main executable.
std::unordered_map<std::string, std::shared_ptr<ConnectorFactory>>&
connectorFactories();

} // namespace detail

/// Adds a factory for creating connectors to the registry using connector
/// name as the key. Throws if factory with the same name is already present.
/// Always returns true. The return value makes it easy to use with
/// FB_ANONYMOUS_VARIABLE.
/// NOT inline — defined in Registration.cpp so that dlopen'd plugins
/// register in the same map as the main executable.
bool registerConnectorFactory(std::shared_ptr<ConnectorFactory> factory);

/// Returns true if a connector with the specified name has been registered,
/// false otherwise.
bool hasConnectorFactory(const std::string& connectorName);

/// Unregister a connector factory by name.
/// Returns true if a connector with the specified name has been
/// unregistered, false otherwise.
bool unregisterConnectorFactory(const std::string& connectorName);

/// Returns a factory for creating connectors with the specified name.
/// Throws if factory doesn't exist.
std::shared_ptr<ConnectorFactory> getConnectorFactory(
    const std::string& connectorName);

/// Returns a list of all registered connector factory names.
std::vector<std::string> listConnectorFactories();

/// Registers all connector factories using the facebook::presto namespace
/// factory registry.
void registerConnectorFactories();

void registerConnectors();

} // namespace facebook::presto
