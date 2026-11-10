/*
 * ota_uecc.h - micro-ecc configured for Stepwhere: secp256r1 only.
 * Always include this instead of uecc/uECC.h so the settings match.
 */
#ifndef STEPWHERE_OTA_UECC_H
#define STEPWHERE_OTA_UECC_H

#define uECC_SUPPORTS_secp160r1       0
#define uECC_SUPPORTS_secp192r1       0
#define uECC_SUPPORTS_secp224r1       0
#define uECC_SUPPORTS_secp256r1       1
#define uECC_SUPPORTS_secp256k1       0
#define uECC_SUPPORT_COMPRESSED_POINT 0
#ifndef uECC_OPTIMIZATION_LEVEL
#define uECC_OPTIMIZATION_LEVEL       1   /* small code; verify is still fast enough */
#endif

#include "uecc/uECC.h"

#endif /* STEPWHERE_OTA_UECC_H */
