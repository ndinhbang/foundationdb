/*
 * RESTUtils.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
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

#include "fdbclient/RESTUtils.h"
#include "fdbclient/Knobs.h"

#include "flow/flat_buffers.h"
#include "flow/UnitTest.h"
#include "flow/IConnection.h"

#include <boost/algorithm/string.hpp>
#include <queue>

#include "flow/actorcompiler.h" // always the last include

const std::unordered_map<std::string, RESTConnectionType> RESTConnectionType::supportedConnTypes = {
	{ "http", RESTConnectionType("http", RESTConnectionType::NOT_SECURE_CONNECTION) },
	{ "https", RESTConnectionType("https", RESTConnectionType::SECURE_CONNECTION) }
};

RESTConnectionType RESTConnectionType::getConnectionType(const std::string& protocol) {
	auto itr = RESTConnectionType::supportedConnTypes.find(protocol);
	if (itr == RESTConnectionType::supportedConnTypes.end()) {
		TraceEvent("RESTConnectionTypeUnsupportedPrototocol").detail("Protocol", protocol);
		CODE_PROBE(true, "REST URI unsupported protocol");
		throw rest_unsupported_protocol();
	}
	return itr->second;
}

bool RESTConnectionType::isProtocolSupported(const std::string& protocol) {
	auto itr = RESTConnectionType::supportedConnTypes.find(protocol);
	return itr != RESTConnectionType::supportedConnTypes.end();
}

bool RESTConnectionType::isSecure(const std::string& protocol) {
	auto itr = RESTConnectionType::supportedConnTypes.find(protocol);
	if (itr == RESTConnectionType::supportedConnTypes.end()) {
		TraceEvent("RESTConnectionTypeUnsupportedPrototocol").detail("Protocol", protocol);
		throw rest_unsupported_protocol();
	}
	return itr->second.secure == RESTConnectionType::SECURE_CONNECTION;
}

RESTClientKnobs::RESTClientKnobs() {
	connection_pool_size = FLOW_KNOBS->RESTCLIENT_MAX_CONNECTIONPOOL_SIZE;
	connect_tries = FLOW_KNOBS->RESTCLIENT_CONNECT_TRIES;
	connect_timeout = FLOW_KNOBS->RESTCLIENT_CONNECT_TIMEOUT;
	max_connection_life = FLOW_KNOBS->RESTCLIENT_MAX_CONNECTION_LIFE;
	request_tries = FLOW_KNOBS->RESTCLIENT_REQUEST_TRIES;
	request_timeout_secs = FLOW_KNOBS->RESTCLIENT_REQUEST_TIMEOUT_SEC;

	knobMap["connection_pool_size"] = std::addressof(connection_pool_size);
	knobMap["pz"] = std::addressof(connection_pool_size);
	knobMap["connect_tries"] = std::addressof(connect_tries);
	knobMap["ct"] = std::addressof(connect_tries);
	knobMap["connect_timeout"] = std::addressof(connect_timeout);
	knobMap["cto"] = std::addressof(connect_timeout);
	knobMap["max_connection_life"] = std::addressof(max_connection_life);
	knobMap["mcl"] = std::addressof(max_connection_life);
	knobMap["request_tries"] = std::addressof(request_tries);
	knobMap["rt"] = std::addressof(request_tries);
	knobMap["request_timeout_secs"] = std::addressof(request_timeout_secs);
	knobMap["rtom"] = std::addressof(request_timeout_secs);
}

void RESTClientKnobs::set(const std::unordered_map<std::string, int>& knobSettings) {
	TraceEvent trace = TraceEvent("RESTClientSetKnobs");

	for (const auto& itr : knobSettings) {
		const auto& kItr = RESTClientKnobs::knobMap.find(itr.first);
		if (kItr == RESTClientKnobs::knobMap.end()) {
			trace.detail("RESTClientInvalidKnobName", itr.first);
			throw rest_invalid_rest_client_knob();
		}
		ASSERT_EQ(itr.first.compare(kItr->first), 0);
		*(kItr->second) = itr.second;
		trace.detail(itr.first.c_str(), itr.second);
	}
}

std::unordered_map<std::string, int> RESTClientKnobs::get() const {
	std::unordered_map<std::string, int> details = {
		{ "connection_pool_size", connection_pool_size },
		{ "connect_tries", connect_tries },
		{ "connect_timeout", connect_timeout },
		{ "max_connection_life", max_connection_life },
		{ "request_tries", request_tries },
		{ "request_timeout_secs", request_timeout_secs },
	};

	return details;
}

ACTOR Future<RESTConnectionPool::ReusableConnection> connect_impl(Reference<RESTConnectionPool> connectionPool,
                                                                  RESTConnectionPoolKey connectKey,
                                                                  bool isSecure,
                                                                  int maxConnLife) {
	auto poolItr = connectionPool->connectionPoolMap.find(connectKey);
	while (poolItr != connectionPool->connectionPoolMap.end() && !poolItr->second.empty()) {
		RESTConnectionPool::ReusableConnection rconn = poolItr->second.front();
		poolItr->second.pop();

		if (rconn.expirationTime > now()) {
			TraceEvent("RESTClientReuseConn")
			    .suppressFor(60)
			    .detail("Host", connectKey.first)
			    .detail("Service", connectKey.second)
			    .detail("RemoteEndpoint", rconn.conn->getPeerAddress())
			    .detail("ExpireIn", rconn.expirationTime - now());
			return rconn;
		}
	}

	// No valid connection exists, create a new one
	state Reference<IConnection> conn =
	    wait(INetworkConnections::net()->connect(connectKey.first, connectKey.second, isSecure));
	wait(conn->connectHandshake());

	RESTConnectionPool::ReusableConnection reusableConn =
	    RESTConnectionPool::ReusableConnection({ conn, now() + maxConnLife });
	connectionPool->connectionPoolMap.insert(
	    { connectKey, std::queue<RESTConnectionPool::ReusableConnection>({ reusableConn }) });

	TraceEvent("RESTClientCreateNewConn")
	    .suppressFor(60)
	    .detail("Host", connectKey.first)
	    .detail("Service", connectKey.second)
	    .detail("RemoteEndpoint", conn->getPeerAddress());
	return reusableConn;
}

Future<RESTConnectionPool::ReusableConnection> RESTConnectionPool::connect(RESTConnectionPoolKey connectKey,
                                                                           const bool isSecure,
                                                                           const int maxConnLife) {
	return connect_impl(Reference<RESTConnectionPool>::addRef(this), connectKey, isSecure, maxConnLife);
}

void RESTConnectionPool::returnConnection(RESTConnectionPoolKey connectKey,
                                          ReusableConnection& rconn,
                                          const int maxConnections) {
	auto poolItr = connectionPoolMap.find(connectKey);
	if (poolItr == connectionPoolMap.end()) {
		throw rest_connectpool_key_not_found();
	}

	// If it expires in the future then add it to the pool in the front iff connection pool size is not maxed
	if (rconn.expirationTime > now() && poolItr->second.size() < maxConnections) {
		poolItr->second.push(rconn);
	}
	rconn.conn = Reference<IConnection>();
}

RESTUrl::RESTUrl(const std::string& fUrl) {
	parseUrl(fUrl);
}

RESTUrl::RESTUrl(const std::string& fullUrl, const std::string& b) : body(b) {
	parseUrl(fullUrl);
}

void RESTUrl::parseUrl(const std::string& fullUrl) {
	// Sample valid URIs
	// 1. With 'host' & 'resource' := '<protocol>://<host>/<resource>'
	// 2. With 'host', 'service' & 'resource' := '<protocol>://<host>:port/<resource>'
	// 3. With 'host', 'service', 'resource' & 'reqParameters' := '<protocol>://<host>:port/<resource>?<parameter-list>'

	try {
		StringRef t(fullUrl);
		StringRef p = t.eat("://");
		std::string protocol = p.toString();
		boost::algorithm::to_lower(protocol);
		this->connType = RESTConnectionType::getConnectionType(protocol);
		if (!this->connType.secure && !CLIENT_KNOBS->REST_KMS_ENABLE_NOT_SECURE_CONNECTION) {
			TraceEvent("RESTUtilsNotSecureConnNotSupported").detail("Protocol", protocol);
			CODE_PROBE(true, "REST URI not-secure connection not supported");
			throw rest_unsupported_protocol();
		}

		// extract 'resource' and optional 'parameter list' if supplied in the URL
		uint8_t foundSeparator = 0;
		StringRef hostPort = t.eatAny("/?", &foundSeparator);
		if (foundSeparator == '/') {
			this->resource = t.eat("?").toString();
			this->reqParameters = t.eat().toString();
		}

		// hostPort is at least a host or IP address, optionally followed by :portNumber or :serviceName
		StringRef hRef(hostPort);
		StringRef h = hRef.eat(":");
		if (h.size() == 0) {
			CODE_PROBE(true, "REST URI empty host");
			throw std::string("host cannot be empty");
		}
		this->host = h.toString();
		this->service = hRef.eat().toString();

		TraceEvent(SevDebug, "RESTClientParseURI")
		    .detail("URI", fullUrl)
		    .detail("Host", this->host)
		    .detail("Service", this->service)
		    .detail("Resource", this->resource)
		    .detail("ReqParameters", this->reqParameters)
		    .detail("ConnectionType", this->connType.toString());
	} catch (std::string& err) {
		TraceEvent(SevWarnAlways, "RESTClientParseError").detail("URI", fullUrl).detail("Error", err);
		throw rest_invalid_uri();
	}
}

// Only used to link unit tests
void forceLinkRESTUtilsTests() {}

TEST_CASE("/RESTUtils/InvalidProtocol") {
	try {
		std::string uri("httpx://foo/bar");
		RESTUrl r(uri);
		ASSERT(false);
	} catch (Error& e) {
		if (e.code() != error_code_rest_unsupported_protocol) {
			throw e;
		}
	}
	return Void();
}

TEST_CASE("/RESTUtils/MissingHost") {
	try {
		std::string uri("https://:/bar");
		RESTUrl r(uri);
		ASSERT(false);
	} catch (Error& e) {
		if (e.code() != error_code_rest_invalid_uri) {
			throw e;
		}
	}
	return Void();
}

TEST_CASE("/RESTUtils/ValidURIWithService") {
	std::string uri("https://host:80/foo/bar");
	RESTUrl r(uri);
	ASSERT_EQ(r.connType.secure, RESTConnectionType::SECURE_CONNECTION);
	ASSERT_EQ(r.host.compare("host"), 0);
	ASSERT_EQ(r.service.compare("80"), 0);
	ASSERT_EQ(r.resource.compare("foo/bar"), 0);
	return Void();
}

TEST_CASE("/RESTUtils/ValidURIWithoutService") {
	std::string uri("https://host/foo/bar");
	RESTUrl r(uri);
	ASSERT_EQ(r.connType.secure, RESTConnectionType::SECURE_CONNECTION);
	ASSERT_EQ(r.host.compare("host"), 0);
	ASSERT(r.service.empty());
	ASSERT_EQ(r.resource.compare("foo/bar"), 0);
	return Void();
}

TEST_CASE("/RESTUtils/ValidURIWithParamsSecure") {
	std::string uri("https://host/foo/bar?param1,param2");
	RESTUrl r(uri);
	ASSERT_EQ(r.connType.secure, RESTConnectionType::SECURE_CONNECTION);
	ASSERT_EQ(r.host.compare("host"), 0);
	ASSERT(r.service.empty());
	ASSERT_EQ(r.resource.compare("foo/bar"), 0);
	ASSERT_EQ(r.reqParameters.compare("param1,param2"), 0);
	return Void();
}

TEST_CASE("/RESTUtils/ValidURIWithParamsKnobNotEnabled") {
	auto& g_knobs = IKnobCollection::getMutableGlobalKnobCollection();
	g_knobs.setKnob("rest_kms_enable_not_secure_connection", KnobValueRef::create(bool{ false }));
	std::string uri("http://host/foo/bar?param1,param2");
	try {
		RESTUrl r(uri);
		ASSERT(false);
	} catch (Error& e) {
		ASSERT_EQ(e.code(), error_code_rest_unsupported_protocol);
	}
	return Void();
}

TEST_CASE("/RESTUtils/ValidURIWithParams") {
	auto& g_knobs = IKnobCollection::getMutableGlobalKnobCollection();
	g_knobs.setKnob("rest_kms_enable_not_secure_connection", KnobValueRef::create(bool{ true }));
	std::string uri("http://host/foo/bar?param1,param2");
	RESTUrl r(uri);
	ASSERT_EQ(r.connType.secure, RESTConnectionType::NOT_SECURE_CONNECTION);
	ASSERT_EQ(r.host.compare("host"), 0);
	ASSERT(r.service.empty());
	ASSERT_EQ(r.resource.compare("foo/bar"), 0);
	ASSERT_EQ(r.reqParameters.compare("param1,param2"), 0);
	return Void();
}