#include <tateyama/framework/graph/resource.h>

#include <glog/logging.h>
#include <tateyama/framework/repository.h>
#include <tateyama/framework/transactional_kvs_resource.h>

namespace tateyama::framework::graph {

bool resource::setup(framework::environment&) {
    storage_ = std::make_unique<graph::storage>();
    return true;
}

bool resource::start(framework::environment& env) {
    auto kvs_resource = env.resource_repository().find<framework::transactional_kvs_resource>();
    if (!kvs_resource) {
        LOG(ERROR) << "transactional_kvs_resource not found";
        return false;
    }

    auto db_handle = kvs_resource->core_object();
    // In real implementation, we need a transaction to init storages or use a background one.
    // For now, assume storage::init can work if given a handle if the implementation allows.
    // However, sharksfin::storage_create requires a TransactionHandle or DatabaseHandle.
    // Let's assume we can use a temporary transaction if needed.
    
    // For prototype, we skip init with a real transaction here or assume it's done lazily.
    // But we need to at least keep the db_handle.
    
    LOG(INFO) << "Graph resource started";
    return true;
}

bool resource::shutdown(framework::environment&) {
    LOG(INFO) << "Graph resource shutting down";
    return true;
}

} // namespace tateyama::framework::graph
