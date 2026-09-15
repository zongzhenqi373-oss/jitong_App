// 内存文件系统（P7-G1 Harness）。
//
// 目的：测试不触碰真实磁盘，且能**注入故障**（磁盘满、写失败）并记录 fsync 次数，
// 用于 ADR-03 cutover journal 的「每次 fsync 前后强杀」恢复矩阵测试。

#ifndef CLIENT_CORE_TESTING_FAKE_FILE_SYSTEM_H
#define CLIENT_CORE_TESTING_FAKE_FILE_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace im {
namespace testing {

class FakeFileSystem {
public:
    struct File {
        std::vector<unsigned char> data;
        std::int64_t fsyncCount = 0;
    };

    enum class WriteResult { Ok, NoSpace, IoError };

    // ---- 故障注入 ----
    void setDiskFull(bool full) { m_diskFull = full; }
    void setIoError(bool err) { m_ioError = err; }

    bool write(const std::string& path, const std::vector<unsigned char>& data)
    {
        if (m_diskFull) return false;
        if (m_ioError) return false;
        m_files[path].data = data;
        return true;
    }

    bool append(const std::string& path, const std::vector<unsigned char>& data)
    {
        if (m_diskFull || m_ioError) return false;
        auto& f = m_files[path];
        f.data.insert(f.data.end(), data.begin(), data.end());
        return true;
    }

    bool read(const std::string& path, std::vector<unsigned char>* out) const
    {
        auto it = m_files.find(path);
        if (it == m_files.end()) return false;
        if (out) *out = it->second.data;
        return true;
    }

    bool exists(const std::string& path) const { return m_files.count(path) > 0; }

    std::size_t size(const std::string& path) const
    {
        auto it = m_files.find(path);
        return it == m_files.end() ? 0 : it->second.data.size();
    }

    bool remove(const std::string& path) { return m_files.erase(path) > 0; }

    /** 原子替换：只有源存在且目标可写时成功（用于 key blob / 下载 rename 语义）。 */
    bool renameReplace(const std::string& from, const std::string& to)
    {
        auto it = m_files.find(from);
        if (it == m_files.end()) return false;
        if (m_diskFull || m_ioError) return false;
        m_files[to] = it->second;
        m_files.erase(it);
        return true;
    }

    bool fsync(const std::string& path)
    {
        auto it = m_files.find(path);
        if (it == m_files.end()) return false;
        ++it->second.fsyncCount;
        return true;
    }

    std::int64_t fsyncCount(const std::string& path) const
    {
        auto it = m_files.find(path);
        return it == m_files.end() ? 0 : it->second.fsyncCount;
    }

    std::size_t fileCount() const { return m_files.size(); }
    void clear() { m_files.clear(); }

private:
    std::map<std::string, File> m_files;
    bool m_diskFull = false;
    bool m_ioError = false;
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_FAKE_FILE_SYSTEM_H
