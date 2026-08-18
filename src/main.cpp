#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <optional>

#include <fmt/base.h>
#include <fmt/core.h>
#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <spdlog/spdlog.h>

#include "kcache/cache.h"
#include "kcache/group.h"
#include "kcache/server.h"

using namespace kcache;

DEFINE_int32(port, 8001, "节点端口");
DEFINE_string(node, "A", "节点标识符");
DEFINE_string(group, "default", "缓存组名称");
DEFINE_string(cache, "LRU", "缓存策略");
DEFINE_string(log_level, "info", "日志级别， 可选值：trace, debug, info, warn, error, critical");
DEFINE_string(etcd_endpoints, "http://127.0.0.1:2379", "etcd地址");

// 模拟数据库
std::unordered_map<std::string, std::string> db = {
    {"Tom", "400"},     {"Kerolt", "370"}, {"Jack", "296"}, {"Alice", "320"}, {"Bob", "280"},
    {"Charlie", "410"}, {"Diana", "390"},  {"Eve", "310"},  {"abcde", "789"}, {"hello", "879"}
};

std::function<void(int)> handler_wrapper;
void HandleCtrlC(int signum) { handler_wrapper(signum); }

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[knode][%^%l%$] %v");

    std::string addr = "localhost:" + std::to_string(FLAGS_port);
    std::string service_name = "kcache";
    spdlog::info("[node{}] start at: {}", FLAGS_node, addr);

    try {
        // 创建节点，同时注册到etcd
        ServerOptions opts;
        opts.etcd_endpoints = {FLAGS_etcd_endpoints};
        auto node = std::make_unique<KCacheServer>(addr, service_name, opts);
        spdlog::info("[node{}] server created successfully", FLAGS_node);

        // 启动节点
        std::thread server_thread{[&] {
            spdlog::info("[node{}] starting service...", FLAGS_node);
            try {
                node->Start();
            } catch (const std::exception& e) {
                spdlog::info("[node{}] failed to start service: {}", FLAGS_node, e.what());
                std::exit(1);
            }
        }};

        // 注册 Ctrl+C 信号处理器用来优雅关闭服务
        handler_wrapper = [&](int signal) {
            if (signal == SIGINT) {
                spdlog::info("[node{}] received Ctrl+C signal, shutting down service...", FLAGS_node);
                if (node) {
                    node->Stop();
                }
                spdlog::info("[node{}] service stopped", FLAGS_node);
                // 不直接 exit，而是要等其他工作线程完成清理工作
            }
        };
        signal(SIGINT, HandleCtrlC);

        std::this_thread::sleep_for(std::chrono::seconds(5));  // 等待服务器启动

        // 创建缓存组。当前版本，缓存组方法的Key变量不能传入const类型和临时变量
        std::unique_ptr<CachePolicy<std::string,std::string>>cache;

        if(FLAGS_cache == "LRU-K"){
            cache = std::make_unique<KLruKCache<std::string,std::string>>(10000,10000);
            spdlog::info("using cachepolicy: LRU-K");
        } else if (FLAGS_cache == "HASH_LRU"){
            cache = std::make_unique<KHashLruCache<std::string,std::string>>(20000);
            spdlog::info("using cachepolicy: HASH_LRU");
        } else if (FLAGS_cache == "LFU"){
            cache = std::make_unique<KLfuCache<std::string,std::string>>(20000);
            spdlog::info("using cachepolicy: LFU");
        } else if (FLAGS_cache == "HASH_LFU"){
            cache = std::make_unique<KHashLfuCache<std::string,std::string>>(20000);
            spdlog::info("using cachepolicy: HASH-LFU");
        } else if (FLAGS_cache == "ARC"){
            cache = std::make_unique<KArcCache<std::string,std::string>>(20000);
            spdlog::info("using cachepolicy: ARC");
        } else {
            cache = std::make_unique<KLruCache<std::string,std::string>>(20000);//"LRU"
            spdlog::info("using cachepolicy: LRU");
        }

        MakeCacheGroup<std::string,std::string>(FLAGS_group, std::move(cache), [&](const std::string& key) -> std::optional<std::string> {
            if (db.find(key) != db.end()) {
                spdlog::info(">_< search [{}] from db\n", key);
                return db[key];
            }
            return std::nullopt;
        });

        spdlog::info("[node{}] service running, press Ctrl+C to exit...", FLAGS_node);

        // 等待服务器线程
        if (server_thread.joinable()) {
            server_thread.join();
        }

    } catch (const std::exception& e) {
        spdlog::error("[node{}] exception occurred: {}", FLAGS_node, e.what());
        std::exit(1);
    }

    return 0;
}