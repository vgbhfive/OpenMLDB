// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef RTIDB_LOG_WRITER_H
#define RTIDB_LOG_WRITER_H

#include <stdint.h>
#include "log/log_format.h"
#include "log/writable_file.h"
#include "base/slice.h"
#include "base/status.h"

using ::rtidb::base::Slice;
using ::rtidb::base::Status;

namespace rtidb {
namespace log {

class Writer {

public:
    // Create a writer that will append data to "*dest".
    // "*dest" must be initially empty.
    // "*dest" must remain live while this Writer is in use.
    explicit Writer(WritableFile* dest);

    // Create a writer that will append data to "*dest".
    // "*dest" must have initial length "dest_length".
    // "*dest" must remain live while this Writer is in use.
    Writer(WritableFile* dest, uint64_t dest_length);

    ~Writer();

    Status AddRecord(const Slice& slice);
    Status EndLog();

private:
    WritableFile* dest_;
    int block_offset_;       // Current offset in block

    // crc32c values for all supported record types.  These are
    // pre-computed to reduce the overhead of computing the crc of the
    // record type stored in the header.
    uint32_t type_crc_[kMaxRecordType + 1];

    Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

    // No copying allowed
    Writer(const Writer&);
    void operator=(const Writer&);
};

struct WriteHandle {
    FILE* fd_;
    WritableFile* wf_;
    Writer* lw_;
    WriteHandle(const std::string& fname, FILE* fd):fd_(fd),
    wf_(NULL), lw_(NULL) {
        wf_ = ::rtidb::log::NewWritableFile(fname, fd);
        lw_ = new Writer(wf_);
    }

    ::rtidb::base::Status Write(const ::rtidb::base::Slice& slice) {
        return lw_->AddRecord(slice);
    }

    ::rtidb::base::Status Sync() {
        return wf_->Sync(); 
    }

    ::rtidb::base::Status EndLog() {
        return lw_->EndLog();
    }

    uint64_t GetSize() {
        return wf_->GetSize();
    }

    ~WriteHandle() {
        delete lw_;
        delete wf_;
    }
};


}  // namespace log
}  // namespace rtidb

#endif  // RTIDB_LOG_WRITER_H