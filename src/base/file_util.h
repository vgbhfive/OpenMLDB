//
// file_util.h
// Copyright (C) 2017 4paradigm.com
// Author vagrant
// Date 2017-06-07
//


#ifndef RTIDB_FILE_UTIL_H
#define RTIDB_FILE_UTIL_H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include "logging.h"
#include <string.h>
#include <vector>

using ::baidu::common::INFO;
using ::baidu::common::WARNING;

namespace rtidb {
namespace base {

inline static bool Mkdir(const std::string& path) {

    const int dir_mode = 0777;
    int ret = ::mkdir(path.c_str(), dir_mode); 
    if (ret == 0 || errno == EEXIST) {
        return true; 
    }
    PDLOG(WARNING, "mkdir %s failed err[%d: %s]", 
            path.c_str(), errno, strerror(errno));
    return false;
}

inline static bool IsExists(const std::string& path) {
    struct stat buf;
    int ret = ::lstat(path.c_str(), &buf);
    if (ret < 0) {
        return false;
    }
    return true;
}

inline static bool Rename(const std::string& source, const std::string& target) {
    int ret = ::rename(source.c_str(), target.c_str()); 
    if (ret != 0) {
        PDLOG(WARNING, "fail to rename %s to %s with error %s", source.c_str(),
                target.c_str(), strerror(errno));
        return false;
    }
    return true;
}

inline static bool MkdirRecur(const std::string& dir_path) {

    size_t beg = 0;
    size_t seg = dir_path.find('/', beg);
    while (seg != std::string::npos) {
        if (seg + 1 >= dir_path.size()) {
            break; 
        }
        if (!Mkdir(dir_path.substr(0, seg + 1))) {
            return false; 
        }
        beg = seg + 1;
        seg = dir_path.find('/', beg);
    }
    return Mkdir(dir_path);
}

inline static int GetSubDir(const std::string& path, std::vector<std::string>& sub_dir) {
    if (path.empty()) {
        return -1;
    }
    DIR *dir = opendir(path.c_str());
    if (dir == NULL) {
        return -1;
    }
    struct dirent *ptr;
    while ((ptr = readdir(dir)) != NULL) {
        if(strcmp(ptr->d_name, ".") == 0 || strcmp(ptr->d_name, "..") == 0) {
            continue;
        } else if (ptr->d_type == DT_DIR) {
            sub_dir.push_back(ptr->d_name);
        }
    }
    closedir(dir);
    return 0;
}

inline static int GetFileName(const std::string& path, std::vector<std::string>& file_vec) {
    if (path.empty()) {
        PDLOG(WARNING, "input path is empty");
        return -1;
    }
    DIR *dir = opendir(path.c_str());
    if (dir == NULL) {
        PDLOG(WARNING, "fail to open path %s for %s", path.c_str(),
                strerror(errno));
        return -1;
    }
    struct dirent *ptr;
    struct stat stat_buf;
    while ((ptr = readdir(dir)) != NULL) {
        if(strcmp(ptr->d_name, ".") == 0 || strcmp(ptr->d_name, "..") == 0) {
            continue;
        }
        std::string file_path = path + "/" + ptr->d_name;
        int ret = lstat(file_path.c_str(), &stat_buf);
        if (ret == -1) {
            PDLOG(WARNING, "stat path %s failed err[%d: %s]",
                    file_path.c_str(),
                    errno,
                    strerror(errno));
            closedir(dir);
            return -1;
        }
        if (S_ISREG(stat_buf.st_mode)) {
            file_vec.push_back(file_path);
        }
    }
    closedir(dir);
    return 0;
}

}
}

#endif /* !FILE_UTIL_H */