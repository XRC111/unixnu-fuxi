// SPDX-License-Identifier: BSD-2-Clause-Patent
// The persistent-store hooks NativeRamBlock.cpp (an unchanged copy of UniXNU RamBlock 4be2d7e) imports weakly. On sheng
// ShengUfs provides them; fuxi originally had no storage driver and the kernel links statically, so the weak imports still needed definitions. With ShengUfs linked (CONFIG_XNU_NATIVE_UFS=y), NativeUfs defines these hooks strongly; each no-op here is weak, so the strong definition wins and the same file serves both builds.
#include <stdint.h>

extern "C" {
// With ShengUfs linked (CONFIG_XNU_NATIVE_UFS=y), NativeUfs defines these hooks strongly;
// a weak no-op here loses to the strong definition, so the same object file works for both
// the storage-driver build and a future build without one.
__attribute__((weak)) unsigned native_ufs_store_mode(void) {return 0;}
__attribute__((weak)) int native_ufs_store_open(uint64_t, uint64_t *, const char **why) {if (why) *why = "no store on fuxi"; return 0;}
__attribute__((weak)) int native_ufs_store_read(uint64_t, uint32_t, void *) {return 0;}
__attribute__((weak)) int native_ufs_store_write(uint64_t, uint32_t, const void *) {return 0;}
__attribute__((weak)) int native_ufs_store_sync(void) {return 0;}
__attribute__((weak)) unsigned native_ufs_store_gpt_check(void) {return 0xff;}
}



