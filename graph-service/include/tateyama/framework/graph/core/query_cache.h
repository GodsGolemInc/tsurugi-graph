#pragma once

#include <tateyama/framework/graph/core/ast.h>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <list>

namespace tateyama::framework::graph::core {

/**
 * @brief LRU cache for parsed Cypher query ASTs (ADR-0003).
 *
 * Thread-safe: all public methods are guarded by a mutex.
 * Keyed by exact query string; parameterized queries with different
 * literal values will not cache-hit.
 */
class query_cache {
public:
    explicit query_cache(size_t max_size = 1024) : max_size_(max_size) {}

    /**
     * @brief Look up a cached statement for the given query string.
     * @return shared_ptr to the cached statement, or nullptr on cache miss.
     */
    std::shared_ptr<statement> get(const std::string& query) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(query);
        if (it == cache_.end()) return nullptr;

        // Move to front of LRU list
        lru_order_.erase(lru_map_.at(query));
        lru_order_.push_front(query);
        lru_map_[query] = lru_order_.begin();

        return it->second;
    }

    /**
     * @brief Store a parsed statement in the cache.
     */
    void put(const std::string& query, std::shared_ptr<statement> stmt) {
        std::lock_guard<std::mutex> lock(mutex_);

        // If already cached, update and move to front
        auto it = cache_.find(query);
        if (it != cache_.end()) {
            it->second = std::move(stmt);
            lru_order_.erase(lru_map_[query]);
            lru_order_.push_front(query);
            lru_map_[query] = lru_order_.begin();
            return;
        }

        // Evict LRU entry if at capacity
        if (cache_.size() >= max_size_) {
            const auto& lru_key = lru_order_.back();
            cache_.erase(lru_key);
            lru_map_.erase(lru_key);
            lru_order_.pop_back();
        }

        cache_[query] = std::move(stmt);
        lru_order_.push_front(query);
        lru_map_[query] = lru_order_.begin();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_order_.clear();
        lru_map_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

private:
    size_t max_size_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<statement>> cache_;
    mutable std::list<std::string> lru_order_;
    mutable std::unordered_map<std::string, std::list<std::string>::iterator> lru_map_;
};

} // namespace tateyama::framework::graph::core
