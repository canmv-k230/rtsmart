/**
 * @file      pufs_ecp.h
 * @brief     PUFsecurity ECP API interface
 * @copyright 2020 PUFsecurity
 */
/* THIS SOFTWARE IS SUPPLIED BY PUFSECURITY ''AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. TO THE FULLEST
 * EXTENT ALLOWED BY LAW, PUFSECURITY'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES,
 * IF ANY, THAT YOU HAVE PAID DIRECTLY TO PUFSECURITY FOR THIS SOFTWARE.
 */

#ifndef __PUFS_ECP_H__
#define __PUFS_ECP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "pufs_ka.h"
#include "pufs_ecc.h"
#include "pufs_hmac.h"
#include "pufs_rt.h"

/*****************************************************************************
 * API functions
 ****************************************************************************/
/**
 * @brief Initialize PKC module
 *
 * @param[in] pkc_offset  PKC offset of memory map
 */
void pufs_pkc_module_init(uintptr_t pkc_offset);
/**
 * @brief RSA verification.
 *
 * @param[in] sig      RSA signature.
 * @param[in] rsatype  RSA type.
 * @param[in] n        RSA parameter n.
 * @param[in] puk      RSA public key.
 * @param[in] msg      Message.
 * @return             SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_rsa_verify(const uint8_t* sig,
                              pufs_rsa_type_t rsatype,
                              const uint8_t* n,
                              uint32_t puk,
                              const uint8_t* msg);
/**
 * @brief Wrapper function of _pufs_rsa_sign() to set NULL as the
 *        default value of the last parameter if not provided.
 */
#define pufs_rsa_sign(sig, rsatype, n, puk, prk, ...) \
    _pufs_rsa_sign(sig, rsatype, n, puk, prk, DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief RSA signing.
 *
 * @param[out] sig      RSA signature.
 * @param[in]  rsatype  RSA type.
 * @param[in]  n        RSA parameter n.
 * @param[in]  puk      RSA public key.
 * @param[in]  prk      RSA private key.
 * @param[in]  msg      Message.
 * @param[in]  phi      The value of Euler totient function of RSA parameter n.
 * @return              SUCCESS on success, otherwise an error code.
 */
pufs_status_t _pufs_rsa_sign(uint8_t* sig,
                            pufs_rsa_type_t rsatype,
                            const uint8_t* n,
                            uint32_t puk,
                            const uint8_t* prk,
                            const uint8_t* msg,
                            const uint8_t* phi);
/**
 * @brief X9.31 RSA verification.
 *
 * @param[in] sig      RSA signature.
 * @param[in] rsatype  RSA type.
 * @param[in] n        RSA parameter n.
 * @param[in] puk      RSA public key.
 * @param[in] msg      Message.
 * @param[in] msglen   Message length in bytes.
 * @return             SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_rsa_x931_verify(const uint8_t* sig,
                                   pufs_rsa_type_t rsatype,
                                   const uint8_t* n,
                                   uint32_t puk,
                                   const uint8_t* msg,
                                   uint32_t msglen);
/**
 * @brief Wrapper function of _pufs_rsa_x931_sign() to set NULL as the
 *        default value of the last parameter if not provided.
 */
#define pufs_rsa_x931_sign(sig, rsatype, n, puk, prk, hash, msg, ...) \
    _pufs_rsa_x931_sign(sig, rsatype, n, puk, prk, hash, msg, DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief X9.31 RSA signing.
 *
 * @param[out] sig      RSA signature.
 * @param[in]  rsatype  RSA type.
 * @param[in]  n        RSA parameter n.
 * @param[in]  puk      RSA public key.
 * @param[in]  prk      RSA private key.
 * @param[in]  hash     Hash algorithm.
 * @param[in]  msg      Message.
 * @param[in]  msglen   Message length in bytes.
 * @param[in]  phi      The value of Euler totient function of RSA parameter n.
 * @return              SUCCESS on success, otherwise an error code.
 */
pufs_status_t _pufs_rsa_x931_sign(uint8_t* sig,
                                  pufs_rsa_type_t rsatype,
                                  const uint8_t* n,
                                  uint32_t puk,
                                  const uint8_t* prk,
                                  pufs_hash_t hash,
                                  const uint8_t* msg,
                                  uint32_t msglen,
                                  const uint8_t* phi);
/**
 * @brief PKCS#1 v1.5 RSA verification.
 *
 * @param[in] sig      RSA signature.
 * @param[in] rsatype  RSA type.
 * @param[in] n        RSA parameter n.
 * @param[in] puk      RSA public key.
 * @param[in] msg      Message.
 * @param[in] msglen   Message length in bytes.
 * @return             SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_rsa_p1v15_verify(const uint8_t* sig,
                                    pufs_rsa_type_t rsatype,
                                    const uint8_t* n,
                                    uint32_t puk,
                                    const uint8_t* msg,
                                    uint32_t msglen);
/**
 * @brief Wrapper function of _pufs_rsa_p1v15_sign() to set NULL as the
 *        default value of the last parameter if not provided.
 */
#define pufs_rsa_p1v15_sign(sig, rsatype, n, puk, prk, hash, msg, ...) \
    _pufs_rsa_p1v15_sign(sig, rsatype, n, puk, prk, hash, msg, DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief PKCS#1 v1.5 RSA signing.
 *
 * @param[out] sig      RSA signature.
 * @param[in]  rsatype  RSA type.
 * @param[in]  n        RSA parameter n.
 * @param[in]  puk      RSA public key.
 * @param[in]  prk      RSA private key.
 * @param[in]  hash     Hash algorithm.
 * @param[in]  msg      Message.
 * @param[in]  msglen   Message length in bytes.
 * @param[in]  phi      The value of Euler totient function of RSA parameter n.
 * @return              SUCCESS on success, otherwise an error code.
 */
pufs_status_t _pufs_rsa_p1v15_sign(uint8_t* sig,
                                   pufs_rsa_type_t rsatype,
                                   const uint8_t* n,
                                   uint32_t puk,
                                   const uint8_t* prk,
                                   pufs_hash_t hash,
                                   const uint8_t* msg,
                                   uint32_t msglen,
                                   const uint8_t* phi);
/**
 * @brief RSA PSS verification.
 *
 * @param[in] sig      RSA signature.
 * @param[in] rsatype  RSA type.
 * @param[in] n        RSA parameter n.
 * @param[in] puk      RSA public key.
 * @param[in] hash     Hash algorithm.
 * @param[in] msg      Message.
 * @param[in] msglen   Message length in bytes.
 * @return             SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_rsa_pss_verify(const uint8_t* sig,
                                  pufs_rsa_type_t rsatype,
                                  const uint8_t* n,
                                  uint32_t puk,
                                  pufs_hash_t hash,
                                  const uint8_t* msg,
                                  uint32_t msglen);
/**
 * @brief Wrapper function of _pufs_rsa_p1v15_sign() to set NULL as the
 *        default value of the last parameter if not provided.
 */
#define pufs_rsa_pss_sign(sig, rsatype, n, puk, prk, hash, msg, msglen, salt, ...) \
    _pufs_rsa_pss_sign(sig, rsatype, n, puk, prk, hash, msg, msglen, salt, DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief RSA PSS signing.
 *
 * @param[out] sig      RSA signature.
 * @param[in]  rsatype  RSA type.
 * @param[in]  n        RSA parameter n.
 * @param[in]  puk      RSA public key.
 * @param[in]  prk      RSA private key.
 * @param[in]  hash     Hash algorithm.
 * @param[in]  msg      Message.
 * @param[in]  msglen   Message length in bytes.
 * @param[in]  salt     Salt.
 * @param[in]  saltlen  Salt length in bytes.
 * @param[in]  phi      The value of Euler totient function of RSA parameter n.
 * @return              SUCCESS on success, otherwise an error code.
 */
pufs_status_t _pufs_rsa_pss_sign(uint8_t* sig,
                                 pufs_rsa_type_t rsatype,
                                 const uint8_t* n,
                                 uint32_t puk,
                                 const uint8_t* prk,
                                 pufs_hash_t hash,
                                 const uint8_t* msg,
                                 uint32_t msglen,
                                 const uint8_t* salt,
                                 uint32_t saltlen,
                                 const uint8_t* phi);
/**
 * @brief Set elliptic curve domain parameters by name.
 *
 * @param[in] name  Elliptic curve name.
 * @return          SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_ecp_set_curve_byname(pufs_ec_name_t name);
/**
 * @brief Wrapper function of _pufs_ecp_gen_sprk() to set SHA_256 as the
 *        default value of the last parameter if not provided.
 */
#define pufs_ecp_gen_sprk(slot, keytype, keyaddr, keybits, salt, saltlen, info, ...) \
    _pufs_ecp_gen_sprk(slot, keytype, keyaddr, keybits, salt, saltlen, info, DEF_ARG(__VA_ARGS__, SHA_256))
/**
 * @brief Generate static ECC private key.
 *
 * @param[in] slot     Private key slot.
 * @param[in] pufslot  PUF slots (1-3).
 * @param[in] salt     Salt used by the KDF to derive KDK.
 * @param[in] saltlen  Salt length.
 * @param[in] info     Info used in KDF.
 * @param[in] infolen  Info length.
 * @param[in] hash     Hash algorithm.
 * @return             SUCCESS on success, otherwise an error code.
 */
pufs_status_t _pufs_ecp_gen_sprk(pufs_ka_slot_t slot,
                                 pufs_key_type_t keytype,
                                 size_t keyaddr,
                                 uint32_t keybits,
                                 const uint8_t* salt,
                                 uint32_t saltlen,
                                 const uint8_t* info,
                                 uint32_t infolen,
                                 pufs_hash_t hash);
/**
 * @brief Generate ephemeral ECC private key.
 *
 * @param[in] slot  Private key slot.
 * @return          SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_ecp_gen_eprk(pufs_ka_slot_t slot);
/**
 * @brief Generate ECC public key of the corresponding private key.
 *
 * @param[out] puk      Public key.
 * @param[in]  prktype  Private key type.
 * @param[in]  prkslot  Private key slot.
 * @return              SUCCESS on success, otherwise an error code.
 * @retval E_UNSUPPORT  NIST P-256 with OTPKEY on ECF39303 microprogram version 0.
 *
 * @note Only \ref PRKEY and \ref OTPKEY are allowed \em keytype.
 */
pufs_status_t pufs_ecp_gen_puk(pufs_ec_point_st* puk,
                               pufs_key_type_t prktype,
                               uint32_t prkslot);
/**
 * @brief Wrapper function of _pufs_ecp_validate_puk() to set false as the
 *        default value of the last parameter if not provided.
 */
#define pufs_ecp_validate_puk(...) \
    _pufs_ecp_validate_puk(DEF_ARG(__VA_ARGS__, false))
/**
 * @brief Validate ECC public key.
 *
 * @param[in] puk   ECC public key.
 * @param[in] full  A flag to enable full validation.
 * @return          SUCCESS on success, otherwise an error code.
 *
 * @remark Use the wrapper function pufs_ecp_validate_puk() for convenience.
 */
pufs_status_t _pufs_ecp_validate_puk(pufs_ec_point_st puk, bool full);
/**
 * @brief Wrapper function of _pufs_ecp_ecccdh_2e() to set NULL as the default
 *        value of the last parameter if not provided.
 */
#define pufs_ecp_ecccdh_2e(puk, ...) \
    _pufs_ecp_ecccdh_2e(puk, DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief Derive shared secret from ephemeral keys by ECC CDH.
 *
 * @param[in]  puk      Public key.
 * @param[in]  prkslot  Private key slot.
 * @param[out] ss       Shared secret.
 * @return              SUCCESS on success, otherwise an error code.
 *
 * @remark Use the wrapper function pufs_ecp_ecccdh_2e() for convenience.
 */
pufs_status_t _pufs_ecp_ecccdh_2e(pufs_ec_point_st puk,
                                  pufs_ka_slot_t prkslot,
                                  uint8_t* ss);
/**
 * @brief Wrapper function of _pufs_ecp_ecccdh_2e2s() to set NULL as the default
 *        value of the last parameter if not provided.
 */
#define pufs_ecp_ecccdh_2e2s(puk_e, puk_s, prkslot_e, prktype_s, ...)\
    _pufs_ecp_ecccdh_2e2s(puk_e, puk_s, prkslot_e, prktype_s, \
                          DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief Derive shared secret from ephemeral and static keys by ECC CDH.
 *
 * @param[in]  puk_e      Ephemeral public key.
 * @param[in]  puk_s      Static public key.
 * @param[in]  prkslot_e  Ephemeral private key slot.
 * @param[in]  prktype_s  Static private key type.
 * @param[in]  prkslot_s  Static private key slot.
 * @param[out] ss         Shared secret.
 * @return                SUCCESS on success, otherwise an error code.
 *
 * @note Only \ref PRKEY and \ref OTPKEY are allowed \em keytype.
 * @remark Use the wrapper function pufs_ecp_ecccdh_2e2s() for convenience.
 */
pufs_status_t _pufs_ecp_ecccdh_2e2s(pufs_ec_point_st puk_e,
                                    pufs_ec_point_st puk_s,
                                    pufs_ka_slot_t prkslot_e,
                                    pufs_key_type_t prktype_s,
                                    uint32_t prkslot_s,
                                    uint8_t* ss);
/**
 * @brief Verify the ECDSA signature of the message digest.
 *
 * @param[in] sig  Signature.
 * @param[in] md   Message digest.
 * @param[in] puk  Public key for signature verification.
 * @return         SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_ecp_ecdsa_verify_dgst(pufs_ecdsa_sig_st sig,
                                         pufs_dgst_st md,
                                         pufs_ec_point_st puk);
/**
 * @brief Verify the ECDSA signature of the message.
 *
 * @param[in] sig     Signature.
 * @param[in] msg     Message.
 * @param[in] msglen  Message length in bytes.
 * @param[in] hash    Hash algorithm.
 * @param[in] puk     Public key for signature verification.
 * @return            SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_ecp_ecdsa_verify_msg(pufs_ecdsa_sig_st sig,
                                        const uint8_t* msg,
                                        uint32_t msglen,
                                        pufs_hash_t hash,
                                        pufs_ec_point_st puk);
/**
 * @brief Verify the ECDSA signature of the message digest using OTP public key.
 *
 * @param[in] sig  Signature.
 * @param[in] md   Digest of the signed message.
 * @param[in] puk  OTP key slot of public key for signature verification.
 * @return         SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_ecp_ecdsa_verify_dgst_otpkey(pufs_ecdsa_sig_st sig,
                                                pufs_dgst_st md,
                                                pufs_rt_slot_t puk);
/**
 * @brief Verify the ECDSA signature of the message using OTP public key.
 *
 * @param[in] sig     Signature.
 * @param[in] msg     Message.
 * @param[in] msglen  Message length in bytes.
 * @param[in] hash    Hash algorithm.
 * @param[in] puk     OTP key slot of public key for signature verification.
 * @return            SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_ecp_ecdsa_verify_msg_otpkey(pufs_ecdsa_sig_st sig,
                                               const uint8_t* msg,
                                               uint32_t msglen,
                                               pufs_hash_t hash,
                                               pufs_rt_slot_t puk);
/**
 * @brief Wrapper function of _pufs_ecp_ecdsa_sign_dgst() to set NULL as the
 *        default value of the last parameter if not provided.
 */
#define pufs_ecp_ecdsa_sign_dgst(sig, md, prktype, ...) \
    _pufs_ecp_ecdsa_sign_dgst(sig, md, prktype, DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief Generate an ECDSA signature from a message digest.
 *
 * @param[in] sig      Signature.
 * @param[in] md       Message digest.
 * @param[in] prktype  Private key type.
 * @param[in] prkslot  Private key slot.
 * @param[in] k        Random k only used in CAVP.
 * @return             SUCCESS on success, otherwise an error code.
 * @retval E_UNSUPPORT NIST P-256 with OTPKEY on ECF39303 microprogram version 0.
 *
 * @note Only \ref PRKEY and \ref OTPKEY are allowed \em keytype.
 * @remark Use the wrapper function pufs_ecp_ecdsa_sign_dgst() for convenience.
 */
pufs_status_t _pufs_ecp_ecdsa_sign_dgst(pufs_ecdsa_sig_st* sig,
                                        pufs_dgst_st md,
                                        pufs_key_type_t prktype,
                                        uint32_t prkslot,
                                        const uint8_t* k);
/**
 * @brief Wrapper function of _pufs_ecp_ecdsa_sign_msg() to set NULL as the
 *        default value of the last parameter if not provided.
 */
#define pufs_ecp_ecdsa_sign_msg(sig, msg, msglen, hash, prktype, ...) \
    _pufs_ecp_ecdsa_sign_msg(sig, msg, msglen, hash, prktype, \
                             DEF_ARG(__VA_ARGS__, NULL))
/**
 * @brief Generate an ECDSA signature of a message.
 *
 * @param[in] sig      Signature.
 * @param[in] msg      Message.
 * @param[in] msglen   Message length in bytes.
 * @param[in] hash     Hash algorithm.
 * @param[in] prktype  Private key type.
 * @param[in] prkslot  Private key slot.
 * @param[in] k        Random k only used in CAVP.
 * @return             SUCCESS on success, otherwise an error code.
 *
 * @note Only \ref PRKEY and \ref OTPKEY are allowed \em keytype.
 * @remark Use the wrapper function pufs_ecp_ecdsa_sign_msg() for convenience.
 */
pufs_status_t _pufs_ecp_ecdsa_sign_msg(pufs_ecdsa_sig_st* sig,
                                       const uint8_t* msg,
                                       uint32_t msglen,
                                       pufs_hash_t hash,
                                       pufs_key_type_t prktype,
                                       uint32_t prkslot,
                                       const uint8_t* k);

#ifdef __cplusplus
} // closing brace for extern "C"
#endif

#endif /* __PUFS_ECP_H__ */
