#include <tateyama/framework/graph/resource.h>

#include <glog/logging.h>
#include <tateyama/framework/repository.h>
#include <tateyama/framework/transactional_kvs_resource.h>
#include <sharksfin/api.h>

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

    db_handle_ = kvs_resource->core_object();
    if (!db_handle_) {
        LOG(ERROR) << "Database handle is null";
        return false;
    }

    // Initialize graph storages using a dedicated transaction
    sharksfin::TransactionOptions tx_opt{};
    sharksfin::TransactionControlHandle tx_ctl{};
    auto rc = sharksfin::transaction_begin(db_handle_, tx_opt, &tx_ctl);
    if (rc != sharksfin::StatusCode::OK) {
        LOG(ERROR) << "Failed to begin initialization transaction";
        return false;
    }

    // Get data-path handle from control handle
    sharksfin::TransactionHandle tx{};
    rc = sharksfin::transaction_borrow_handle(tx_ctl, &tx);
    if (rc != sharksfin::StatusCode::OK) {
        LOG(ERROR) << "Failed to borrow transaction handle";
        sharksfin::transaction_abort(tx_ctl);
        sharksfin::transaction_dispose(tx_ctl);
        return false;
    }

    if (!storage_->init(db_handle_, tx)) {
        LOG(ERROR) << "Failed to initialize graph storages";
        sharksfin::transaction_abort(tx_ctl);
        sharksfin::transaction_dispose(tx_ctl);
        return false;
    }

    rc = sharksfin::transaction_commit(tx_ctl);
    sharksfin::transaction_dispose(tx_ctl);

    if (rc != sharksfin::StatusCode::OK) {
        LOG(ERROR) << "Failed to commit initialization transaction";
        return false;
    }

    initialized_ = true;
    LOG(INFO) << "Graph resource started - storages initialized";
    return true;
}

bool resource::shutdown(framework::environment&) {
    initialized_ = false;
    LOG(INFO) << "Graph resource shutting down";
    return true;
}

} // namespace tateyama::framework::graph
