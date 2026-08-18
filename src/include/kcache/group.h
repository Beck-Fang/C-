#ifndef GROUP_H_
#define GROUP_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <fmt/base.h>
#include <spdlog/spdlog.h>

#include "kcache/cache.h"
#include "kcache/singleflight.h"

namespace kcache {

struct GroupStatus {
    std::atomic_int64_t loads;          // 加载次数
    std::atomic_int64_t local_hits;        // 本地缓存命中次数
    std::atomic_int64_t local_misses;      // 本地缓存未命中次数
    std::atomic_int64_t getter_hits;       // 从数据源获取成功次数
    std::atomic_int64_t getter_misses;     // 从数据源获取失败次数
    std::atomic_int64_t load_duration;  // 加载总耗时（纳秒
};

template<typename Key, typename Value> class KCacheGroup;

template<typename Key, typename Value>
using Getter = std::function<std::optional<Value>(const Key&)>;

extern std::unordered_map<std::string, std::shared_ptr<KCacheGroup<std::string,std::string>>> cache_groups;
extern std::mutex mtx;

// 自身不需加锁，无数据竞争，CachePolicy和SingleFlight内部加锁，并发安全
template<typename Key, typename Value>
class KCacheGroup {
    using ValueOptional = std::optional<Value>;

public:
    KCacheGroup() = default;

    KCacheGroup(std::string& name, std::unique_ptr<CachePolicy<Key,Value>>cache, const Getter<Key, Value> &getter)
        : cache_(std::move(cache)), name_(name), getter_(getter) {}

    KCacheGroup(const KCacheGroup&) = delete;

    auto operator=(const KCacheGroup& other) -> KCacheGroup& = delete;

    KCacheGroup(KCacheGroup&& other) = delete;

    auto operator=(KCacheGroup&& other) -> KCacheGroup& = delete;

    auto Get(Key& key) -> ValueOptional
    {
            if (is_close_) {
            spdlog::error("Cache group [{}] is closed!!!", name_);
            return std::nullopt;
        }

        // 先从本地缓存中获取
        auto ret = cache_->Get(key);
        if (ret) {
            // ++status_.local_hits;  // 本地命中缓存次数+1
            return ret;
        }

        // ++status_.local_misses;  // 本地未命中缓存次数+1
        return Load(key);
    };

    bool Set(Key& key, Value value)
    {
        if (is_close_) {
            spdlog::error("Cache group [{}] is closed!!!", name_);
            return false;
        }
        cache_->Set(key, value);
        spdlog::debug("key:{} is set", key);
        return true;
    };

    bool Delete(Key& key)
    {
        if (is_close_) {
            spdlog::error("Cache group [{}] is closed!!!", name_);
            return false;
        }
        cache_->Delete(key);
        spdlog::debug("key:{} is deleted", key);
        return true;
    };

private:
    auto Load(Key& key) -> ValueOptional
    {
        auto ret = loader_.Do(key, [&] { return LoadData(key); });
        if (!ret) {
            spdlog::info(">_< Uh oh, there is not found: [{}]", key);
            return std::nullopt;
        }
        cache_->Set(key, ret.value());
        // TODO 记录加载时间
        return ret;
    };
    auto LoadData(Key& key) -> ValueOptional
    {
        spdlog::info("Try to load key [{}] from local", key);
        // 通过getter从数据源获取
        auto val = getter_(key);
        if (!val) {
            // ++status_.getter_misses;    //从数据源获取失败次数+1
            return std::nullopt;
        }
        // ++status_.getter_hits;  //从数据源获取成功次数+1
        return val;
    };

private:
    std::unique_ptr<CachePolicy<Key,Value>> cache_;
    std::string name_;
    std::atomic<bool> is_close_{false};
    SingleFlight<Key,Value> loader_;
    // GroupStatus status_;  //统计数据
    Getter<Key, Value> getter_;
};

//使用该方法时必须确保cache和getter的类型一致
template<typename Key, typename Value>
auto MakeCacheGroup(const std::string& name, std::unique_ptr<CachePolicy<Key,Value>>cache, const Getter<Key, Value> &getter) -> std::shared_ptr<KCacheGroup<Key, Value>>
{
    if (getter == nullptr) {
        spdlog::critical("no getter function!");
        std::exit(1);
    }
    std::lock_guard lock{mtx};
    std::string name_ = name;
    cache_groups[name] = std::make_shared<KCacheGroup<Key,Value>>(name_, std::move(cache), getter);
    return cache_groups[name];
};

//需要在业务层正确判断缓存组的Key,Value类型，一种缓存组配套一个GetCCacheGroup方法。后续拓展功能
template<typename Key, typename Value>
auto GetCacheGroup(const std::string& name) -> std::shared_ptr<KCacheGroup<Key, Value>>
{
    std::lock_guard lock{mtx};
    if (cache_groups.find(name) == cache_groups.end()) {
        return nullptr;
    }
    return cache_groups[name];
};

}  // namespace kcache

#endif
