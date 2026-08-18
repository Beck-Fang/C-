#ifndef KARCCACHE_H_
#define KARCCACHE_H_

#include "kcache/CachePolicy.h"
#include "kcache/KArcLruPart.h"
#include "kcache/KArcLfuPart.h"
#include <memory>

namespace kcache 
{

template<typename Key, typename Value>
class KArcCache : public CachePolicy<Key, Value> 
{
    using EvictedFunc = std::function<void(Key, Value)>;
    using ValueOptional = std::optional<Value>;
public:
    explicit KArcCache(size_t capacity = 10, size_t transformThreshold = 2, const EvictedFunc& evicted_func = nullptr)
        : capacity_(capacity)
        , transformThreshold_(transformThreshold)
        , lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold, evicted_func))
        , lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity, transformThreshold, evicted_func))
    {}

    ~KArcCache() override = default;

    bool Set(Key &key, Value value) override 
    {
        checkGhostCaches(key);

        // 检查 LFU 部分是否存在该键
        bool inLfu = lfuPart_->contain(key);
        // 更新 LRU 部分缓存
        lruPart_->Set(key, value);
        // 如果 LFU 部分存在该键，则更新 LFU 部分
        if (inLfu) 
        {
            lfuPart_->Set(key, value);
        }
        return true;
    }

    bool Get(Key& key, Value& value) override 
    {
        checkGhostCaches(key);

        bool shouldTransform = false;
        if (lruPart_->Get(key, value, shouldTransform)) 
        {
            if (shouldTransform) 
            {
                lfuPart_->Set(key, value);
            }
            return true;
        }
        return lfuPart_->Get(key, value);
    }

    ValueOptional Get(Key& key) override 
    {
        Value value{};
        if(Get(key, value))
            return value;
        return std::nullopt;
    }

    bool Delete(Key& key)override
    {
        lfuPart_->Delete(key);

        lruPart_->Delete(key);

        return true;
    }

private:
    bool checkGhostCaches(Key key) 
    {
        bool inGhost = false;
        if (lruPart_->checkGhost(key)) 
        {
            if (lfuPart_->decreaseCapacity()) 
            {
                lruPart_->increaseCapacity();
            }
            inGhost = true;
        } 
        else if (lfuPart_->checkGhost(key)) 
        {
            if (lruPart_->decreaseCapacity()) 
            {
                lfuPart_->increaseCapacity();
            }
            inGhost = true;
        }
        return inGhost;
    }

private:
    size_t capacity_;
    size_t transformThreshold_;
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;
};

} 

#endif