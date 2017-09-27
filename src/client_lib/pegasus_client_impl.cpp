// Copyright (c) 2017, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#include <cctype>
#include <algorithm>
#include <string>
#include <stdint.h>

#include <dsn/cpp/auto_codes.h>
#include <dsn/cpp/serialization_helper/dsn.layer2_types.h>
#include <rrdb/rrdb.code.definition.h>
#include <rrdb/rrdb.types.h>
#include <pegasus/error.h>
#include <pegasus_utils.h>
#include "pegasus_client_impl.h"

#ifdef __TITLE__
#undef __TITLE__
#endif
#define __TITLE__ "pegasus.client.impl"

using namespace ::dsn;

namespace pegasus {
namespace client {

#define ROCSKDB_ERROR_START -1000

std::unordered_map<int, std::string> pegasus_client_impl::_client_error_to_string;
std::unordered_map<int, int> pegasus_client_impl::_server_error_to_client;

pegasus_client_impl::pegasus_client_impl(const char *cluster_name, const char *app_name)
    : _cluster_name(cluster_name), _app_name(app_name)
{
    _server_uri = "dsn://" + _cluster_name + "/" + _app_name;
    _server_address.assign_uri(dsn_uri_build(_server_uri.c_str()));
    _client = new ::dsn::apps::rrdb_client(_server_address);

    std::string section = "uri-resolver.dsn://" + _cluster_name;
    std::string server_list = dsn_config_get_value_string(section.c_str(), "arguments", "", "");
    std::vector<std::string> lv;
    ::dsn::utils::split_args(server_list.c_str(), lv, ',');
    std::vector<dsn::rpc_address> meta_servers;
    for (auto &s : lv) {
        ::dsn::rpc_address addr;
        if (!addr.from_string_ipv4(s.c_str())) {
            dassert(false,
                    "invalid address '%s' specified in config [%s].arguments",
                    s.c_str(),
                    section.c_str());
        }
        meta_servers.push_back(addr);
    }
    dassert(meta_servers.size() > 0,
            "no meta server specified in config [%s].arguments",
            section.c_str());

    _meta_server.assign_group(dsn_group_build("meta-servers"));
    for (auto &ms : meta_servers) {
        dsn_group_add(_meta_server.group_handle(), ms.c_addr());
    }
}

pegasus_client_impl::~pegasus_client_impl()
{
    delete _client;
    dsn_uri_destroy(_server_address.group_handle());
}

const char *pegasus_client_impl::get_cluster_name() const { return _cluster_name.c_str(); }

const char *pegasus_client_impl::get_app_name() const { return _app_name.c_str(); }

int pegasus_client_impl::set(const std::string &hash_key,
                             const std::string &sort_key,
                             const std::string &value,
                             int timeout_milliseconds,
                             int ttl_seconds,
                             internal_info *info)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback = [&](int err, internal_info &&_info) {
        ret = err;
        if (info != nullptr)
            (*info) = std::move(_info);
        op_completed.notify();
    };
    async_set(hash_key, sort_key, value, std::move(callback), timeout_milliseconds, ttl_seconds);
    op_completed.wait();
    return ret;
}

void pegasus_client_impl::async_set(const std::string &hash_key,
                                    const std::string &sort_key,
                                    const std::string &value,
                                    async_set_callback_t &&callback,
                                    int timeout_milliseconds,
                                    int ttl_seconds)
{
    // check params
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, internal_info());
        return;
    }
    ::dsn::apps::update_request req;
    pegasus_generate_key(req.key, hash_key, sort_key);
    req.value.assign(value.c_str(), 0, value.size());
    if (ttl_seconds == 0)
        req.expire_ts_seconds = 0;
    else
        req.expire_ts_seconds = ttl_seconds + utils::epoch_now();

    auto partition_hash = pegasus_key_hash(req.key);

    // wrap the user defined callback function, generate a new callback function.
    auto new_callback = [user_callback = std::move(callback)](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        if (user_callback == nullptr) {
            err.end_tracking();
            return;
        }
        internal_info info;
        ::dsn::apps::update_response response;
        if (err == ::dsn::ERR_OK) {
            ::dsn::unmarshall(resp, response);
            info.app_id = response.app_id;
            info.partition_index = response.partition_index;
            info.decree = response.decree;
            info.server = response.server;
        }
        auto ret = get_client_error(
            (err == ::dsn::ERR_OK) ? get_rocksdb_server_error(response.error) : err.get());
        user_callback(ret, std::move(info));
    };
    _client->put(req,
                 std::move(new_callback),
                 std::chrono::milliseconds(timeout_milliseconds),
                 0,
                 partition_hash);
}

int pegasus_client_impl::multi_set(const std::string &hash_key,
                                   const std::map<std::string, std::string> &kvs,
                                   int timeout_milliseconds,
                                   int ttl_seconds,
                                   internal_info *info)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback = [&](int err, internal_info &&_info) {
        ret = err;
        if (info != nullptr)
            (*info) = std::move(_info);
        op_completed.notify();
    };
    async_multi_set(hash_key, kvs, std::move(callback), timeout_milliseconds, ttl_seconds);
    op_completed.wait();
    return ret;
}

void pegasus_client_impl::async_multi_set(const std::string &hash_key,
                                          const std::map<std::string, std::string> &kvs,
                                          async_multi_set_callback_t &&callback,
                                          int timeout_milliseconds,
                                          int ttl_seconds)
{
    // check params
    if (hash_key.size() == 0) {
        derror("invalid hash key: hash key should not be empty for multi_set");
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, internal_info());
        return;
    }
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, internal_info());
        return;
    }
    if (kvs.empty()) {
        derror("invalid kvs: kvs should not be empty");
        if (callback != nullptr)
            callback(PERR_INVALID_VALUE, internal_info());
        return;
    }

    ::dsn::apps::multi_put_request req;
    req.hash_key = ::dsn::blob(hash_key.data(), 0, hash_key.size());
    for (auto &kv : kvs) {
        ::dsn::apps::key_value kv_blob;
        kv_blob.key = ::dsn::blob(kv.first.data(), 0, kv.first.size());
        kv_blob.value = ::dsn::blob(kv.second.data(), 0, kv.second.size());
        req.kvs.emplace_back(std::move(kv_blob));
    }
    if (ttl_seconds == 0)
        req.expire_ts_seconds = 0;
    else
        req.expire_ts_seconds = ttl_seconds + utils::epoch_now();

    ::dsn::blob tmp_key;
    pegasus_generate_key(tmp_key, req.hash_key, ::dsn::blob());
    auto partition_hash = pegasus_key_hash(tmp_key);
    // wrap the user-defined-callback-function, generate a new callback function.
    auto new_callback = [user_callback = std::move(callback)](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        if (user_callback == nullptr) {
            err.end_tracking();
            return;
        }
        internal_info info;
        ::dsn::apps::update_response response;
        if (err == ::dsn::ERR_OK) {
            ::dsn::unmarshall(resp, response);
            info.app_id = response.app_id;
            info.partition_index = response.partition_index;
            info.decree = response.decree;
            info.server = response.server;
        }
        auto ret = get_client_error(
            (err == ::dsn::ERR_OK) ? get_rocksdb_server_error(response.error) : err.get());
        user_callback(ret, std::move(info));
    };
    _client->multi_put(req,
                       std::move(new_callback),
                       std::chrono::milliseconds(timeout_milliseconds),
                       0,
                       partition_hash);
}

int pegasus_client_impl::get(const std::string &hash_key,
                             const std::string &sort_key,
                             std::string &value,
                             int timeout_milliseconds,
                             internal_info *info)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback = [&](int err, std::string &&str, internal_info &&_info) {
        ret = err;
        value = std::move(str);
        if (info != nullptr)
            (*info) = std::move(_info);
        op_completed.notify();
    };
    async_get(hash_key, sort_key, std::move(callback), timeout_milliseconds);
    op_completed.wait();
    return ret;
}

void pegasus_client_impl::async_get(const std::string &hash_key,
                                    const std::string &sort_key,
                                    async_get_callback_t &&callback,
                                    int timeout_milliseconds)
{
    // check params
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, std::string(), internal_info());
        return;
    }
    ::dsn::blob req;
    pegasus_generate_key(req, hash_key, sort_key);
    auto partition_hash = pegasus_key_hash(req);
    auto new_callback = [user_callback = std::move(callback)](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        if (user_callback == nullptr) {
            err.end_tracking();
            return;
        }
        std::string value;
        internal_info info;
        dsn::apps::read_response response;
        if (err == ::dsn::ERR_OK) {
            ::dsn::unmarshall(resp, response);
            if (response.error == 0) {
                value.assign(response.value.data(), response.value.length());
            }
            info.app_id = response.app_id;
            info.partition_index = response.partition_index;
            info.server = response.server;
        }
        int ret =
            get_client_error(err == ERR_OK ? get_rocksdb_server_error(response.error) : err.get());
        user_callback(ret, std::move(value), std::move(info));
    };
    _client->get(req,
                 std::move(new_callback),
                 std::chrono::milliseconds(timeout_milliseconds),
                 0,
                 partition_hash);
}

int pegasus_client_impl::multi_get(const std::string &hash_key,
                                   const std::set<std::string> &sort_keys,
                                   std::map<std::string, std::string> &values,
                                   int max_fetch_count,
                                   int max_fetch_size,
                                   int timeout_milliseconds,
                                   internal_info *info)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback =
        [&](int err, std::map<std::string, std::string> &&_values, internal_info &&_info) {
            ret = err;
            if (info != nullptr)
                (*info) = std::move(_info);
            values = std::move(_values);
            op_completed.notify();
        };
    async_multi_get(hash_key,
                    sort_keys,
                    std::move(callback),
                    max_fetch_count,
                    max_fetch_size,
                    timeout_milliseconds);
    op_completed.wait();
    return ret;
}

void pegasus_client_impl::async_multi_get(const std::string &hash_key,
                                          const std::set<std::string> &sort_keys,
                                          async_multi_get_callback_t &&callback,
                                          int max_fetch_count,
                                          int max_fetch_size,
                                          int timeout_milliseconds)
{
    // check params
    if (hash_key.size() == 0) {
        derror("invalid hash key: hash key should not be empty for multi_get");
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, std::map<std::string, std::string>(), internal_info());
        return;
    }
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, std::map<std::string, std::string>(), internal_info());
        return;
    }

    ::dsn::apps::multi_get_request req;
    req.hash_key = ::dsn::blob(hash_key.data(), 0, hash_key.size());
    req.max_kv_count = max_fetch_count;
    req.max_kv_size = max_fetch_size;
    for (auto &sort_key : sort_keys) {
        req.sort_keys.emplace_back(sort_key.data(), 0, sort_key.size());
    }
    req.no_value = false;
    ::dsn::blob tmp_key;
    pegasus_generate_key(tmp_key, req.hash_key, ::dsn::blob());
    auto partition_hash = pegasus_key_hash(tmp_key);
    auto new_callback = [user_callback = std::move(callback)](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        if (user_callback == nullptr) {
            err.end_tracking();
            return;
        }
        std::map<std::string, std::string> values;
        internal_info info;
        ::dsn::apps::multi_get_response response;
        if (err == ::dsn::ERR_OK) {
            ::unmarshall(resp, response);
            info.app_id = response.app_id;
            info.partition_index = response.partition_index;
            info.server = response.server;
            for (auto &kv : response.kvs)
                values.emplace(std::string(kv.key.data(), kv.key.length()),
                               std::string(kv.value.data(), kv.value.length()));
        }
        int ret =
            get_client_error(err == ERR_OK ? get_rocksdb_server_error(response.error) : err.get());
        user_callback(ret, std::move(values), std::move(info));
    };
    _client->multi_get(req,
                       std::move(new_callback),
                       std::chrono::milliseconds(timeout_milliseconds),
                       0,
                       partition_hash);
}

int pegasus_client_impl::multi_get_sortkeys(const std::string &hash_key,
                                            std::set<std::string> &sort_keys,
                                            int max_fetch_count,
                                            int max_fetch_size,
                                            int timeout_milliseconds,
                                            internal_info *info)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback = [&](int err, std::set<std::string> &&_sort_keys, internal_info &&_info) {
        ret = err;
        if (info != nullptr)
            (*info) = std::move(_info);
        sort_keys = std::move(_sort_keys);
        op_completed.notify();
    };
    async_multi_get_sortkeys(
        hash_key, std::move(callback), max_fetch_count, max_fetch_size, timeout_milliseconds);
    op_completed.wait();
    return ret;
}

void pegasus_client_impl::async_multi_get_sortkeys(const std::string &hash_key,
                                                   async_multi_get_sortkeys_callback_t &&callback,
                                                   int max_fetch_count,
                                                   int max_fetch_size,
                                                   int timeout_milliseconds)
{
    // check params
    if (hash_key.size() == 0) {
        derror("invalid hash key: hash key should not be empty for multi_get_sortkeys");
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, std::set<std::string>(), internal_info());
        return;
    }
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, std::set<std::string>(), internal_info());
        return;
    }

    ::dsn::apps::multi_get_request req;
    req.hash_key = ::dsn::blob(hash_key.data(), 0, hash_key.size());
    req.max_kv_count = max_fetch_count;
    req.max_kv_size = max_fetch_size;
    req.no_value = true;
    ::dsn::blob tmp_key;
    pegasus_generate_key(tmp_key, req.hash_key, ::dsn::blob());
    auto partition_hash = pegasus_key_hash(tmp_key);
    auto new_callback = [user_callback = std::move(callback)](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        if (user_callback == nullptr) {
            err.end_tracking();
            return;
        }
        std::set<std::string> sort_keys;
        internal_info info;
        ::dsn::apps::multi_get_response response;
        if (err == ::dsn::ERR_OK) {
            ::unmarshall(resp, response);
            info.app_id = response.app_id;
            info.partition_index = response.partition_index;
            info.server = response.server;
            for (auto &kv : response.kvs)
                sort_keys.insert(std::string(kv.key.data(), kv.key.length()));
        }
        int ret =
            get_client_error(err == ERR_OK ? get_rocksdb_server_error(response.error) : err.get());
        user_callback(ret, std::move(sort_keys), std::move(info));
    };
    _client->multi_get(req,
                       std::move(new_callback),
                       std::chrono::milliseconds(timeout_milliseconds),
                       0,
                       partition_hash);
}

int pegasus_client_impl::exist(const std::string &hash_key,
                               const std::string &sort_key,
                               int timeout_milliseconds,
                               internal_info *info)
{
    int ttl_seconds;
    return ttl(hash_key, sort_key, ttl_seconds, timeout_milliseconds, info);
}

int pegasus_client_impl::sortkey_count(const std::string &hash_key,
                                       int64_t &count,
                                       int timeout_milliseconds,
                                       internal_info *info)
{
    // check params
    if (hash_key.size() == 0) {
        derror("invalid hash key: hash key should not be empty for sortkey_count");
        return PERR_INVALID_HASH_KEY;
    }
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        return PERR_INVALID_HASH_KEY;
    }

    ::dsn::blob tmp_key;
    pegasus_generate_key(tmp_key, hash_key, std::string());
    auto partition_hash = pegasus_key_hash(tmp_key);
    auto pr = _client->sortkey_count_sync(::dsn::blob(hash_key.data(), 0, hash_key.length()),
                                          std::chrono::milliseconds(timeout_milliseconds),
                                          0,
                                          partition_hash);
    if (pr.first == ERR_OK && pr.second.error == 0) {
        count = pr.second.count;
    }
    if (info != nullptr) {
        if (pr.first == ERR_OK) {
            info->app_id = pr.second.app_id;
            info->partition_index = pr.second.partition_index;
            info->decree = -1;
            info->server = pr.second.server;
        } else {
            info->app_id = -1;
            info->partition_index = -1;
            info->decree = -1;
        }
    }
    return get_client_error(pr.first == ERR_OK ? get_rocksdb_server_error(pr.second.error)
                                               : pr.first.get());
}

int pegasus_client_impl::del(const std::string &hash_key,
                             const std::string &sort_key,
                             int timeout_milliseconds,
                             internal_info *info)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback = [&](int err, internal_info &&_info) {
        ret = err;
        if (info != nullptr)
            (*info) = std::move(_info);
        op_completed.notify();
    };
    async_del(hash_key, sort_key, std::move(callback), timeout_milliseconds);
    op_completed.wait();
    return ret;
}

void pegasus_client_impl::async_del(const std::string &hash_key,
                                    const std::string &sort_key,
                                    async_del_callback_t &&callback,
                                    int timeout_milliseconds)
{
    // check params
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, internal_info());
        return;
    }

    ::dsn::blob req;
    pegasus_generate_key(req, hash_key, sort_key);
    auto partition_hash = pegasus_key_hash(req);

    auto new_callback = [user_callback = std::move(callback)](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        if (user_callback == nullptr) {
            err.end_tracking();
            return;
        }
        ::dsn::apps::update_response response;
        internal_info info;
        if (err == ::dsn::ERR_OK) {
            ::dsn::unmarshall(resp, response);
            info.app_id = response.app_id;
            info.partition_index = response.partition_index;
            info.decree = response.decree;
            info.server = response.server;
        }
        int ret =
            get_client_error(err == ERR_OK ? get_rocksdb_server_error(response.error) : err.get());
        user_callback(ret, std::move(info));
    };
    _client->remove(req,
                    std::move(new_callback),
                    std::chrono::milliseconds(timeout_milliseconds),
                    0,
                    partition_hash);
}

int pegasus_client_impl::multi_del(const std::string &hash_key,
                                   const std::set<std::string> &sort_keys,
                                   int64_t &deleted_count,
                                   int timeout_milliseconds,
                                   internal_info *info)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback = [&](int err, int64_t _deleted_count, internal_info &&_info) {
        ret = err;
        deleted_count = _deleted_count;
        if (info != nullptr)
            (*info) = std::move(_info);
        op_completed.notify();
    };
    async_multi_del(hash_key, sort_keys, std::move(callback), timeout_milliseconds);
    op_completed.wait();
    return ret;
}

void pegasus_client_impl::async_multi_del(const std::string &hash_key,
                                          const std::set<std::string> &sort_keys,
                                          async_multi_del_callback_t &&callback,
                                          int timeout_milliseconds)
{
    // check params
    if (hash_key.size() == 0) {
        derror("invalid hash key: hash key should not be empty for multi_del");
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, 0, internal_info());
        return;
    }
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        if (callback != nullptr)
            callback(PERR_INVALID_HASH_KEY, 0, internal_info());
        return;
    }
    if (sort_keys.empty()) {
        derror("invalid sort keys: should not be empty");
        if (callback != nullptr)
            callback(PERR_INVALID_VALUE, 0, internal_info());
        return;
    }

    ::dsn::apps::multi_remove_request req;
    req.hash_key = ::dsn::blob(hash_key.data(), 0, hash_key.size());
    for (auto &sort_key : sort_keys) {
        req.sort_keys.emplace_back(sort_key.data(), 0, sort_key.size());
    }

    ::dsn::blob tmp_key;
    pegasus_generate_key(tmp_key, req.hash_key, ::dsn::blob());
    auto partition_hash = pegasus_key_hash(tmp_key);

    auto new_callback = [user_callback = std::move(callback)](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        if (user_callback == nullptr) {
            err.end_tracking();
            return;
        }
        ::dsn::apps::multi_remove_response response;
        internal_info info;
        int64_t deleted_count = 0;
        if (err == ::dsn::ERR_OK) {
            ::dsn::unmarshall(resp, response);
            info.app_id = response.app_id;
            info.partition_index = response.partition_index;
            info.decree = response.decree;
            info.server = response.server;
            deleted_count = response.count;
        }
        int ret =
            get_client_error(err == ERR_OK ? get_rocksdb_server_error(response.error) : err.get());
        user_callback(ret, deleted_count, std::move(info));
    };
    _client->multi_remove(req,
                          std::move(new_callback),
                          std::chrono::milliseconds(timeout_milliseconds),
                          0,
                          partition_hash);
}

int pegasus_client_impl::ttl(const std::string &hash_key,
                             const std::string &sort_key,
                             int &ttl_seconds,
                             int timeout_milliseconds,
                             internal_info *info)
{
    // check params
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        return PERR_INVALID_HASH_KEY;
    }

    ::dsn::blob req;
    pegasus_generate_key(req, hash_key, sort_key);
    auto partition_hash = pegasus_key_hash(req);
    auto pr =
        _client->ttl_sync(req, std::chrono::milliseconds(timeout_milliseconds), 0, partition_hash);
    if (pr.first == ERR_OK && pr.second.error == 0) {
        ttl_seconds = pr.second.ttl_seconds;
    }
    if (info != nullptr) {
        if (pr.first == ERR_OK) {
            info->app_id = pr.second.app_id;
            info->partition_index = pr.second.partition_index;
            info->decree = -1;
            info->server = pr.second.server;
        } else {
            info->app_id = -1;
            info->partition_index = -1;
            info->decree = -1;
        }
    }
    return get_client_error(pr.first == ERR_OK ? get_rocksdb_server_error(pr.second.error)
                                               : pr.first.get());
}

void pegasus_client_impl::async_get_scanner(const std::string &hash_key,
                                            const std::string &start_sortkey,
                                            const std::string &stop_sortkey,
                                            const scan_options &options,
                                            async_get_scanner_callback_t &&callback)
{
    if (callback) {
        pegasus_scanner *scanner;
        int ret = get_scanner(hash_key, start_sortkey, stop_sortkey, options, scanner);
        callback(ret, scanner);
    }
}

int pegasus_client_impl::get_scanner(const std::string &hash_key,
                                     const std::string &start_sort_key,
                                     const std::string &stop_sort_key,
                                     const scan_options &options,
                                     pegasus_scanner *&scanner)
{
    // check params
    if (hash_key.size() >= UINT16_MAX) {
        derror("invalid hash key: hash key length should be less than UINT16_MAX, but %d",
               (int)hash_key.size());
        return PERR_INVALID_HASH_KEY;
    }
    if (hash_key.empty()) {
        derror("invalid hash key: hash key cannot be empty when scan");
        return PERR_INVALID_HASH_KEY;
    }

    ::dsn::blob start;
    ::dsn::blob stop;
    scan_options o(options);
    pegasus_generate_key(start, hash_key, start_sort_key);
    if (stop_sort_key.empty()) {
        pegasus_generate_next_blob(stop, hash_key);
        o.stop_inclusive = false;
    } else {
        pegasus_generate_key(stop, hash_key, stop_sort_key);
    }

    std::vector<uint64_t> v;
    int cmp = memcmp(start.data(), stop.data(), std::min(start.length(), stop.length()));
    if (cmp < 0 || (cmp == 0 && start.length() < stop.length()) || // start < stop
        (cmp == 0 && start.length() == stop.length() && o.start_inclusive &&
         o.stop_inclusive)) // start == end and bounds are inclusive
    {
        v.push_back(pegasus_key_hash(start));
    }
    scanner = new pegasus_scanner_impl(_client, std::move(v), o, start, stop);

    return PERR_OK;
}

DEFINE_TASK_CODE_RPC(RPC_CM_QUERY_PARTITION_CONFIG_BY_INDEX,
                     TASK_PRIORITY_COMMON,
                     ::dsn::THREAD_POOL_DEFAULT)
void pegasus_client_impl::async_get_unordered_scanners(
    int max_split_count,
    const scan_options &options,
    async_get_unordered_scanners_callback_t &&callback)
{
    if (!callback) {
        return;
    }

    // check params
    if (max_split_count <= 0) {
        derror("invalid max_split_count: which should be greater than 0, but %d", max_split_count);
        callback(PERR_INVALID_SPLIT_COUNT, std::vector<pegasus_scanner *>());
        return;
    }

    auto new_callback = [ user_callback = std::move(callback), max_split_count, options, this ](
        ::dsn::error_code err, dsn_message_t req, dsn_message_t resp)
    {
        std::vector<pegasus_scanner *> scanners;
        configuration_query_by_index_response response;
        if (err == ERR_OK) {
            ::dsn::unmarshall(resp, response);
            if (response.err == ERR_OK) {
                unsigned int count = response.partition_count;
                int split = count < max_split_count ? count : max_split_count;
                scanners.resize(split);

                int size = count / split;
                int more = count - size * split;

                // use default value for other fields in scan_options
                scan_options opt;
                opt.timeout_ms = options.timeout_ms;
                opt.batch_size = options.batch_size;
                opt.snapshot = options.snapshot;
                for (int i = 0; i < split; i++) {
                    int s = size + (i < more);
                    std::vector<uint64_t> hash(s);
                    for (int j = 0; j < s; j++)
                        hash[j] = --count;
                    scanners[i] = new pegasus_scanner_impl(_client, std::move(hash), opt);
                }
            }
        }
        int ret = get_client_error(err == ERR_OK ? response.err.get() : err.get());
        user_callback(ret, std::move(scanners));
    };

    configuration_query_by_index_request req;
    req.app_name = _app_name;
    ::dsn::rpc::call(_meta_server,
                     RPC_CM_QUERY_PARTITION_CONFIG_BY_INDEX,
                     req,
                     nullptr,
                     new_callback,
                     std::chrono::milliseconds(options.timeout_ms),
                     0,
                     0);
}

int pegasus_client_impl::get_unordered_scanners(int max_split_count,
                                                const scan_options &options,
                                                std::vector<pegasus_scanner *> &scanners)
{
    ::dsn::utils::notify_event op_completed;
    int ret = -1;
    auto callback = [&](int err, std::vector<pegasus_scanner *> &&ss) {
        ret = err;
        scanners = std::move(ss);
        op_completed.notify();
    };
    async_get_unordered_scanners(max_split_count, options, std::move(callback));
    op_completed.wait();
    return ret;
}

const char *pegasus_client_impl::get_error_string(int error_code) const
{
    auto it = _client_error_to_string.find(error_code);
    dassert(
        it != _client_error_to_string.end(), "client error %d have no error string", error_code);
    return it->second.c_str();
}

/*static*/ void pegasus_client_impl::init_error()
{
    _client_error_to_string.clear();
#define PEGASUS_ERR_CODE(x, y, z) _client_error_to_string[y] = z
#include <pegasus/error_def.h>
#undef PEGASUS_ERR_CODE

    _server_error_to_client.clear();
    _server_error_to_client[::dsn::ERR_OK] = PERR_OK;
    _server_error_to_client[::dsn::ERR_TIMEOUT] = PERR_TIMEOUT;
    _server_error_to_client[::dsn::ERR_FILE_OPERATION_FAILED] = PERR_SERVER_INTERNAL_ERROR;
    _server_error_to_client[::dsn::ERR_INVALID_STATE] = PERR_SERVER_CHANGED;
    _server_error_to_client[::dsn::ERR_OBJECT_NOT_FOUND] = PERR_OBJECT_NOT_FOUND;
    _server_error_to_client[::dsn::ERR_NETWORK_FAILURE] = PERR_NETWORK_FAILURE;
    _server_error_to_client[::dsn::ERR_HANDLER_NOT_FOUND] = PERR_HANDLER_NOT_FOUND;

    _server_error_to_client[::dsn::ERR_APP_NOT_EXIST] = PERR_APP_NOT_EXIST;
    _server_error_to_client[::dsn::ERR_APP_EXIST] = PERR_APP_EXIST;

    // rocksdb error;
    for (int i = 1001; i < 1013; i++) {
        _server_error_to_client[-i] = -i;
    }
}

/*static*/ int pegasus_client_impl::get_client_error(int server_error)
{
    auto it = _server_error_to_client.find(server_error);
    if (it != _server_error_to_client.end())
        return it->second;
    derror("can't find corresponding client error definition, server error:[%d:%s]",
           server_error,
           ::dsn::error_code(server_error).to_string());
    return PERR_UNKNOWN;
}

/*static*/ int pegasus_client_impl::get_rocksdb_server_error(int rocskdb_error)
{
    return (rocskdb_error == 0) ? 0 : ROCSKDB_ERROR_START - rocskdb_error;
}
}
} // namespace