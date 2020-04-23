/*
 * engine.h
 * Copyright (C) 4paradigm.com 2019 wangtaize <wangtaize@4paradigm.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_VM_ENGINE_H_
#define SRC_VM_ENGINE_H_

#include <map>
#include <memory>
#include <mutex>  //NOLINT
#include <string>
#include <vector>
#include "base/raw_buffer.h"
#include "base/spin_lock.h"
#include "codec/list_iterator_codec.h"
#include "codec/row_codec.h"
#include "proto/common.pb.h"
#include "vm/catalog.h"
#include "vm/mem_catalog.h"
#include "vm/sql_compiler.h"

#include "llvm-c/Target.h"

namespace fesql {
namespace vm {

using ::fesql::codec::Row;
using ::fesql::codec::RowView;

class Engine;

class EngineOptions {
 public:
    void set_keep_ir(bool flag) {
        this->keep_ir_ = flag;
    }
    bool is_keep_ir() const {
        return this->keep_ir_;
    }
 private:
    bool keep_ir_;
};

class CompileInfo {
 public:
    SQLContext& get_sql_context() { return this->sql_ctx; }

    bool get_ir_buffer(const base::RawBuffer& buf) {
        auto& str = this->sql_ctx.ir;
        return buf.CopyFrom(str.data(), str.size());
    }

    size_t get_ir_size() {
        return this->sql_ctx.ir.size();
    }

 private:
    SQLContext sql_ctx;
};

class RunSession {
 public:
    RunSession();

    ~RunSession();

    virtual inline const Schema& GetSchema() const {
        return compile_info_->get_sql_context().schema;
    }
    virtual inline vm::PhysicalOpNode* GetPhysicalPlan() {
        return compile_info_->get_sql_context().plan;
    }
    virtual inline vm::Runner* GetRunner() {
        return compile_info_->get_sql_context().runner;
    }

    virtual inline std::shared_ptr<CompileInfo> GetCompileInfo() {
        return compile_info_;
    }

    virtual const bool IsBatchRun() const = 0;

    void EnableDebug() { is_debug_ = true; }
    void DisableDebug() { is_debug_ = false; }

 protected:
    inline void SetCompileInfo(
        const std::shared_ptr<CompileInfo>& compile_info) {
        compile_info_ = compile_info;
    }
    inline void SetCatalog(const std::shared_ptr<Catalog>& cl) { cl_ = cl; }
    std::shared_ptr<CompileInfo> compile_info_;
    std::shared_ptr<Catalog> cl_;
    bool is_debug_;
    friend Engine;
};

class BatchRunSession : public RunSession {
 public:
    explicit BatchRunSession(bool mini_batch = false)
        : RunSession(), mini_batch_(mini_batch) {}
    ~BatchRunSession() {}
    virtual int32_t Run(std::vector<int8_t*>& buf, uint64_t limit);  // NOLINT
    virtual std::shared_ptr<TableHandler> Run();                     // NOLINT
    const bool IsBatchRun() const override { return true; }

 private:
    const bool mini_batch_;
};

class RequestRunSession : public RunSession {
 public:
    RequestRunSession() : RunSession() {}
    ~RequestRunSession() {}
    virtual int32_t Run(const Row& in_row, Row* output);  // NOLINT
    const bool IsBatchRun() const override { return false; }
    std::shared_ptr<TableHandler> RunRequestPlan(const Row& request,
                                                 PhysicalOpNode* node);
};

typedef std::map<std::string,
                 std::map<std::string, std::shared_ptr<CompileInfo>>>
    EngineCache;
class Engine {
 public:
    Engine(const std::shared_ptr<Catalog>& cl, const EngineOptions& options);
    explicit Engine(const std::shared_ptr<Catalog>& cl);

    // Initialize LLVM environments
    static void InitializeGlobalLLVM();

    ~Engine();

    bool Get(const std::string& sql, const std::string& db,
             RunSession& session,    // NOLINT
             base::Status& status);  // NOLINT
 private:
    std::shared_ptr<CompileInfo> GetCacheLocked(const std::string& db,
                                                const std::string& sql);

    const std::shared_ptr<Catalog> cl_;
    EngineOptions options_;

    base::SpinMutex mu_;
    EngineCache cache_;
    ::fesql::node::NodeManager nm_;
};

}  // namespace vm
}  // namespace fesql
#endif  // SRC_VM_ENGINE_H_
