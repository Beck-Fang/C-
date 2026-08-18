#ifndef CACHEPOLICY_H_
#define CACHEPOLICY_H_

#include <functional>
#include <optional>

namespace kcache
{

template <typename Key, typename Value>
class CachePolicy
{
public:
    virtual ~CachePolicy() {};

    // 添加缓存接口
    virtual bool Set(Key &key, Value value) = 0;

    // key是传入参数  访问到的值以传出参数的形式返回 | 访问成功返回true
    virtual bool Get(Key &key, Value& value) = 0;
    // 如果缓存中能找到key，则直接返回value
    virtual std::optional<Value> Get(Key &key) = 0;
    //删除缓存
    virtual bool Delete(Key &key) = 0;

};

}

#endif