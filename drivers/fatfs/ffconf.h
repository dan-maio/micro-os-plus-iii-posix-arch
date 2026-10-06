/*---------------------------------------------------------------------------/
/  FatFs configuration for the Raspberry Pi Zero 2W (BCM2837) bare-metal port
/---------------------------------------------------------------------------/
/  Tuned for the rpi-zero-2w hardware SD tests:
/   - Read/write of an EXISTING FAT32 boot partition, files under a /tests
/     folder, so boot files (config.txt, kernel7.img/kernel8.img, ...) are
/     never touched.
/   - Short (8.3) file names only: FF_USE_LFN=0 -> no ffunicode.c needed, and
/     every name used by the tests fits in 8.3 (A.mat, b.mat, x_block.mat,
/     xclassic.mat, hello.txt, data1.bin, data2.bin, big.bin).
/   - Single volume, fixed 512-byte sectors, no RTC (fixed timestamp), no
/     re-entrancy (SD I/O runs on one core / one task at a time).
/  Keep FFCONF_DEF in lock-step with FF_DEFINED in ff.h (rev 5380 = R0.15a).
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	5380

/*----- Function Configurations -----*/
#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		0	/* do NOT format the boot card! */
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	3

/*----- Locale and Namespace Configurations -----*/
#define FF_CODE_PAGE	437	/* U.S. (only used when LFN enabled) */
#define FF_USE_LFN		0	/* 8.3 names only -> no ffunicode.c */
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
#define FF_FS_RPATH		0

/*----- Drive/Volume Configurations -----*/
#define FF_VOLUMES		1
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0

/*----- System Configurations -----*/
#define FF_FS_TINY		0
#define FF_FS_EXFAT		0
#define FF_FS_NORTC		1	/* no RTC: use the fixed date below */
#define FF_NORTC_MON	7
#define FF_NORTC_MDAY	5
#define FF_NORTC_YEAR	2026
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000

/*--- End of configuration options ---*/
