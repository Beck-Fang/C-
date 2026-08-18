#ifndef KLRUCACHE_H_
#define KLRUCACHE_H_

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "kcache/CachePolicy.h"

namespace kcache
{

// 前向声明
template<typename Key, typename Value> class KLruCache;

template<typename Key, typename Value>
class LruNode 
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;  // 访问次数
    std::weak_ptr<LruNode<Key, Value>> prev_;  // 改为weak_ptr打破循环引用
    std::shared_ptr<LruNode<Key, Value>> next_;

public:
    LruNode(Key key, Value value)
        : key_(key)
        , value_(value)
        , accessCount_(1) 
    {}

    // 提供必要的访问器
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    void setValue(const Value& value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }

    friend class KLruCache<Key, Value>;
};


template<typename Key, typename Value>
class KLruCache : public CachePolicy<Key, Value>
{
    using EvictedFunc = std::function<void(Key, Value)>;
    using ValueOptional = std::optional<Value>;

public:
    using LruNodeType = LruNode<Key, Value>;
    using NodePtr = std::shared_ptr<LruNodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    KLruCache() = delete;

    explicit KLruCache(int capacity, const EvictedFunc &evicted_func = nullptr)
        : capacity_(capacity), evicted_func_(evicted_func)
    {
        initializeList();
    }

    ~KLruCache() override = default;

    // 添加缓存
    bool Set(Key &key, Value value) override
    {
        if (capacity_ <= 0)
            return false;
    
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 如果在当前容器中,则更新value,并调用get方法，代表该数据刚被访问
            updateExistingNode(it->second, value);
            return true;
        }
        
        addNewNode(key, value);
        return true;
    }

    bool Get(Key &key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            moveToMostRecent(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    ValueOptional Get(Key &key) override
    {
        Value value{};
        // memset(&value, 0, sizeof(value));   // memset 是按字节设置内存的，对于复杂类型（如 string）使用 memset 可能会破坏对象的内部结构
        if(Get(key, value))
            return value;
        return std::nullopt;
    }

    bool Delete(Key& key) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            NodePtr Recent = it->second;
            removeNode(Recent);
            nodeMap_.erase(Recent->getKey());
            if (this->evicted_func_)
                this->evicted_func_(Recent->getKey(), Recent->getValue());
        }
        return true;
    }

private:
    void initializeList()
    {
        // 创建首尾虚拟节点
        dummyHead_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyTail_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    void updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToMostRecent(node);
    }

    void addNewNode(const Key& key, const Value& value) 
    {
       if (nodeMap_.size() >= capacity_) 
       {
           evictLeastRecent();
       }

       NodePtr newNode = std::make_shared<LruNodeType>(key, value);
       insertNode(newNode);
       nodeMap_[key] = newNode;
    }

    // 将该节点移动到最新的位置
    void moveToMostRecent(NodePtr node) 
    {
        removeNode(node);
        insertNode(node);
    }

    void removeNode(NodePtr node) 
    {
        if(!node->prev_.expired() && node->next_) 
        {
            auto prev = node->prev_.lock(); // 使用lock()获取shared_ptr
            prev->next_ = node->next_;
            node->next_->prev_ = prev;
            node->next_ = nullptr; // 清空next_指针，彻底断开节点与链表的连接
        }
    }

    // 从尾部插入结点
    void insertNode(NodePtr node) 
    {
        node->next_ = dummyTail_;
        node->prev_ = dummyTail_->prev_;
        dummyTail_->prev_.lock()->next_ = node; // 使用lock()获取shared_ptr
        dummyTail_->prev_ = node;
    }

    // 驱逐最近最少访问
    void evictLeastRecent() 
    {
        NodePtr leastRecent = dummyHead_->next_;
        removeNode(leastRecent);
        nodeMap_.erase(leastRecent->getKey());
        if(this->evicted_func_)
            this->evicted_func_(leastRecent->getKey(),leastRecent->getValue());
    }

private:
    int           capacity_; // 缓存容量
    NodeMap       nodeMap_; // key -> Node 
    std::mutex    mutex_;
    NodePtr       dummyHead_; // 虚拟头结点
    NodePtr       dummyTail_;
    EvictedFunc evicted_func_; //删除缓存条目后的钩子
};

// LRU优化：Lru-k版本。
template<typename Key, typename Value>
class KLruKCache : public CachePolicy<Key, Value>
{
    using EvictedFunc = std::function<void(Key, Value)>;
    using ValueOptional = std::optional<Value>;

public:
    KLruKCache() = delete;

    explicit KLruKCache(int capacity, int historyCapacity, int k = 2, const EvictedFunc &evicted_func = nullptr)
        : lruCache(std::make_unique<KLruCache<Key, Value>>(capacity, evicted_func))
        , historyList_(std::make_unique<KLruCache<Key, std::pair<size_t,Value>>>(historyCapacity))
        , k_(k)
    {}

    bool Get(Key &key, Value &value)override 
    {
        std::lock_guard<std::mutex>lock(mutex_);

        // 首先尝试从主缓存获取数据
        bool inMainCache = lruCache->Get(key, value);

        // 获取并更新访问历史计数
        std::pair<size_t, Value>history{ 0,Value{} };
        if (historyList_->Get(key, history))
        {
            history.first++;
            historyList_->Set(key, history);
        }

        // 如果数据在主缓存中，直接返回
        if (inMainCache) 
        {
            return true;
        }

        // 如果数据不在主缓存，但访问次数达到了k次(k必须大于0)
        if (history.first >= k_) 
        {
                // 有历史值，将其添加到主缓存
                value = history.second;
                
                // 从历史记录移除
                historyList_->Delete(key);
                
                // 添加到主缓存
                lruCache->Set(key, value);
                
                return true;
        }

        // 数据不在主缓存且不满足添加条件，返回默认值
        return false;
    }

    ValueOptional Get(Key& key)override
    {
        Value value;
        //memset(&value, 0, sizeof(value));
        if(Get(key, value))
            return value;
        return std::nullopt;
    }

    bool Set(Key &key, Value value) override
    {
        std::lock_guard<std::mutex>lock(mutex_);

        // 检查是否已在主缓存
        Value existingValue{};
        bool inMainCache = lruCache->Get(key, existingValue);
        
        if (inMainCache) 
        {
            // 已在主缓存，直接更新
            lruCache->Set(key, value);
            return true;
        }
        
        // 获取并更新访问历史
        std::pair<size_t, Value>history{ 0,Value{} };
        if (historyList_->Get(key, history))
        {
            history.first++;
            historyList_->Set(key, history);
        }
        
        // 检查是否达到k次访问阈值
        if (history.first >= k_) 
        {
            // 达到阈值，添加到主缓存
            historyList_->Delete(key);
            lruCache->Set(key, value);
        }
        return true;
    }

    bool Delete(Key& key)override
    {
        std::lock_guard<std::mutex>lock(mutex_);
        // 检查是否已在主缓存和历史记录中
        Value existingValue{};
        
        if (lruCache->Get(key, existingValue))
        {
            lruCache->Delete(key);
        }

        std::pair<size_t, Value>history{ 0,Value{} };
        if (historyList_->Get(key, history))
        {
            historyList_->Delete(key);
        }
        return true;
    }

private:
    std::mutex                              mutex_;//互斥锁
    int                                     k_; // 进入缓存队列的评判标准,必须大于0
    std::unique_ptr<KLruCache<Key, std::pair<size_t,Value>>> historyList_; // 访问数据历史记录(value为访问次数)
    std::unique_ptr<KLruCache<Key, Value>>  lruCache;//真正的LRU缓存
};

// lru优化：对lru进行分片，提高高并发使用的性能
template<typename Key, typename Value>
class KHashLruCache : public CachePolicy<Key,Value>
{
    using EvictedFunc = std::function<void(Key, Value)>;
    using ValueOptional = std::optional<Value>;

public:
    KHashLruCache() = delete;

    explicit KHashLruCache(size_t capacity, int sliceNum = -1, const EvictedFunc& evicted_func = nullptr)
        : capacity_(capacity)
        , sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
    {
        size_t sliceSize = std::ceil(capacity / static_cast<double>(sliceNum_)); // 获取每个分片的大小
        for (int i = 0; i < sliceNum_; ++i)
        {
            lruSliceCaches_.emplace_back(std::make_unique<KLruCache<Key, Value>>(sliceSize, evicted_func)); 
        }
    }

    bool Set(Key &key, Value value)override
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->Set(key, value);
        return true;
    }

    bool Get(Key &key, Value& value)override
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->Get(key, value);
    }

    ValueOptional Get(Key &key)override
    {
        Value value;
        //memset(&value, 0, sizeof(value));
        if(Get(key, value))
            return value;   
        return std::nullopt;
    }

    bool Delete(Key& key)override
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->Delete(key);
        return true;
    }

private:
    // 将key转换为对应hash值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t                                              capacity_;  // 总容量
    int                                                 sliceNum_;  // 切片数量
    std::vector<std::unique_ptr<KLruCache<Key, Value>>> lruSliceCaches_; // 切片LRU缓存
};
}

#endif