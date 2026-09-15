// SQLCipher 参数固化常量（P6-T01）。
//
// 全部数值来自**实测**，不是凭印象填写：
//   - 来源：arm64 AVD（Android 14）上用 Room 自带 AAR 的 libsqlcipher.so 实跑
//     `PRAGMA cipher_version / cipher_page_size / kdf_iter / cipher_kdf_algorithm /
//      cipher_hmac_algorithm / cipher_use_hmac / cipher_plaintext_header_size` 得到。
//   - 即本工程 Room（`net.zetetic:sqlcipher-android:4.6.1`）实际使用的默认值。
//   - 依据与复现命令见 outputs/kernel-round5-encryption-note.md。
//
// 用途：
//   1. 新建 Native 数据库时显式设置这些参数，保证与 Room 参数一致（未来若要接管
//      Room 库，或做 Room↔Native 兼容性 fixture，参数必须一致才可能互开）。
//   2. 启动自检：`cipher_version` 必须以 kExpectedCipherVersion 开头，否则 fail-close，
//      绝不创建一个"看似成功"的明文/错参数据库。
//
// 注意：不得记录、打印 key；本文件只含参数，不含任何密钥材料。

#ifndef CLIENT_CORE_CIPHER_PARAMS_H
#define CLIENT_CORE_CIPHER_PARAMS_H

namespace im {
namespace storage {

struct CipherParams {
    /** 期望的 SQLCipher 版本前缀（实测 "4.6.1 community"）。 */
    static constexpr const char* kExpectedCipherVersion = "4.6.1";

    /** 数据库页大小（同时用于 cipher_page_size）。 */
    static constexpr int kPageSize = 4096;

    /** KDF 迭代次数。 */
    static constexpr int kKdfIter = 256000;

    /** KDF 算法。 */
    static constexpr const char* kKdfAlgorithm = "PBKDF2_HMAC_SHA512";

    /** HMAC 算法。 */
    static constexpr const char* kHmacAlgorithm = "HMAC_SHA512";

    /** 是否启用 HMAC（1=启用）。 */
    static constexpr int kUseHmac = 1;

    /** 明文头大小（0=完全加密，连文件头也不明文）。 */
    static constexpr int kPlaintextHeaderSize = 0;

    /** raw key 长度（字节）。DbKeyManager 生成的 realKey 即 32 字节。 */
    static constexpr std::size_t kRawKeyBytes = 32;

    /** raw key 的 SQLCipher 字面量形式 x'<64hex>' 的长度（含 x、两个引号与结尾）。 */
    static constexpr std::size_t kRawKeyLiteralChars = 2 + 64 + 2; // x'...' + 结尾 0
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_CIPHER_PARAMS_H
