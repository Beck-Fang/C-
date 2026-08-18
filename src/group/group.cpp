#include "kcache/group.h"

namespace kcache {

// 全局map与互斥锁，只在cpp中实例化一次
std::unordered_map<std::string, std::shared_ptr<KCacheGroup<std::string,std::string>>> cache_groups;
std::mutex mtx;

}