//
// snapshot.cc
// Copyright (C) 2017 4paradigm.com
// Author vagrant
// Date 2017-07-24
//
//
#include "storage/snapshot.h"

#include "base/file_util.h"
#include "base/strings.h"
#include "base/slice.h"
#include "base/count_down_latch.h"
#include "log/sequential_file.h"
#include "log/log_reader.h"
#include "proto/tablet.pb.h"
#include "gflags/gflags.h"
#include "logging.h"
#include "timer.h"
#include "thread_pool.h"
#include <unistd.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

using ::baidu::common::DEBUG;
using ::baidu::common::INFO;
using ::baidu::common::WARNING;

DECLARE_string(db_root_path);
DECLARE_uint64(gc_on_table_recover_count);

namespace rtidb {
namespace storage {

const std::string SNAPSHOT_SUBFIX=".sdb";
const std::string MANIFEST = "MANIFEST";
const uint32_t KEY_NUM_DISPLAY = 1000000;

Snapshot::Snapshot(uint32_t tid, uint32_t pid, LogParts* log_part):tid_(tid), pid_(pid),
     log_part_(log_part) {
    offset_ = 0;
}

Snapshot::~Snapshot() {
}

bool Snapshot::Init() {
    snapshot_path_ = FLAGS_db_root_path + "/" + std::to_string(tid_) + "_" + std::to_string(pid_) + "/snapshot/";
    log_path_ = FLAGS_db_root_path + "/" + std::to_string(tid_) + "_" + std::to_string(pid_) + "/binlog/";
    if (!::rtidb::base::MkdirRecur(snapshot_path_)) {
        PDLOG(WARNING, "fail to create db meta path %s", snapshot_path_.c_str());
        return false;
    }
    if (!::rtidb::base::MkdirRecur(log_path_)) {
        PDLOG(WARNING, "fail to create db meta path %s", log_path_.c_str());
        return false;
    }
    making_snapshot_.store(false, std::memory_order_release);
    return true;
}

bool Snapshot::Recover(std::shared_ptr<Table> table, uint64_t& latest_offset) {
    ::rtidb::api::Manifest manifest;
    manifest.set_offset(0);
    int ret = GetSnapshotRecord(manifest);
    if (ret == -1) {
        return false;
    }
    if (ret == 0) {
        RecoverFromSnapshot(manifest.name(), manifest.count(), table);
        latest_offset = manifest.offset();
        offset_ = latest_offset;
    }
    return RecoverFromBinlog(table, manifest.offset(), latest_offset);
}

bool Snapshot::RecoverFromBinlog(std::shared_ptr<Table> table, uint64_t offset,
                                 uint64_t& latest_offset) {
    PDLOG(INFO, "start recover table tid %u, pid %u from binlog with start offset %lu",
            table->GetId(), table->GetPid(), offset);
    ::rtidb::log::LogReader log_reader(log_part_, log_path_);
    log_reader.SetOffset(offset);
    ::rtidb::api::LogEntry entry;
    uint64_t cur_offset = offset;
    std::string buffer;
    uint64_t succ_cnt = 0;
    uint64_t failed_cnt = 0;
    uint64_t consumed = ::baidu::common::timer::now_time();
    while (true) {
        buffer.clear();
        ::rtidb::base::Slice record;
        ::rtidb::base::Status status = log_reader.ReadNextRecord(&record, &buffer);
        if (status.IsWaitRecord()) {
            consumed = ::baidu::common::timer::now_time() - consumed;
            PDLOG(INFO, "table tid %u pid %u completed, succ_cnt %lu, failed_cnt %lu, consumed %us",
                       tid_, pid_, succ_cnt, failed_cnt, consumed);
            break;
        }

        if (status.IsEof()) {
            continue;
        }

        if (!status.ok()) {
            failed_cnt++;
            continue;
        }
        bool ok = entry.ParseFromString(record.ToString());
        if (!ok) {
            PDLOG(WARNING, "fail parse record for tid %u, pid %u with value %s", tid_, pid_,
                        ::rtidb::base::DebugString(record.ToString()).c_str());
            failed_cnt++;
            continue;
        }

        if (cur_offset >= entry.log_index()) {
            PDLOG(DEBUG, "offset %lu has been made snapshot", entry.log_index());
            continue;
        }

        if (cur_offset + 1 != entry.log_index()) {
            PDLOG(WARNING, "missing log entry cur_offset %lu , new entry offset %lu for tid %u, pid %u",
                  cur_offset, entry.log_index(), tid_, pid_);
        }

        if (entry.dimensions_size() > 0) {
            table->Put(entry.ts(), entry.value(), entry.dimensions());
        }else {
            // the legend way
            table->Put(entry.pk(), entry.ts(), 
                       entry.value().c_str(), 
                       entry.value().size());
        }
        cur_offset = entry.log_index();
        succ_cnt++;
        if (succ_cnt % 100000 == 0) {
            PDLOG(INFO, "[Recover] load data from binlog succ_cnt %lu, failed_cnt %lu for tid %u, pid %u",
                    succ_cnt, failed_cnt, tid_, pid_);
        }
        if (succ_cnt % FLAGS_gc_on_table_recover_count == 0) {
            table->SchedGc();
        }
    }
    latest_offset = cur_offset;
    return true;
}

void Snapshot::RecoverFromSnapshot(const std::string& snapshot_name, uint64_t expect_cnt, std::shared_ptr<Table> table) {
    std::string full_path = snapshot_path_ + "/" + snapshot_name;
    std::atomic<uint64_t> g_succ_cnt(0);
    std::atomic<uint64_t> g_failed_cnt(0);
    RecoverSingleSnapshot(full_path, table, &g_succ_cnt, &g_failed_cnt);
    PDLOG(INFO, "[Recover] progress done stat: success count %lu, failed count %lu", 
                    g_succ_cnt.load(std::memory_order_relaxed),
                    g_failed_cnt.load(std::memory_order_relaxed));
    if (g_succ_cnt.load(std::memory_order_relaxed) != expect_cnt) {
        PDLOG(WARNING, "snapshot %s , expect cnt %lu but succ_cnt %lu", snapshot_name.c_str(),
                expect_cnt, g_succ_cnt.load(std::memory_order_relaxed));
    }
}


void Snapshot::RecoverSingleSnapshot(const std::string& path, std::shared_ptr<Table> table, 
                                     std::atomic<uint64_t>* g_succ_cnt,
                                     std::atomic<uint64_t>* g_failed_cnt) {
    do {
        if (table == NULL) {
            PDLOG(WARNING, "table input is NULL");
            break;
        }
        FILE* fd = fopen(path.c_str(), "rb");
        if (fd == NULL) {
            PDLOG(WARNING, "fail to open path %s for error %s", path.c_str(), strerror(errno));
            break;
        }
        ::rtidb::log::SequentialFile* seq_file = ::rtidb::log::NewSeqFile(path, fd);
        ::rtidb::log::Reader reader(seq_file, NULL, false, 0);
        std::string buffer;
        ::rtidb::api::LogEntry entry;
        uint64_t succ_cnt = 0;
        uint64_t failed_cnt = 0;
        // second
        uint64_t consumed = ::baidu::common::timer::now_time();
        while (true) {
            ::rtidb::base::Slice record;
            ::rtidb::base::Status status = reader.ReadRecord(&record, &buffer);
            if (status.IsWaitRecord() || status.IsEof()) {
                consumed = ::baidu::common::timer::now_time() - consumed;
                PDLOG(INFO, "read path %s for table tid %u pid %u completed, succ_cnt %lu, failed_cnt %lu, consumed %us",
                        path.c_str(), tid_, pid_, succ_cnt, failed_cnt, consumed);
                break;
            }
            if (!status.ok()) {
                PDLOG(WARNING, "fail to read record for tid %u, pid %u with error %s", tid_, pid_, status.ToString().c_str());
                failed_cnt++;
                continue;
            }
            bool ok = entry.ParseFromString(record.ToString());
            if (!ok) {
                PDLOG(WARNING, "fail parse record for tid %u, pid %u with value %s", tid_, pid_,
                        ::rtidb::base::DebugString(record.ToString()).c_str());
                failed_cnt++;
                continue;
            }
            succ_cnt++;
            if (succ_cnt % 100000 == 0) { 
                PDLOG(INFO, "load snapshot %s with succ_cnt %lu, failed_cnt %lu", path.c_str(),
                        succ_cnt, failed_cnt);
            }
            if (entry.dimensions_size() > 0) {
                table->Put(entry.ts(), entry.value(), entry.dimensions());
                
            }else {
                // the legend way
                table->Put(entry.pk(), entry.ts(), 
                           entry.value().c_str(), 
                           entry.value().size());
            }
        }
        // will close the fd atomic
        delete seq_file;
        if (g_succ_cnt) {
            g_succ_cnt->fetch_add(succ_cnt, std::memory_order_relaxed);
        }
        if (g_failed_cnt) {
            g_failed_cnt->fetch_add(failed_cnt, std::memory_order_relaxed);
        }
    }while(false);
}

int Snapshot::TTLSnapshot(std::shared_ptr<Table> table, const ::rtidb::api::Manifest& manifest, WriteHandle* wh, 
            uint64_t& count, uint64_t& expired_key_num) {
	std::string full_path = snapshot_path_ + manifest.name();
	FILE* fd = fopen(full_path.c_str(), "rb");
	if (fd == NULL) {
		PDLOG(WARNING, "fail to open path %s for error %s", full_path.c_str(), strerror(errno));
		return -1;
	}
	::rtidb::log::SequentialFile* seq_file = ::rtidb::log::NewSeqFile(manifest.name(), fd);
	::rtidb::log::Reader reader(seq_file, NULL, false, 0);

	std::string buffer;
	::rtidb::api::LogEntry entry;
	uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    bool has_error = false;
	while (true) {
		::rtidb::base::Slice record;
		::rtidb::base::Status status = reader.ReadRecord(&record, &buffer);
		if (status.IsEof()) {
			break;
		}
		if (!status.ok()) {
			PDLOG(WARNING, "fail to read record for tid %u, pid %u with error %s", tid_, pid_, status.ToString().c_str());
			has_error = true;        
			break;
		}
		if (!entry.ParseFromString(record.ToString())) {
			PDLOG(WARNING, "fail parse record for tid %u, pid %u with value %s", tid_, pid_,
					::rtidb::base::DebugString(record.ToString()).c_str());
			has_error = true;        
			break;
		}
		// delete timeout key
		if (table->IsExpired(entry, cur_time)) {
			expired_key_num++;
			continue;
		}
		status = wh->Write(record);
		if (!status.ok()) {
			PDLOG(WARNING, "fail to write snapshot. status[%s]", 
			              status.ToString().c_str());
			has_error = true;        
			break;
		}
        if ((count + expired_key_num) % KEY_NUM_DISPLAY == 0) {
			PDLOG(INFO, "tackled key num[%lu] total[%lu]", count + expired_key_num, manifest.count()); 
        }
		count++;
	}
	delete seq_file;
    if (expired_key_num + count != manifest.count()) {
	    PDLOG(WARNING, "key num not match! total key num[%lu] load key num[%lu] ttl key num[%lu]",
                    manifest.count(), count, expired_key_num);
        has_error = true;
    }
	if (has_error) {
		return -1;
	}	
	PDLOG(INFO, "load snapshot success. load key num[%lu] ttl key num[%lu]", count, expired_key_num); 
	return 0;
}

int Snapshot::MakeSnapshot(std::shared_ptr<Table> table, uint64_t& out_offset) {
    if (making_snapshot_.load(std::memory_order_acquire)) {
        PDLOG(INFO, "snapshot is doing now!");
        return 0;
    }
    making_snapshot_.store(true, std::memory_order_release);
    std::string now_time = ::rtidb::base::GetNowTime();
    std::string snapshot_name = now_time.substr(0, now_time.length() - 2) + ".sdb";
    std::string snapshot_name_tmp = snapshot_name + ".tmp";
    std::string full_path = snapshot_path_ + snapshot_name;
    std::string tmp_file_path = snapshot_path_ + snapshot_name_tmp;
    FILE* fd = fopen(tmp_file_path.c_str(), "ab+");
    if (fd == NULL) {
        PDLOG(WARNING, "fail to create file %s", tmp_file_path.c_str());
        making_snapshot_.store(false, std::memory_order_release);
        return -1;
    }

    uint64_t start_time = ::baidu::common::timer::now_time();
    WriteHandle* wh = new WriteHandle(snapshot_name_tmp, fd);
    ::rtidb::api::Manifest manifest;
    bool has_error = false;
    uint64_t write_count = 0;
    uint64_t expired_key_num = 0;
    int result = GetSnapshotRecord(manifest);
    if (result == 0) {
        // filter old snapshot
        if (TTLSnapshot(table, manifest, wh, write_count, expired_key_num) < 0) {
            has_error = true;
        }
    } else if (result < 0) {
        // parse manifest error
        has_error = true;
    }
    
    ::rtidb::log::LogReader log_reader(log_part_, log_path_);
    log_reader.SetOffset(offset_);
    uint64_t cur_offset = offset_;
    std::string buffer;
	uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    while (!has_error) {
        buffer.clear();
        ::rtidb::base::Slice record;
        ::rtidb::base::Status status = log_reader.ReadNextRecord(&record, &buffer);
        if (status.ok()) {
            ::rtidb::api::LogEntry entry;
            if (!entry.ParseFromString(record.ToString())) {
                PDLOG(WARNING, "fail to parse LogEntry. record[%s] size[%ld]", 
                        ::rtidb::base::DebugString(record.ToString()).c_str(), record.ToString().size());
                has_error = true;        
                break;
            }
            if (entry.log_index() <= cur_offset) {
                continue;
            }
            if (cur_offset + 1 != entry.log_index()) {
                PDLOG(WARNING, "log missing expect offset %lu but %ld", cur_offset + 1, entry.log_index());
                has_error = true;
                break;
            }
            cur_offset = entry.log_index();
            if (table->IsExpired(entry, cur_time)) {
                expired_key_num++;
                continue;
            }
            ::rtidb::base::Status status = wh->Write(record);
            if (!status.ok()) {
                PDLOG(WARNING, "fail to write snapshot. path[%s] status[%s]", 
                tmp_file_path.c_str(), status.ToString().c_str());
                has_error = true;        
                break;
            }
            write_count++;
            if ((write_count + expired_key_num) % KEY_NUM_DISPLAY == 0) {
                PDLOG(INFO, "has write key num[%lu] expired key num[%lu]", write_count, expired_key_num);
            }
        } else if (status.IsEof()) {
            continue;
        } else if (status.IsWaitRecord()) {
            PDLOG(DEBUG, "has read all record!");
            break;
        } else {
            PDLOG(WARNING, "fail to get record. status is %s", status.ToString().c_str());
            has_error = true;        
            break;
        }
    }
    if (wh != NULL) {
        wh->EndLog();
        delete wh;
        wh = NULL;
    }
    int ret = 0;
    if (has_error) {
        unlink(tmp_file_path.c_str());
        ret = -1;
    } else {
        if (rename(tmp_file_path.c_str(), full_path.c_str()) == 0) {
            if (RecordOffset(snapshot_name, write_count, cur_offset) == 0) {
                // delete old snapshot
                if (manifest.has_name() && manifest.name() != snapshot_name) {
                    PDLOG(DEBUG, "old snapshot[%s] has deleted", manifest.name().c_str()); 
                    unlink((snapshot_path_ + manifest.name()).c_str());
                }
                uint64_t consumed = ::baidu::common::timer::now_time() - start_time;
                PDLOG(INFO, "make snapshot[%s] success. update offset from %lu to %lu. use %lu second. write key %lu expired key %lu", 
                          snapshot_name.c_str(), offset_, cur_offset, consumed, write_count, expired_key_num);
                offset_ = cur_offset;
                out_offset = cur_offset;
            } else {
                PDLOG(WARNING, "RecordOffset failed. delete snapshot file[%s]", full_path.c_str());
                unlink(full_path.c_str());
                ret = -1;
            }
        } else {
            PDLOG(WARNING, "rename[%s] failed", snapshot_name.c_str());
            unlink(tmp_file_path.c_str());
            ret = -1;
        }
    }
    making_snapshot_.store(false, std::memory_order_release);
    return ret;
}

int Snapshot::GetSnapshotRecord(::rtidb::api::Manifest& manifest) {
    std::string full_path = snapshot_path_ + MANIFEST;
    int fd = open(full_path.c_str(), O_RDONLY);
    if (fd < 0) {
        PDLOG(INFO, "[%s] is not exisit", MANIFEST.c_str());
        return 1;
    } else {
        google::protobuf::io::FileInputStream fileInput(fd);
        fileInput.SetCloseOnDelete(true);
        if (!google::protobuf::TextFormat::Parse(&fileInput, &manifest)) {
            PDLOG(WARNING, "parse manifest failed");
            return -1;
        }
    }
    return 0;
}

int Snapshot::RecordOffset(const std::string& snapshot_name, uint64_t key_count, uint64_t offset) {
    PDLOG(DEBUG, "record offset[%lu]. add snapshot[%s] key_count[%lu]",
                offset, snapshot_name.c_str(), key_count);
    std::string full_path = snapshot_path_ + MANIFEST;
    std::string tmp_file = snapshot_path_ + MANIFEST + ".tmp";
    ::rtidb::api::Manifest manifest;
    std::string manifest_info;
    manifest.set_offset(offset);
    manifest.set_name(snapshot_name);
    manifest.set_count(key_count);
    manifest_info.clear();
    google::protobuf::TextFormat::PrintToString(manifest, &manifest_info);
    FILE* fd_write = fopen(tmp_file.c_str(), "w");
    if (fd_write == NULL) {
        PDLOG(WARNING, "fail to open file %s", tmp_file.c_str());
        return -1;
    }
    bool io_error = false;
    if (fputs(manifest_info.c_str(), fd_write) == EOF) {
        PDLOG(WARNING, "write error. path[%s]", tmp_file.c_str());
        io_error = true;
    }
    if (!io_error && ((fflush(fd_write) == EOF) || fsync(fileno(fd_write)) == -1)) {
        PDLOG(WARNING, "flush error. path[%s]", tmp_file.c_str());
        io_error = true;
    }
    fclose(fd_write);
    if (!io_error && rename(tmp_file.c_str(), full_path.c_str()) == 0) {
        PDLOG(DEBUG, "%s generate success. path[%s]", MANIFEST.c_str(), full_path.c_str());
        return 0;
    }
    unlink(tmp_file.c_str());
    return -1;
}

}
}


