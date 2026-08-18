#ifndef KARCLFUPART_H
#define KARCLFUPART_H

#include "kcache/KArcCacheNode.h"
#include <unordered_map>
#include <map>
#include <mutex>

namespace kcache 
{

template<typename Key, typename Value>
class ArcLfuPart 
{
    using EvictedFunc = std::function<void(Key, Value)>;
    using ValueOptional = std::optional<Value>;
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqMap = std::map<size_t, std::list<NodePtr>>;

    explicit ArcLfuPart(size_t capacity, size_t transformThreshold, const EvictedFunc& evicted_func = nullptr)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
        , minFreq_(0)
        , evicted_func_(evicted_func)
    {
        initializeLists();
    }

    bool Set(Key& key, Value value) 
    {
        if (capacity_ == 0) 
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);
    }

    bool Get(Key& key, Value& value) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            updateNodeFrequency(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    bool Delete(Key& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if(it != mainCache_.end())
        {
            size_t oldFreq = it->second->getAccessCount();

            // 从旧频率列表中移除
            auto& oldList = freqMap_[oldFreq];
            oldList.remove(it->second);
            if (oldList.empty()) 
            {
                freqMap_.erase(oldFreq);
            }
            mainCache_.erase(it);

            if(this->evicted_func_ != nullptr)
                this->evicted_func_(key,it->second->getValue());
        }
        auto ghost_it = ghostCache_.find(key);
        if(ghost_it != ghostCache_.end())
        {
            removeFromGhost(ghost_it->second);
            ghostCache_.erase(ghost_it);
        }

        return true;
    }

    bool contain(Key key)
    {
        return mainCache_.find(key) != mainCache_.end();
    }

    bool checkGhost(Key key) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) 
        {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    void increaseCapacity() { ++capacity_; }
    
    bool decreaseCapacity() 
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) 
        {
            evictLeastFrequent();
        }
        --capacity_;
        return true;
    }

private:
    void initializeLists() 
    {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        updateNodeFrequency(node);
        return true;
    }

    bool addNewNode(const Key& key, const Value& value) 
    {
        if (mainCache_.size() >= capacity_) 
        {
            evictLeastFrequent();
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;
        
        // 将新节点添加到频率为1的列表中
        if (freqMap_.find(1) == freqMap_.end()) 
        {
            freqMap_[1] = std::list<NodePtr>();
        }
        freqMap_[1].push_back(newNode);
        minFreq_ = 1;
        
        return true;
    }

    void updateNodeFrequency(NodePtr node) 
    {
        size_t oldFreq = node->getAccessCount();
        node->incrementAccessCount();