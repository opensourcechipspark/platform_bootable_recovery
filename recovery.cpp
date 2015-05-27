/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _U
#define _U _CTYPE_U
#endif
#ifndef _L
#define _L _CTYPE_L
#endif
#ifndef _N
#define _N _CTYPE_N
#endif
#ifndef _X
#define _X _CTYPE_X
#endif
#ifndef _P
#define _P _CTYPE_P
#endif
#ifndef _B
#define _B _CTYPE_B
#endif
#ifndef _C
#define _C _CTYPE_C
#endif
#ifndef _S
#define _S _CTYPE_S
#endif

#include <ctype.h>
#include <dirent.h>
#include <fs_mgr.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mount.h>
#include <pthread.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "cutils/android_reboot.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "ui.h"
#include "screen_ui.h"
#include "device.h"
#include "adb_install.h"
#include "mtdutils/mounts.h"
#include "sdtool.h"
extern "C" {
#include <sparse/sparse.h>
// #include "minadbd/adb.h"
#include "mtdutils/rk29.h"
#include "mtdutils/mtdutils.h"
//#include "rkimage.h"
}

#ifdef USE_RADICAL_UPDATE
#include "radical_update.h"
#endif

#include "rkupdate/Upgrade.h"

struct selabel_handle *sehandle;

static const struct option OPTIONS[] = {
  { "factory_mode", required_argument, NULL, 'f' },
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "update_rkimage", required_argument, NULL, 'r' },   // support rkimage to update
  { "radical_update_package", required_argument, NULL, 'z' },   // to support ru_pkg to update
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { "show_text", no_argument, NULL, 't' },
  { "wipe_all", no_argument, NULL, 'w'+'a' },
  { "just_exit", no_argument, NULL, 'x' },
  { "locale", required_argument, NULL, 'l' },
  { "stages", required_argument, NULL, 'g' },
  { "pcba_test", required_argument, NULL, 'p' },
  { "fw_update", required_argument, NULL, 'f'+'w' },
  { "demo_copy", required_argument, NULL, 'd' },
  { "volume_label", required_argument, NULL, 'v' },
  { "resize_partition", required_argument, NULL, 'r'+'p' },
  { NULL, 0, NULL, 0 },
};

#define LAST_LOG_FILE "/cache/recovery/last_log"

static const char *CACHE_LOG_DIR = "/cache/recovery";
static const char *COMMAND_FILE = "/cache/recovery/command";
static const char *FLAG_FILE = "/cache/recovery/last_flag";
static const char *INTENT_FILE = "/cache/recovery/intent";
static const char *LOG_FILE = "/cache/recovery/log";
static const char *LAST_INSTALL_FILE = "/cache/recovery/last_install";
static const char *LOCALE_FILE = "/cache/recovery/last_locale";
static const char *CACHE_ROOT = "/cache";
static const char *USB_ROOT = "/mnt/usb_storage";
//static const char *SDCARD_ROOT = "/sdcard";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *TEMPORARY_INSTALL_FILE = "/tmp/last_install";
static const char *SIDELOAD_TEMP_DIR = "/tmp/sideload";
static const char *AUTO_FACTORY_UPDATE_TAG = "/FirmwareUpdate/auto_sd_update.tag";
static const char *AUTO_FACTORY_UPDATE_PACKAGE = "/FirmwareUpdate/update.img";
static const char *DATA_PARTITION_NAME = "userdata";
static const char *DATABK_PARTITION_NAME = "databk";
static char IN_SDCARD_ROOT[256] = "\0";
static char EX_SDCARD_ROOT[256] = "\0";
static char USB_DEVICE_PATH[128] = "\0";
static char updatepath[128] = "\0";

extern "C" int adb_main();

extern "C" int custom();
extern "C" int restore();
int do_update_rkimage(char *pFile);


#if TARGET_BOARD_PLATFORM == rockchip
bool bNeedClearMisc=true;
#endif
bool bEmmc=false;
bool bAutoUpdateComplete = false;
RecoveryUI* ui = NULL;
char* recovery_locale = NULL;
char recovery_version[PROPERTY_VALUE_MAX+1];
char* stage = NULL;

//for sdtool, factory tools
bool bIfUpdateLoader = false;
char gVolume_label[128];
enum ConfigId {
	pcba_test = 0,
	fw_update,
	display_lcd,
	display_led,
	demo_copy,
	volume_label
};

static RKSdBootCfgItem SdBootConfigs[6];

void *thrd_led_func(void *arg);
pthread_t tid;
bool isLedFlash = false;
bool bSDMounted = false;
bool bUsbMounted = false;

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *   --just_exit - do nothing; exit and reboot
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_volume() reformats /data
 * 6. erase_volume() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=/cache/some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_volume() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;
static const int BUF_SIZE = 1024*1024;

#ifndef O_BINARY
#define O_BINARY 0
#endif

struct MtdPartition {
    int device_index;
    unsigned int size;
    unsigned int erase_size;
    char *name;
};
void check_power_key_press()
{
	int key;
	while(1)
	{
		do
		{
			key = ui->WaitKey();
			if (key==KEY_POWER)
				return;
		}
		while (key!=-1);
		sleep(1);
	}
}

void handle_upgrade_callback(char *szPrompt)
{
	if (ui)
	{
		if (strcmp(szPrompt,"pause")==0)
			check_power_key_press();
		else
		{
			ui->Print(szPrompt);
			ui->Print("\n");
		}
	}
}
void handle_upgrade_progress_callback(float portion, float seconds)
{
	if (ui)
	{
		if (seconds==0)
			ui->SetProgress(portion);
		else
			ui->ShowProgress(portion,seconds);
	}
}

void init_sdboot_configs()
{
	SdBootConfigs[0].name = strdup("pcba_test");
	SdBootConfigs[0].value = strdup("1");
	SdBootConfigs[1].name = strdup("fw_update");
	SdBootConfigs[1].value = strdup("0");
	SdBootConfigs[2].name = strdup("display_lcd");
	SdBootConfigs[2].value= strdup("1");
	SdBootConfigs[3].name = strdup("display_led");
	SdBootConfigs[3].value = strdup("1");
	SdBootConfigs[4].name = strdup("demo_copy");
	SdBootConfigs[4].value = strdup("0");
	SdBootConfigs[5].name = strdup("volume_label");
	SdBootConfigs[5].value = strdup("rockchip");
}
void deinit_sdboot_configs()
{
	int i;
	for (i=0;i<6;i++)
	{
		if(SdBootConfigs[i].name)
		{
			free(SdBootConfigs[i].name);
			SdBootConfigs[i].name = NULL;
		}
		if (SdBootConfigs[i].value){
			free(SdBootConfigs[i].value);
			SdBootConfigs[i].value = NULL;
		}
	}
}


int simg2img(const char* input_path, const char *output_path)
{
    int in;
    int out;
    int i;
    int ret;
    struct sparse_file *s;

    out = open(output_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0664);
    if (out < 0) {
        fprintf(stderr, "Cannot open output file %s\n", output_path);
        return -1;
    }

    in = open(input_path, O_RDONLY | O_BINARY);
    if (in < 0) {
        fprintf(stderr, "Cannot open input file %s\n", input_path);
        return -1;
    }

    s = sparse_file_import(in, true, false);
    if (!s) {
        fprintf(stderr, "Failed to read sparse file\n");
        return -1;
    }

    lseek(out, SEEK_SET, 0);

    ret = sparse_file_write(s, out, false, false, false);
    if (ret < 0) {
        fprintf(stderr, "Cannot write output file\n");
        return -1;
    }
    sparse_file_destroy(s);
    close(in);

    close(out);

    return 0;
}

static int check_and_resize_fs(const char *dev) {
    int err;
    const char *const resize2fs_argv[] = { "/sbin/resize2fs", dev, NULL };
    /* -y Assume an answer of 'yes' to all questions; allows e2fsck to be used non-interactively. */
    const char *const e2fsck_argv[] = { "/sbin/e2fsck", "-y", "-f", dev, NULL };

    if (run(e2fsck_argv[0], (char **) e2fsck_argv)) {
        LOGE("check_and_resize_fs->error %s\n", e2fsck_argv);
        return -1;
    }

    if (run(resize2fs_argv[0], (char **) resize2fs_argv)) {
        LOGE("check_and_resize_fs->error %s\n", resize2fs_argv);
        return -1;
    }
    return 0;
}

int start_to_clone(const char *data_devname, const char *databk_devname) {

    if(simg2img(databk_devname, data_devname)){
        LOGE("null of databk ->failed to clone\n");
        return -1;
    }
    LOGI("Cloning %s to %s\n", databk_devname, data_devname);
    return 0;
}

static int clone_data_if_exist() {
    int loop_counts;
    int databk_size, data_partition_size;
    char data_devname[64];
    char databk_devname[64];
    int result=0;
    int fd,nbytes;

    // Get partitions info
    char buf[2048];
    result=mtd_find_nand();

    if(result>0){
	fd = open("/proc/mtd", O_RDONLY);
	nbytes = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[nbytes] = '\0';
	LOGI("%s", buf);

	if (mtd_scan_partitions() <= 0) {
		LOGE("clone_data_if_exist->error scanning partitions\n");
	 	return -1;
	}
	const MtdPartition *databk_partition = mtd_find_partition_by_name(DATABK_PARTITION_NAME);

	if (databk_partition == NULL) {
		LOGE("clone_data_if_exist->can't find %s partition\n", DATABK_PARTITION_NAME);    
	 	return -1;
	}
	const MtdPartition *data_partition = mtd_find_partition_by_name(DATA_PARTITION_NAME);

	if (data_partition == NULL) {
	 	LOGE("clone_data_if_exist->can't find %s partition\n", DATA_PARTITION_NAME);
	 	return -1;
	}
	sprintf(data_devname, "/dev/block/rknand_%s", data_partition->name);
	sprintf(databk_devname, "/dev/block/rknand_%s", databk_partition->name);
	   
    }else if(result==0){
	    
	fd = open("/proc/cmdline", O_RDONLY);

	nbytes = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[nbytes] = '\0';

	if (mtd_scan_cmdline() <= 0) {
	    	LOGE("clone_data_if_exist->error scanning cmdline\n");
	    	return -1;
	}

	const MtdPartitionbyCmdline *databk_partition = mtd_find_partition_by_cmdline(DATABK_PARTITION_NAME);
	if (databk_partition == NULL) {
	    	LOGE("clone_data_if_exist->can't find %s cmdline\n", DATABK_PARTITION_NAME);
	    	return -1;
	}

	const MtdPartitionbyCmdline *data_partition = mtd_find_partition_by_cmdline(DATA_PARTITION_NAME);
	if (data_partition == NULL) {
	    	LOGE("clone_data_if_exist->can't find %s cmdline\n", DATA_PARTITION_NAME);
	   	return -1;
	}

	sprintf(data_devname, "/dev/block/mmcblk0p%d", data_partition->device_index);
	sprintf(databk_devname, "/dev/block/mmcblk0p%d", databk_partition->device_index);
    }

   


    // Start to clone
    if (start_to_clone(data_devname, databk_devname)) {
        LOGE("clone_data_if_exist->error clone data\n");
        return -1;
    }
	
    return 0;
}


// open a given path, mounting partitions as necessary
FILE*
fopen_path(const char *path, const char *mode) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1, sehandle);

    FILE *fp = fopen(path, mode);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}
bool parse_config(char *pConfig,VEC_SD_CONFIG &vecItem)
{
	printf("in parse_config\n");
	stringstream configStream(pConfig);
	string strLine,strItemName,strItemValue;
	string::size_type line_size,pos;
	vecItem.clear();
	STRUCT_SD_CONFIG_ITEM item;
	while (!configStream.eof())
	{
		getline(configStream,strLine);
		line_size = strLine.size();
		if (line_size==0)
			continue;
		if (strLine[line_size-1]=='\r')
		{
			strLine = strLine.substr(0,line_size-1);
		}
		printf("%s\n",strLine.c_str());
		pos = strLine.find("=");
		if (pos==string::npos)
		{
			continue;
		}
		if (strLine[0]=='#')
		{
			continue;
		}
		strItemName = strLine.substr(0,pos);
		strItemValue = strLine.substr(pos+1);
		strItemName.erase(0,strItemName.find_first_not_of(" "));
		strItemName.erase(strItemName.find_last_not_of(" ")+1);
		strItemValue.erase(0,strItemValue.find_first_not_of(" "));
		strItemValue.erase(strItemValue.find_last_not_of(" ")+1);
		if ((strItemName.size()>0)&&(strItemValue.size()>0))
		{
			item.strKey = strItemName;
			item.strValue = strItemValue;
			vecItem.push_back(item);
		}
	}
	printf("out parse_config\n");
	return true;
	
}

bool parse_config_file(const char *pConfigFile,VEC_SD_CONFIG &vecItem)
{
	printf("in parse_config_file\n");
	FILE *file=NULL;
	file = fopen(pConfigFile,"rb");
	if( !file )
	{
		return false;
	}
	int iFileSize;
	fseek(file,0,SEEK_END);
	iFileSize = ftell(file);
	fseek(file,0,SEEK_SET);
	char *pConfigBuf=NULL;
	pConfigBuf = new char[iFileSize+1];
	if (!pConfigBuf)
	{
		fclose(file);
		return false;
	}
	memset(pConfigBuf,0,iFileSize+1);
	int iRead;
	iRead = fread(pConfigBuf,1,iFileSize,file);
	if (iRead!=iFileSize)
	{
		fclose(file);
		delete []pConfigBuf;
		return false;
	}
	fclose(file);
	bool bRet;
	bRet = parse_config(pConfigBuf,vecItem);
	delete []pConfigBuf;
	printf("out parse_config_file\n");
	return bRet;
}
int mount_usb_device()
{
	char configFile[64];
	char usbDevice[64];
	int result;
	DIR* d=NULL;
	struct dirent* de;
	d = opendir(USB_ROOT);
	if (d)
	{//check whether usb_root has  mounted
		strcpy(configFile, USB_ROOT);
		strcat(configFile, "/sd_boot_config.config");
		if (access(configFile,F_OK)==0)
		{
			closedir(d);
			return 0;
		}
		closedir(d);
	}
	else
	{
		if (errno==ENOENT)
		{
			if (mkdir(USB_ROOT,0755)!=0)
		    {
				printf("failed to create %s dir,err=%s!\n",USB_ROOT,strerror(errno));
				return -1;
			}
		}
		else
		{
			printf("failed to open %s dir,err=%s\n!",USB_ROOT,strerror(errno));
			return -1;
		}
	}

	d = opendir("/dev/block");
	if(d != NULL) {
		while(de = readdir(d)) {
			printf("/dev/block/%s\n", de->d_name);
			if((strncmp(de->d_name, "sd", 2) == 0) &&(isdigit(de->d_name[strlen(de->d_name)-1])!=0)){
				memset(usbDevice, 0, sizeof(usbDevice));
				sprintf(usbDevice, "/dev/block/%s", de->d_name);
				printf("try to mount usb device %s by vfat", usbDevice);
				result = mount(usbDevice, USB_ROOT, "vfat",
						MS_NOATIME | MS_NODEV | MS_NODIRATIME, "shortname=mixed,utf8");
				if(result != 0) {
					printf("try to mount usb device %s by ntfs\n", usbDevice);
					result = mount(usbDevice, USB_ROOT, "ntfs",
							MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
				}

				if(result == 0) {
					strcpy(USB_DEVICE_PATH,usbDevice);
					closedir(d);
					return 0;
				}
			}
		}
		closedir(d);
	}

	return -2;
}
void ensure_sd_mounted()
{
	int i;
    for(i = 0; i < 5; i++) {
		if(0 == ensure_path_mounted(EX_SDCARD_ROOT)){
			bSDMounted = true;
			break;
		}else {
			printf("delay 2sec\n");
			sleep(2);
		}
	}
}
void ensure_usb_mounted()
{
	int i;
    for(i = 0; i < 5; i++) {
		if(0 == mount_usb_device()){
			bUsbMounted = true;
			break;
		}else {
			printf("delay 2sec\n");
			sleep(2);
		}
	}
}


static bool
get_args_from_sd(int *argc, char ***argv,bool *bmalloc)
{
	printf("in get_args_from_sd\n");
	*bmalloc = false;
	ensure_sd_mounted();
	if (!bSDMounted)
	{
		printf("out get_args_from_sd:bSDMounted=false\n");
		return false;
	}
	
	char configFile[64];
	char arg[64];
	strcpy(configFile, EX_SDCARD_ROOT);
	strcat(configFile, "/sd_boot_config.config");
	VEC_SD_CONFIG vecItem;
	int i;
	if (!parse_config_file(configFile,vecItem))
	{
		printf("out get_args_from_sd:parse_config_file\n");
		return false;
	}

	*argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
	*bmalloc = true;
    (*argv)[0] = strdup("recovery");
	(*argc) = 1;

	for (i=0;i<vecItem.size();i++)
	{
		if ((strcmp(vecItem[i].strKey.c_str(),"pcba_test")==0)||
		   (strcmp(vecItem[i].strKey.c_str(),"fw_update")==0)||
		   (strcmp(vecItem[i].strKey.c_str(),"demo_copy")==0)||
		   (strcmp(vecItem[i].strKey.c_str(),"volume_label")==0))
		{
			if (strcmp(vecItem[i].strValue.c_str(),"0")!=0)
			{
				sprintf(arg,"--%s=%s",vecItem[i].strKey.c_str(),vecItem[i].strValue.c_str());
				printf("%s\n",arg);
				(*argv)[*argc] = strdup(arg);
				(*argc)++;
			}
		}
	}
	printf("out get_args_from_sd\n");
	return true;

}
static bool
get_args_from_usb(int *argc, char ***argv,bool *bmalloc)
{
	printf("in get_args_from_usb\n");
	*bmalloc = false;
	ensure_usb_mounted();
	if (!bUsbMounted)
	{
		printf("out get_args_from_usb:bUsbMounted=false\n");
		return false;
	}
	
	char configFile[64];
	char arg[64];
	strcpy(configFile, USB_ROOT);
	strcat(configFile, "/sd_boot_config.config");
	VEC_SD_CONFIG vecItem;
	int i;
	if (!parse_config_file(configFile,vecItem))
	{
		printf("out get_args_from_usb:parse_config_file\n");
		return false;
	}

	*argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
	*bmalloc = true;
    (*argv)[0] = strdup("recovery");
	(*argc) = 1;

	for (i=0;i<vecItem.size();i++)
	{
		if ((strcmp(vecItem[i].strKey.c_str(),"pcba_test")==0)||
		   (strcmp(vecItem[i].strKey.c_str(),"fw_update")==0)||
		   (strcmp(vecItem[i].strKey.c_str(),"demo_copy")==0)||
		   (strcmp(vecItem[i].strKey.c_str(),"volume_label")==0))
		{
			if (strcmp(vecItem[i].strValue.c_str(),"0")!=0)
			{
				sprintf(arg,"--%s=%s",vecItem[i].strKey.c_str(),vecItem[i].strValue.c_str());
				printf("%s\n",arg);
				(*argv)[*argc] = strdup(arg);
				(*argc)++;
			}
		}
	}
	printf("out get_args_from_usb\n");
	return true;

}



// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static bool
get_args(int *argc, char ***argv,bool *bmalloc) {
	printf("in get_args\n");
    struct bootloader_message boot;
	int iRet;
	*bmalloc = false;
    memset(&boot, 0, sizeof(boot));
    iRet = get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure
    if (iRet!=0)
		return false;
    stage = strndup(boot.stage, sizeof(boot.stage));

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
			*bmalloc = true;
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *token;
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
			*bmalloc = true;
            (*argv)[0] = strdup(argv0);  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                token = strtok(buf, "\r\n");
                if (token != NULL) {
                    (*argv)[*argc] = strdup(token);  // Strip newline.
                } else {
                    --*argc;
                }
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
	return true;
}
void free_arg(int *argc, char ***argv)
{
	int i;
	if (*argv)
	{
		for (i=0;i<*argc;i++)
		{
			if ((*argv)[i])
			{
				free((*argv)[i]);
				(*argv)[i] = NULL;
			}
		}
		free(*argv);
		*argv = NULL;
	}
}

static void
set_sdcard_update_bootloader_message(const char *package_path) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    if(package_path == NULL) {
    	strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    }else {
    	char cmd[100] = "recovery\n--update_package=";
    	strcat(cmd, package_path);
    	strlcpy(boot.recovery, cmd, sizeof(boot.recovery));
    }

    set_bootloader_message(&boot);
}

static void
set_sdcard_update_img_bootloader_message(const char *package_path) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    if(package_path == NULL) {
    	strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    }else {
    	char cmd[100] = "recovery\n--update_rkimage=";
    	strcat(cmd, package_path);
    	strlcpy(boot.recovery, cmd, sizeof(boot.recovery));
    }

    set_bootloader_message(&boot);
}

// How much of the temp log we have copied to the copy in cache.
static long tmplog_offset = 0;

static void
copy_log_file(const char* source, const char* destination, int append) {
    FILE *log = fopen_path(destination, append ? "a" : "w");
    if (log == NULL) {
        LOGE("Can't open %s\n", destination);
    } else {
        FILE *tmplog = fopen(source, "r");
        if (tmplog != NULL) {
            if (append) {
                fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            }
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            if (append) {
                tmplog_offset = ftell(tmplog);
            }
            check_and_fclose(tmplog, source);
        }
        check_and_fclose(log, destination);
    }
}

// Rename last_log -> last_log.1 -> last_log.2 -> ... -> last_log.$max
// Overwrites any existing last_log.$max.
static void
rotate_last_logs(int max) {
    char oldfn[256];
    char newfn[256];

    int i;
    for (i = max-1; i >= 0; --i) {
        snprintf(oldfn, sizeof(oldfn), (i==0) ? LAST_LOG_FILE : (LAST_LOG_FILE ".%d"), i);
        snprintf(newfn, sizeof(newfn), LAST_LOG_FILE ".%d", i+1);
        // ignore errors
        rename(oldfn, newfn);
    }
}

static void
copy_logs() {
    // Copy logs to cache so the system can find out what happened.
    copy_log_file(TEMPORARY_LOG_FILE, LOG_FILE, true);
    copy_log_file(TEMPORARY_LOG_FILE, LAST_LOG_FILE, false);
    copy_log_file(TEMPORARY_INSTALL_FILE, LAST_INSTALL_FILE, false);
    chmod(LOG_FILE, 0600);
    chown(LOG_FILE, 1000, 1000);   // system user
    chmod(LAST_LOG_FILE, 0640);
    chmod(LAST_INSTALL_FILE, 0644);
    sync();
}

// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent) {
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Save the locale to cache, so if recovery is next started up
    // without a --locale argument (eg, directly from the bootloader)
    // it will use the last-known locale.
    if (recovery_locale != NULL) {
        LOGI("Saving locale \"%s\"\n", recovery_locale);
        FILE* fp = fopen_path(LOCALE_FILE, "w");
        fwrite(recovery_locale, 1, strlen(recovery_locale), fp);
        fflush(fp);
        fsync(fileno(fp));
        check_and_fclose(fp, LOCALE_FILE);
    }

    copy_logs();

#if TARGET_BOARD_PLATFORM == rockchip
    // Reset to normal system boot so recovery won't cycle indefinitely.
    if( bNeedClearMisc )
	{
		LOGI("clear bootloader message\n");
		struct bootloader_message boot;
		memset(&boot, 0, sizeof(boot));
		set_bootloader_message(&boot);
	}
#else
	struct bootloader_message boot;
	memset(&boot, 0, sizeof(boot));
	set_bootloader_message(&boot);
#endif

 		if (bAutoUpdateComplete==true)
      	{
		      FILE *fp = fopen_path(FLAG_FILE, "w");
		      if (fp == NULL) {
		            LOGE("Can't open %s\n", FLAG_FILE);
		      	}
					char strflag[160]="success$path=";
					strcat(strflag,updatepath);
		   		if (fwrite(strflag, 1, sizeof(strflag), fp) != sizeof(strflag)) {
		      		LOGE("write %s failed! \n", FLAG_FILE);
		       }
		      fclose(fp);
		      bAutoUpdateComplete=false;
     }

    // Remove the command file, so recovery won't repeat indefinitely.
    if (ensure_path_mounted(COMMAND_FILE) != 0 ||
        (unlink(COMMAND_FILE) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    ensure_path_unmounted(CACHE_ROOT);
    sync();  // For good measure.
}

typedef struct _saved_log_file {
    char* name;
    struct stat st;
    unsigned char* data;
    struct _saved_log_file* next;
} saved_log_file;

static int
erase_volume(const char *volume) {
    bool is_cache = (strcmp(volume, CACHE_ROOT) == 0);

    saved_log_file* head = NULL;

    if (is_cache) {
        // If we're reformatting /cache, we load any
        // "/cache/recovery/last*" files into memory, so we can restore
        // them after the reformat.

        ensure_path_mounted(volume);

        DIR* d;
        struct dirent* de;
        d = opendir(CACHE_LOG_DIR);
        if (d) {
            char path[PATH_MAX];
            strcpy(path, CACHE_LOG_DIR);
            strcat(path, "/");
            int path_len = strlen(path);
            while ((de = readdir(d)) != NULL) {
                if (strncmp(de->d_name, "last", 4) == 0) {
                    saved_log_file* p = (saved_log_file*) malloc(sizeof(saved_log_file));
                    strcpy(path+path_len, de->d_name);
                    p->name = strdup(path);
                    if (stat(path, &(p->st)) == 0) {
                        // truncate files to 512kb
                        if (p->st.st_size > (1 << 19)) {
                            p->st.st_size = 1 << 19;
                        }
                        p->data = (unsigned char*) malloc(p->st.st_size);
                        FILE* f = fopen(path, "rb");
                        fread(p->data, 1, p->st.st_size, f);
                        fclose(f);
                        p->next = head;
                        head = p;
                    } else {
                        free(p);
                    }
                }
            }
            closedir(d);
        } else {
            if (errno != ENOENT) {
                printf("opendir failed: %s\n", strerror(errno));
            }
        }
    }

    ui->Print("Formatting %s...\n", volume);

    ensure_path_unmounted(volume);
    int result = format_volume(volume);

    if (is_cache) {
        while (head) {
            FILE* f = fopen_path(head->name, "wb");
            if (f) {
                fwrite(head->data, 1, head->st.st_size, f);
                fclose(f);
                chmod(head->name, head->st.st_mode);
                chown(head->name, head->st.st_uid, head->st.st_gid);
            }
            free(head->name);
            free(head->data);
            saved_log_file* temp = head->next;
            free(head);
            head = temp;
        }

        // Any part of the log we'd copied to cache is now gone.
        // Reset the pointer so we copy from the beginning of the temp
        // log.
        tmplog_offset = 0;
        copy_logs();
    }

    return result;
}

static char*
copy_sideloaded_package(const char* original_path) {
  if (ensure_path_mounted(original_path) != 0) {
    LOGE("Can't mount %s\n", original_path);
    return NULL;
  }

  if (ensure_path_mounted(SIDELOAD_TEMP_DIR) != 0) {
    LOGE("Can't mount %s\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }

  if (mkdir(SIDELOAD_TEMP_DIR, 0700) != 0) {
    if (errno != EEXIST) {
      LOGE("Can't mkdir %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
      return NULL;
    }
  }

  // verify that SIDELOAD_TEMP_DIR is exactly what we expect: a
  // directory, owned by root, readable and writable only by root.
  struct stat st;
  if (stat(SIDELOAD_TEMP_DIR, &st) != 0) {
    LOGE("failed to stat %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
    return NULL;
  }
  if (!S_ISDIR(st.st_mode)) {
    LOGE("%s isn't a directory\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }
  if ((st.st_mode & 0777) != 0700) {
    LOGE("%s has perms %o\n", SIDELOAD_TEMP_DIR, st.st_mode);
    return NULL;
  }
  if (st.st_uid != 0) {
    LOGE("%s owned by %lu; not root\n", SIDELOAD_TEMP_DIR, st.st_uid);
    return NULL;
  }

  char copy_path[PATH_MAX];
  strcpy(copy_path, SIDELOAD_TEMP_DIR);
  strcat(copy_path, "/package.zip");

  char* buffer = (char*)malloc(BUFSIZ);
  if (buffer == NULL) {
    LOGE("Failed to allocate buffer\n");
    return NULL;
  }

  size_t read;
  FILE* fin = fopen(original_path, "rb");
  if (fin == NULL) {
    LOGE("Failed to open %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }
  FILE* fout = fopen(copy_path, "wb");
  if (fout == NULL) {
    LOGE("Failed to open %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  while ((read = fread(buffer, 1, BUFSIZ, fin)) > 0) {
    if (fwrite(buffer, 1, read, fout) != read) {
      LOGE("Short write of %s (%s)\n", copy_path, strerror(errno));
      return NULL;
    }
  }

  free(buffer);

  if (fclose(fout) != 0) {
    LOGE("Failed to close %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  if (fclose(fin) != 0) {
    LOGE("Failed to close %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }

  // "adb push" is happy to overwrite read-only files when it's
  // running as root, but we'll try anyway.
  if (chmod(copy_path, 0400) != 0) {
    LOGE("Failed to chmod %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  return strdup(copy_path);
}

static const char**
prepend_title(const char* const* headers) {
    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 3;   // our title has 3 lines
    const char* const* p;
    for (p = headers; *p; ++p, ++count);

    const char** new_headers = (const char**)malloc((count+1) * sizeof(char*));
    const char** h = new_headers;
    *(h++) = "Android system recovery <" EXPAND(RECOVERY_API_VERSION) "e>";
    *(h++) = recovery_version;
    *(h++) = "";
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

static int
get_menu_selection(const char* const * headers, const char* const * items,
                   int menu_only, int initial_selection, Device* device) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui->FlushKeys();

    ui->StartMenu(headers, items, initial_selection);
    int selected = initial_selection;
    int chosen_item = -1;

    while (chosen_item < 0) {
        int key = ui->WaitKey();
        int visible = ui->IsTextVisible();

        if (key == -1) {   // ui_wait_key() timed out
            if (ui->WasTextEverVisible()) {
                continue;
            } else {
                LOGI("timed out waiting for key input; rebooting.\n");
                ui->EndMenu();
                return 0; // XXX fixme
            }
        }

        int action = device->HandleMenuKey(key, visible);

        if (action < 0) {
            switch (action) {
                case Device::kHighlightUp:
                    --selected;
                    selected = ui->SelectMenu(selected);
                    break;
                case Device::kHighlightDown:
                    ++selected;
                    selected = ui->SelectMenu(selected);
                    break;
                case Device::kInvokeItem:
                    chosen_item = selected;
                    break;
                case Device::kNoAction:
                    break;
            }
        } else if (!menu_only) {
            chosen_item = action;
        }
    }

    ui->EndMenu();
    return chosen_item;
}

static int compare_string(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static int
update_directory(const char* path, const char* unmount_when_done,
                 int* wipe_cache, Device* device) {
    ensure_path_mounted(path);

    const char* MENU_HEADERS[] = { "Choose a package to install:",
                                   path,
                                   "",
                                   NULL };
    DIR* d;
    struct dirent* de;
    d = opendir(path);
    if (d == NULL) {
        LOGE("error opening %s: %s\n", path, strerror(errno));
        if (unmount_when_done != NULL) {
            ensure_path_unmounted(unmount_when_done);
        }
        return 0;
    }

    const char** headers = prepend_title(MENU_HEADERS);

    int d_size = 0;
    int d_alloc = 10;
    char** dirs = (char**)malloc(d_alloc * sizeof(char*));
    int z_size = 1;
    int z_alloc = 10;
    char** zips = (char**)malloc(z_alloc * sizeof(char*));
    zips[0] = strdup("../");

    while ((de = readdir(d)) != NULL) {
        int name_len = strlen(de->d_name);

        if (de->d_type == DT_DIR) {
            // skip "." and ".." entries
            if (name_len == 1 && de->d_name[0] == '.') continue;
            if (name_len == 2 && de->d_name[0] == '.' &&
                de->d_name[1] == '.') continue;

            if (d_size >= d_alloc) {
                d_alloc *= 2;
                dirs = (char**)realloc(dirs, d_alloc * sizeof(char*));
            }
            dirs[d_size] = (char*)malloc(name_len + 2);
            strcpy(dirs[d_size], de->d_name);
            dirs[d_size][name_len] = '/';
            dirs[d_size][name_len+1] = '\0';
            ++d_size;
        } else if (de->d_type == DT_REG &&
                   name_len >= 4 &&
                   strncasecmp(de->d_name + (name_len-4), ".zip", 4) == 0) {
            if (z_size >= z_alloc) {
                z_alloc *= 2;
                zips = (char**)realloc(zips, z_alloc * sizeof(char*));
            }
            zips[z_size++] = strdup(de->d_name);
        }
    }
    closedir(d);

    qsort(dirs, d_size, sizeof(char*), compare_string);
    qsort(zips, z_size, sizeof(char*), compare_string);

    // append dirs to the zips list
    if (d_size + z_size + 1 > z_alloc) {
        z_alloc = d_size + z_size + 1;
        zips = (char**)realloc(zips, z_alloc * sizeof(char*));
    }
    memcpy(zips + z_size, dirs, d_size * sizeof(char*));
    free(dirs);
    z_size += d_size;
    zips[z_size] = NULL;

    int result;
    int chosen_item = 0;
    do {
        chosen_item = get_menu_selection(headers, zips, 1, chosen_item, device);

        char* item = zips[chosen_item];
        int item_len = strlen(item);
        if (chosen_item == 0) {          // item 0 is always "../"
            // go up but continue browsing (if the caller is update_directory)
            result = -1;
            break;
        } else if (item[item_len-1] == '/') {
            // recurse down into a subdirectory
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);
            new_path[strlen(new_path)-1] = '\0';  // truncate the trailing '/'
            result = update_directory(new_path, unmount_when_done, wipe_cache, device);
            if (result >= 0) break;
        } else {
            // selected a zip file:  attempt to install it, and return
            // the status to the caller.
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);

            ui->Print("\n-- Install %s ...\n", path);
//            char* copy = copy_sideloaded_package(new_path);
//            if (unmount_when_done != NULL) {
//                ensure_path_unmounted(unmount_when_done);
//            }
//            if (copy) {
            	set_sdcard_update_bootloader_message(NULL);
                result = install_package(new_path, wipe_cache, TEMPORARY_INSTALL_FILE);
//                free(copy);
//            } else {
//                result = INSTALL_ERROR;
//            }
            break;
        }
    } while (true);

    int i;
    for (i = 0; i < z_size; ++i) free(zips[i]);
    free(zips);
    free(headers);

    if (unmount_when_done != NULL) {
        ensure_path_unmounted(unmount_when_done);
    }
    return result;
}

static void
wipe_data(int confirm, Device* device) {
    if (confirm) {
        static const char** title_headers = NULL;

        if (title_headers == NULL) {
            const char* headers[] = { "Confirm wipe of all user data?",
                                      "  THIS CAN NOT BE UNDONE.",
                                      "",
                                      NULL };
            title_headers = prepend_title((const char**)headers);
        }

        const char* items[] = { " No",
                                " No",
                                " No",
                                " No",
                                " No",
                                " No",
                                " No",
                                " Yes -- delete all user data",   // [7]
                                " No",
                                " No",
                                " No",
                                NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1, 0, device);
        if (chosen_item != 7) {
            return;
        }
    }

    ui->Print("\n-- Wiping data...\n");
    device->WipeData();
    erase_volume("/data");
    erase_volume("/cache");
    ui->Print("Data wipe complete.\n");
}

static void
prompt_and_wait(Device* device, int status) {
    const char* const* headers = prepend_title(device->GetMenuHeaders());

    for (;;) {
        finish_recovery(NULL);
        switch (status) {
            case INSTALL_SUCCESS:
            case INSTALL_NONE:
                ui->SetBackground(RecoveryUI::NO_COMMAND);
                break;

            case INSTALL_ERROR:
            case INSTALL_CORRUPT:
                ui->SetBackground(RecoveryUI::ERROR);
                break;
        }
        ui->SetProgressType(RecoveryUI::EMPTY);

        int chosen_item = get_menu_selection(headers, device->GetMenuItems(), 0, 0, device);

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device->InvokeMenuItem(chosen_item);

        int wipe_cache;
        switch (chosen_item) {
            case Device::REBOOT:
                return;

            case Device::WIPE_DATA:
                wipe_data(ui->IsTextVisible(), device);
                if (!ui->IsTextVisible()) return;
                break;

            case Device::WIPE_CACHE:
                ui->Print("\n-- Wiping cache...\n");
                erase_volume("/cache");
                ui->Print("Cache wipe complete.\n");
                if (!ui->IsTextVisible()) return;
                break;

            case Device::APPLY_EXT:
                status = update_directory(EX_SDCARD_ROOT, EX_SDCARD_ROOT, &wipe_cache, device);
                if (status == INSTALL_SUCCESS && wipe_cache) {
                    ui->Print("\n-- Wiping cache (at package request)...\n");
                    if (erase_volume("/cache")) {
                        ui->Print("Cache wipe failed.\n");
                    } else {
                        ui->Print("Cache wipe complete.\n");
                    }
                }
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui->SetBackground(RecoveryUI::ERROR);
                        ui->Print("Installation aborted.\n");
                    } else if (!ui->IsTextVisible()) {
                        return;  // reboot if logs aren't visible
                    } else {
                        ui->Print("\nInstall from sdcard complete.\n");
                    }
                }
                break;

            case Device::RECOVER_SYSTEM:
            	ui->Print("\n-- Recovery system from backup...\n");
            	do_rk_backup_recovery((void *)handle_upgrade_callback,(void *)handle_upgrade_progress_callback);
            	ui->Print("Recovery system from backup complete.\n");
				if (!ui->IsTextVisible()) return;
            	break;

            case Device::APPLY_INT_RKIMG:
            	ui->Print("\n-- Update rkimage...\n");
            	char path[50];
            	strcpy(path, EX_SDCARD_ROOT);
            	strcat(path, "/update.img");
            	set_sdcard_update_img_bootloader_message(NULL);
            	status = do_update_rkimage(path);
				if (status==INSTALL_SUCCESS)
            		ui->Print(" Update rkimage complete.\n");
				else
					ui->Print(" Update rkimage failed.\n");
            	if (!ui->IsTextVisible()) return;
            	break;

            case Device::APPLY_CACHE:
                // Don't unmount cache at the end of this.
                status = update_directory(CACHE_ROOT, NULL, &wipe_cache, device);
                if (status == INSTALL_SUCCESS && wipe_cache) {
                    ui->Print("\n-- Wiping cache (at package request)...\n");
                    if (erase_volume("/cache")) {
                        ui->Print("Cache wipe failed.\n");
                    } else {
                        ui->Print("Cache wipe complete.\n");
                    }
                }
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui->SetBackground(RecoveryUI::ERROR);
                        ui->Print("Installation aborted.\n");
                    } else if (!ui->IsTextVisible()) {
                        return;  // reboot if logs aren't visible
                    } else {
                        ui->Print("\nInstall from cache complete.\n");
                    }
                }
                break;

            case Device::APPLY_ADB_SIDELOAD:
                status = apply_from_adb(ui, &wipe_cache, TEMPORARY_INSTALL_FILE);
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui->SetBackground(RecoveryUI::ERROR);
                        ui->Print("Installation aborted.\n");
                        copy_logs();
                    } else if (!ui->IsTextVisible()) {
                        return;  // reboot if logs aren't visible
                    } else {
                        ui->Print("\nInstall from ADB complete.\n");
                    }
                }
                break;
        }
    }
}

static void
print_property(const char *key, const char *name, void *cookie) {
    printf("%s=%s\n", key, name);
}

void SetSdcardRootPath(void)
{
     property_get("InternalSD_ROOT", IN_SDCARD_ROOT, "");
	   LOGI("InternalSD_ROOT: %s\n", IN_SDCARD_ROOT);
	   property_get("ExternalSD_ROOT", EX_SDCARD_ROOT, "");
	   LOGI("ExternalSD_ROOT: %s\n", EX_SDCARD_ROOT);

	   return;
}

static void
load_locale_from_cache() {
    FILE* fp = fopen_path(LOCALE_FILE, "r");
    char buffer[80];
    if (fp != NULL) {
        fgets(buffer, sizeof(buffer), fp);
        int j = 0;
        unsigned int i;
        for (i = 0; i < sizeof(buffer) && buffer[i]; ++i) {
            if (!isspace(buffer[i])) {
                buffer[j++] = buffer[i];
            }
        }
        buffer[j] = 0;
        recovery_locale = strdup(buffer);
        check_and_fclose(fp, LOCALE_FILE);
    }
}

void SureMetadataMount() {
    if (ensure_path_mounted("/metadata")) {
        printf("mount cache fail,so formate...\n");
        tmplog_offset = 0;
        format_volume("/metadata");
        ensure_path_mounted("/metadata");
    }
}


void SureCacheMount() {
	if(ensure_path_mounted("/cache")) {
		printf("mount cache fail,so formate...\n");
		tmplog_offset = 0;
		format_volume("/cache");
		ensure_path_mounted("/cache");
	}
}

void get_auto_sdcard_update_path(char **path) {
	if(!ensure_path_mounted(EX_SDCARD_ROOT)) {
		char *target = (char *)malloc(100);
		strcpy(target, EX_SDCARD_ROOT);
		strcat(target, AUTO_FACTORY_UPDATE_TAG);
		printf("auto sdcard update path: %s\n", target);
		FILE* f = fopen(target, "rb");
		if(f) {
			*path = (char *)malloc(100);
			strcpy(*path, EX_SDCARD_ROOT);
			strcat(*path, AUTO_FACTORY_UPDATE_PACKAGE);
			printf("find auto sdcard update target file %s\n", *path);
			fclose(f);
		}
		free(target);
	}
}

int handle_board_id() {
	printf("resize /system \n");
	Volume* v = volume_for_path("/system");
	int result = rk_check_and_resizefs(v->blk_device);
	if(result) {
		ui->Print("check and resize /system failed!\n");
		return result;
	}

	printf("resize /cust \n");
	Volume* v1 = volume_for_path("/cust");
	result = rk_check_and_resizefs(v1->blk_device);
	if(result) {
		ui->Print("check and resize /cust failed!\n");
		return result;
	}

	ensure_path_mounted("/cust");
	ensure_path_mounted("/system");

	result = restore();
	if(result) {
		ui->Print("restore failed!\n");
		return result;
	}

	result = custom();
	if(result) {
		ui->Print("custom failed!\n");
		return result;
	}

	//write flag for devicetest.apk
	FILE *fp_device_test = fopen("/cache/device_test", "w");
	if(fp_device_test != NULL) {
		fwrite("first_startup", 1, 13, fp_device_test);
		printf("write flag for device_test.apk\n");
		fclose(fp_device_test);
		chmod("/cache/device_test", 0666);
	}

	//cop demo files
	ensure_path_mounted("/cust");
	ensure_path_mounted("/mnt/sdcard");
	char *cmd[6];
	cmd[0] = strdup("/sbin/busybox");
	cmd[1] = strdup("cp");
	cmd[2] = strdup("-R");
	cmd[3] = strdup("cust/demo");
	cmd[4] = strdup("/mnt/sdcard/");
	cmd[5] = NULL;
	run(cmd[0], cmd);

	return 0;
}


char* findPackageAndMountUsbDevice(const char *path) {
	char *fileName = strrchr(path, '/');
	char* searchFile = (char *)malloc(128);
	sprintf(searchFile, "%s%s", USB_ROOT, fileName);
	printf("findPackageAndMountUsbDevice : searchFile = %s\n", searchFile);

	char usbDevice[64];
	DIR* d;
	struct dirent* de;
	d = opendir("/dev/block");
	if(d != NULL) {
		while(de = readdir(d)) {
			printf("/dev/block/%s\n", de->d_name);
			if(strncmp(de->d_name, "sd", 2) == 0) {
				memset(usbDevice, 0, sizeof(usbDevice));
				sprintf(usbDevice, "/dev/block/%s", de->d_name);
				printf("try to mount usb device at %s by vfat", usbDevice);
				int result = mount(usbDevice, USB_ROOT, "vfat",
						MS_NOATIME | MS_NODEV | MS_NODIRATIME, "shortname=mixed,utf8");
				if(result != 0) {
					printf("try to mount usb device %s by ntfs\n", usbDevice);
					result = mount(usbDevice, USB_ROOT, "ntfs",
							MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
				}

				if(result == 0) {
					//find update package
					if(access(searchFile, F_OK) != 0) {
						//unmount the usb device
						umount(USB_ROOT);
					}else {
						printf("find usb update package.\n");
						closedir(d);
						return searchFile;
					}
				}
			}
		}
	}

	closedir(d);
	return searchFile;
}


void
ui_print(const char* format, ...) {
    char buffer[256];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    if (ui != NULL) {
        ui->Print("%s", buffer);
    } else {
        fputs(buffer, stdout);
    }
}

ssize_t mygetline(char **lineptr, size_t *n, FILE *stream) {
	if(*n <= 0) {
		*lineptr = (char *)malloc(128);
		memset(*lineptr, 0, 128);
		*n = 128;
	}

	char c;
	char *pline = *lineptr;
	size_t count = 0;
	while((fread(&c, 1, 1, stream)) == 1) {
		if(c == '\n') {
			*pline = '\0';
			return count;
		}else if(c == '\r') {
			fread(&c, 1, 1, stream);
			if(c == '\n'){
				*pline = '\0';
				return count;
			}
		}else {
			if(count >= *n -1) {
				*lineptr = (char *)realloc(*lineptr, *n + 128);
				*n = *n + 128;
				pline = *lineptr + count;
			}

			*pline = c;
			count++;
			pline++;
		}
	}

	*pline = '\0';
	if(count == 0) {
		return -1;
	}

	return count;
}

char* readConfig(FILE *pCfgFile, char const *keyName) {
	char *keyValue = NULL;
	char *line = NULL;
	size_t len = 0;
	ssize_t read = 0;

	fseek(pCfgFile, 0, SEEK_SET);
	while((read = mygetline(&line, &len, pCfgFile)) != -1) {

		char *pstr = line;
		if(*pstr == '#' || *pstr == '\0') {
			continue;
		}
		printf("get line %s\n", pstr);
		if(strstr(pstr, keyName) != NULL) {
			char *pValue = strchr(pstr, '=');
			if(pValue != NULL) {
				keyValue = (char *)malloc(strlen(pValue) + 1);
				while(*(++pValue) == ' ');
				strcpy(keyValue, pValue);
				printf("find property %s value %s\n", keyName, keyValue);
				break;
			}
		}
	}

	printf("read config end\n");
	free(line);
	return keyValue;
}

void checkSDRemoved() {
	Volume* v = volume_for_path("/mnt/external_sd");
	char *temp;
	char *sec_dev = v->fs_options;
	if(sec_dev != NULL) {
		temp = strchr(sec_dev, ',');
		if(temp) {
			temp[0] = '\0';
		}
	}

	while(1) {
		int value2 = -1;
		int value = access(v->blk_device, 0);
		if(sec_dev) {
			value2 = access(sec_dev, 0);
		}
		if(value == -1 && value2 == -1) {
			printf("remove sdcard\n");
			break;
		}else {
			sleep(1);
		}
	}
}
void checkUSBRemoved() {
	int ret;

	while(1) {
		ret = access(USB_DEVICE_PATH, F_OK);

		if(ret==-1) {
			printf("remove USB\n");
			break;
		}else {
			sleep(1);
		}
	}
}




void *thrd_led_func(void *arg) {
	FILE * ledFd = NULL;
	bool onoff = false;

	while(isLedFlash) {
		ledFd = fopen("/sys/class/led_gpio/net_led", "w");
		if(onoff) {
			fprintf(ledFd, "%d", 0);
			onoff = false;
		}else {
			fprintf(ledFd, "%d", 1);
			onoff = true;
		}

		fclose(ledFd);
		usleep(500 * 1000);
	}

	printf("stopping led thread, close led and exit\n");
	ledFd = fopen("/sys/class/led_gpio/net_led", "w");
	fprintf(ledFd, "%d", 0);
	fclose(ledFd);
	pthread_exit(NULL);
	return NULL;
}

void startLed() {
	isLedFlash = true;
	if (pthread_create(&tid,NULL,thrd_led_func,NULL)!=0) {
		printf("Create led thread error!\n");
	}

	printf("tid in led pthread: %u.\n",tid);

}

void stopLed() {
	void *tret;
	isLedFlash = false;

	if (pthread_join(tid, &tret)!=0){
		printf("Join led thread error!\n");
	}else {
		printf("join led thread success!\n");
	}
}


#ifdef USE_RADICAL_UPDATE
static bool has_ru_pkg_been_applied()
{
    bool ret = false;
    if ( 0 == ensure_path_mounted(RU_MOUNT_POINT) )
    {
        ret = RadicalUpdate_isApplied();

        if ( 0 != ensure_path_unmounted(RU_MOUNT_POINT) )
        {
            W("fail to unmount ru_partition.");   
        }
        return ret;
    }
    else 
    {
        W("no ru_partition, no ru_pkg applied.");
        return false;
    }
}

/**
 * 恢复 system_partition 中, 可能被之前 applied ru_pkg 覆盖的 fw_files_of_ota_ver.
 * @return
 *      0, 成功; 
 *      其他, 失败. 
 */
int restore_fw_files_of_ota_ver()
{
    int ret = 0;

    CHECK_FUNC_CALL( ensure_path_mounted(SYSTEM_MOUNT_POINT) , ret, EXIT);
    CHECK_FUNC_CALL( ensure_path_mounted(RU_MOUNT_POINT) , ret, EXIT);

    CHECK_FUNC_CALL( RadicalUpdate_restoreFirmwaresInOtaVer() , ret, EXIT);
    
EXIT:
    ensure_path_unmounted(RU_MOUNT_POINT);
    ensure_path_unmounted(SYSTEM_MOUNT_POINT);

    return ret;
}
#endif

int do_resize_partition(char *name)
{
	printf("in do_resize_partition\n");
	int status = INSTALL_SUCCESS;
	int ret;
	ui->SetBackground(RecoveryUI::ERASING);
	ui->SetProgressType(RecoveryUI::INDETERMINATE);
	ui->Print("Resizing %s...\n",name);
	Volume *v = volume_for_name(name);
	if (!v)
	{
		ui->Print("no found %s partition!\n",name);
		status = INSTALL_ERROR;
	}
	else
	{
		ret = rk_check_and_resizefs(v->blk_device);
		if (ret)
		{
			ui->Print("Resizing %s failed!\n",name);
			status = INSTALL_ERROR;
		}
		else
			ui->Print("Resizing %s OK.\n",name);
	}
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}

int do_wipe_all(Device *pDev)
{
	printf("in do_wipe_all\n");
	int status = INSTALL_SUCCESS;
	ui->SetBackground(RecoveryUI::ERASING);
	ui->SetProgressType(RecoveryUI::INDETERMINATE);
    if (pDev->WipeData()) status = INSTALL_ERROR;
    // First clone /databk to /data, if faild, format /data
    if (clone_data_if_exist()) {
        if (erase_volume("/data")) status = INSTALL_ERROR;
    }
	//wipe_data must be erase /cache
    if (erase_volume("/cache")) status = INSTALL_ERROR;

#ifdef USE_BOARD_ID
    status = handle_board_id();
#else
	printf("resize /system \n");
	Volume* v = volume_for_path("/system");
	if(rk_check_and_resizefs(v->blk_device)) {
		ui->Print("check and resize /system failed!\n");
		status = INSTALL_ERROR;
	}
#endif

#ifdef USE_RADICAL_UPDATE
		LOGD("to wipe radical_update_partition. \n");
		if ( 0 != erase_volume("/radical_update") ) 
		{
			status = INSTALL_ERROR;
			LOGE("fail to wipe radical_update_partition.");
		}
#endif

    if (erase_volume(IN_SDCARD_ROOT)) status = INSTALL_ERROR;
    if (status != INSTALL_SUCCESS) ui->Print("All wipe failed.\n");
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}

int do_wipe_data(Device *pDev)
{
	printf("in do_wipe_data\n");
	int status = INSTALL_SUCCESS;
	ui->SetBackground(RecoveryUI::ERASING);
	ui->SetProgressType(RecoveryUI::INDETERMINATE);
    if (pDev->WipeData()) status = INSTALL_ERROR;
    // First clone /databk to /data, if faild, format /data
    if (clone_data_if_exist()) {
        if (erase_volume("/data")) status = INSTALL_ERROR;
    }
	//wipe_data must be erase /cache
    if (erase_volume("/cache")) status = INSTALL_ERROR;
    if (status != INSTALL_SUCCESS) ui->Print("Data wipe failed.\n");
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}
int do_wipe_cache(Device *pDev)
{
	printf("in do_wipe_cache\n");
	int status = INSTALL_SUCCESS;
	ui->SetBackground(RecoveryUI::ERASING);
	ui->SetProgressType(RecoveryUI::INDETERMINATE);
    if (pDev->WipeData()) status = INSTALL_ERROR;
    if (erase_volume("/cache")) status = INSTALL_ERROR;
    if (status != INSTALL_SUCCESS) ui->Print("Cache wipe failed.\n");
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}
int do_sd_demo_copy(char *demoPath)
{
	printf("in do_sd_demo_copy\n");
	int status = INSTALL_SUCCESS;

	if(ensure_path_mounted(IN_SDCARD_ROOT)) {
		printf("mount user partition error!\n");
		return INSTALL_ERROR;
	}
	ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
	ui->SetProgressType(RecoveryUI::INDETERMINATE);
	ui->Print("Copying demo...\n");
	//copy demo files
	char *srcPath = (char *)malloc(strlen(EX_SDCARD_ROOT) + 64);
	strcpy(srcPath, EX_SDCARD_ROOT);
	if (strcmp(demoPath,"1")==0)
		strcat(srcPath, "/Demo");
	else
		strcat(srcPath, demoPath);
	if (access(srcPath,F_OK)!=0)
	{
		ui->SetProgressType(RecoveryUI::EMPTY);
		ui->Print("Demo is not existed,demo=%s!\n",srcPath);
		free(srcPath);
		return INSTALL_ERROR;
	}

	char *args[6];
	args[0] = strdup("/sbin/busybox");
	args[1] = strdup("cp");
	args[2] = strdup("-R");
	args[3] = strdup(srcPath);
	args[4] = strdup(IN_SDCARD_ROOT);
	args[5] = NULL;

	pid_t child = fork();
	if (child == 0) {
		printf("run busybox copy demo files...\n");
		execv(args[0], &args[1]);
		fprintf(stderr, "run_program: execv failed: %s\n", strerror(errno));
		_exit(1);
	}
	int child_status;
	waitpid(child, &child_status, 0);
	if (WIFEXITED(child_status)) {
		if (WEXITSTATUS(child_status) != 0) {
			status = INSTALL_ERROR;
			fprintf(stderr, "run_program: child exited with status %d\n",
					WEXITSTATUS(child_status));
		}
	} else if (WIFSIGNALED(child_status)) {
		status = INSTALL_ERROR;
		fprintf(stderr, "run_program: child terminated by signal %d\n",
				WTERMSIG(child_status));
	}

	free(srcPath);
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}
int do_usb_demo_copy(char *demoPath)
{
	printf("in do_usb_demo_copy\n");
	int status = INSTALL_SUCCESS;

	if(ensure_path_mounted(IN_SDCARD_ROOT)) {
		printf("mount user partition error!\n");
		return INSTALL_ERROR;
	}
	ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
	ui->SetProgressType(RecoveryUI::INDETERMINATE);
	ui->Print("Copying demo...\n");
	//copy demo files
	char *srcPath = (char *)malloc(strlen(USB_ROOT) + 64);
	strcpy(srcPath, USB_ROOT);
	if (strcmp(demoPath,"1")==0)
		strcat(srcPath, "/Demo");
	else
		strcat(srcPath, demoPath);

	if (access(srcPath,F_OK)!=0)
	{
		ui->SetProgressType(RecoveryUI::EMPTY);
		ui->Print("Demo is not existed,demo=%s!\n",srcPath);
		free(srcPath);
		return INSTALL_ERROR;
	}

	char *args[6];
	args[0] = strdup("/sbin/busybox");
	args[1] = strdup("cp");
	args[2] = strdup("-R");
	args[3] = strdup(srcPath);
	args[4] = strdup(IN_SDCARD_ROOT);
	args[5] = NULL;

	pid_t child = fork();
	if (child == 0) {
		printf("run busybox copy demo files...\n");
		execv(args[0], &args[1]);
		fprintf(stderr, "run_program: execv failed: %s\n", strerror(errno));
		_exit(1);
	}
	int child_status;
	waitpid(child, &child_status, 0);
	if (WIFEXITED(child_status)) {
		if (WEXITSTATUS(child_status) != 0) {
			status = INSTALL_ERROR;
			fprintf(stderr, "run_program: child exited with status %d\n",
					WEXITSTATUS(child_status));
		}
	} else if (WIFSIGNALED(child_status)) {
		status = INSTALL_ERROR;
		fprintf(stderr, "run_program: child terminated by signal %d\n",
				WTERMSIG(child_status));
	}

	free(srcPath);
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}

int do_pcba_test(char *param)
{
	printf("in do_pcba_test\n");
	int status = INSTALL_SUCCESS;
	if(!strcmp(param, "1")) 
	{
		//pcba test
		printf("enter pcba test!\n");

		char *args[2];
		args[0] = strdup("/sbin/pcba_core");
		args[1] = NULL;

		pid_t child = fork();
		if (child == 0) {
			execv(args[0], args);
			fprintf(stderr, "run_program: execv failed: %s\n", strerror(errno));
			_exit(1);
		}
		int child_status;
		waitpid(child, &child_status, 0);
		if (WIFEXITED(child_status)) {
			if (WEXITSTATUS(child_status) != 0) {
				printf("pcba test error coder is %d \n", WEXITSTATUS(child_status));
				status = INSTALL_ERROR;
			}
		} else if (WIFSIGNALED(child_status)) {
			printf("run_program: child terminated by signal %d\n", WTERMSIG(child_status));
			status = INSTALL_ERROR;
		}
		ui->SetBackground(RecoveryUI::NONE);
		ui->Print("pcba test return.\n");
	}
	else
		status = INSTALL_ERROR;
	return status;
}
void handle_fw_path(char **ppOutFwPath,char *pInFwPath)
{
	if (strncmp(pInFwPath, "CACHE:", 6) == 0) {
        int len = strlen(pInFwPath) + 10;
        char* modified_path = (char*)malloc(len);
        strlcpy(modified_path, "/cache/", len);
        strlcat(modified_path, pInFwPath+6, len);
        printf("(replacing path \"%s\" with \"%s\")\n",
               pInFwPath, modified_path);
        *ppOutFwPath= modified_path;
    }
	else if(strncmp(pInFwPath, "/mnt/usb_storage", 16) == 0) {
    	*ppOutFwPath= findPackageAndMountUsbDevice(pInFwPath);
    }
	else
		*ppOutFwPath = strdup(pInFwPath);
}
int do_ru_package(char *pFile)
{
	printf("in do_ru_package\n");
	int status=INSTALL_SUCCESS;
	int wipe_cache;
	printf("update_ru_package = %s", pFile);
    status = install_package(pFile, &wipe_cache, TEMPORARY_INSTALL_FILE,1);
    if (status == INSTALL_SUCCESS && wipe_cache) {
        if (erase_volume("/cache")) {
            LOGE("Cache wipe failed in do_ru_package.");
        }
    }
    if (status != INSTALL_SUCCESS) {
        ui->Print("Installation aborted.\n");

        // If this is an eng or userdebug build, then automatically
        // turn the text display on if the script fails so the error
        // message is visible.
        char buffer[PROPERTY_VALUE_MAX+1];
        property_get("ro.build.fingerprint", buffer, "");
        if (strstr(buffer, ":userdebug/") || strstr(buffer, ":eng/")) {
            ui->ShowText(true);
        }
    }else {
 		bAutoUpdateComplete=true;
	}
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}

int do_update_package(char *pFile)
{
	printf("in do_update_package\n");
	int status=INSTALL_SUCCESS;
	int wipe_cache;
	printf("update_package = %s", pFile);
    status = install_package(pFile, &wipe_cache, TEMPORARY_INSTALL_FILE);
    if (status == INSTALL_SUCCESS && wipe_cache) {
        if (erase_volume("/cache")) {
            LOGE("Cache wipe failed in do_update_package.");
        }
    }
    if (status != INSTALL_SUCCESS) {
        ui->Print("Installation aborted.\n");

        // If this is an eng or userdebug build, then automatically
        // turn the text display on if the script fails so the error
        // message is visible.
        char buffer[PROPERTY_VALUE_MAX+1];
        property_get("ro.build.fingerprint", buffer, "");
        if (strstr(buffer, ":userdebug/") || strstr(buffer, ":eng/")) {
            ui->ShowText(true);
        }
    }else {
 		bAutoUpdateComplete=true;
	}
	ui->SetProgressType(RecoveryUI::EMPTY);
	return status;
}
int do_update_rkimage(char *pFile)
{
	printf("in do_update_rkimage\n");
	bool bRet;
	int status;
	ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
    printf("start update rkimage...\n");
    ui->SetProgressType(RecoveryUI::DETERMINATE);
	if (!bSDMounted)
		ensure_sd_mounted();
	if (access(pFile,F_OK)!=0)
	{
		ui->Print("%s is not existed!\n",pFile);
		return INSTALL_ERROR;
	}

	bRet= do_rk_partition_upgrade(pFile,(void *)handle_upgrade_callback,(void *)handle_upgrade_progress_callback,0,NULL);
	ui->SetProgressType(RecoveryUI::EMPTY);
    if (!bRet)
    {
    	status = INSTALL_ERROR;
		printf("Updating rkimage failed!\n");
    }
    else
    {
    	status = INSTALL_SUCCESS;
 		bAutoUpdateComplete=true;
		printf("Updating rkimage ok.\n");
    }
	return status;
}

int do_factory_mode_pcba(char *factoryModeString)
{
	printf("in do_factory_mode_pcba\n");
	int status=INSTALL_SUCCESS;
	char param[] = "1";

	status = do_pcba_test(param);
	if (status==INSTALL_SUCCESS)
		if(!strcmp(factoryModeString, "small"))
		{
			bNeedClearMisc = false;
		}
	return status;
}
int do_sd_mode_update(char *pFile)
{
	printf("in do_sd_mode_update\n");
	int status=INSTALL_SUCCESS;
	bool bRet,bUpdateIDBlock=true;

	char szDev[100];
	if(bEmmc)
		strcpy(szDev,"/dev/block/mmcblk1");
	else
		strcpy(szDev,"/dev/block/mmcblk0");
	
	char *pFwPath = (char *)malloc(100);
	strcpy(pFwPath, EX_SDCARD_ROOT);
	if (strcmp(pFile,"1")==0)
	{
		strcat(pFwPath, "/sdupdate.img");
	}
	else if (strcmp(pFile,"2")==0)
	{
		strcat(pFwPath, "/sdupdate.img");
		bUpdateIDBlock = false;
	}
	else
	{
		strcat(pFwPath, pFile);
	}
	//format user
//	erase_volume(IN_SDCARD_ROOT);
//	//format userdata
//    if (clone_data_if_exist()) {
//        if (erase_volume("/data")) return INSTALL_ERROR;
//    }
	
	ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
	ui->SetProgressType(RecoveryUI::DETERMINATE);
    ui->Print("SD upgrade...\n");

	if (access(pFwPath,F_OK)!=0)
	{
		ui->SetProgressType(RecoveryUI::EMPTY);
		ui->Print("Firmware is not existed,file=%s!\n",pFwPath);
		return INSTALL_ERROR;
	}

	if (bUpdateIDBlock)
		bRet= do_rk_firmware_upgrade(pFwPath,(void *)handle_upgrade_callback,(void *)handle_upgrade_progress_callback,szDev);
	else
		bRet = do_rk_partition_upgrade(pFwPath,(void *)handle_upgrade_callback,(void *)handle_upgrade_progress_callback,1,szDev);
	ui->SetProgressType(RecoveryUI::EMPTY);
    if (!bRet)
    {
    	status = INSTALL_ERROR;
		printf("SD upgrade failed!\n");
    }
    else
    {
    	status = INSTALL_SUCCESS;
//		if(bIfUpdateLoader)
//		{
//			bNeedClearMisc = false;
//		}
 		bAutoUpdateComplete=true;
		printf("SD upgrade ok.\n");
    }
	return status;
}
int do_usb_mode_update(char *pFile)
{
	printf("in do_usb_mode_update\n");
	int status=INSTALL_SUCCESS;
	bool bRet,bUpdateIDBlock=true;
	char szDev[100];
	char *pFwPath = (char *)malloc(100);
	strcpy(pFwPath, USB_ROOT);
	if (strcmp(pFile,"1")==0)
	{
		strcat(pFwPath, "/sdupdate.img");
	}
	else if (strcmp(pFile,"2")==0)
	{
		strcat(pFwPath, "/sdupdate.img");
		bUpdateIDBlock = false;
	}
	else
	{
		strcat(pFwPath, pFile);
	}
	//format user
//	erase_volume(IN_SDCARD_ROOT);
//	//format userdata
//    if (clone_data_if_exist()) {
//        if (erase_volume("/data")) return INSTALL_ERROR;
//    }
	strcpy(szDev,USB_DEVICE_PATH);
	if (strlen(szDev)>0)
		szDev[strlen(szDev)-1]=0;
	
	ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
	ui->SetProgressType(RecoveryUI::DETERMINATE);
    ui->Print("UDisk upgrade...\n");
	if (access(pFwPath,F_OK)!=0)
	{
		ui->SetProgressType(RecoveryUI::EMPTY);
		ui->Print("Firmware is not existed,file=%s!\n",pFwPath);
		return INSTALL_ERROR;
	}

	if (bUpdateIDBlock)
		bRet= do_rk_firmware_upgrade(pFwPath,(void *)handle_upgrade_callback,(void *)handle_upgrade_progress_callback,szDev);
	else
		bRet = do_rk_partition_upgrade(pFwPath,(void *)handle_upgrade_callback,(void *)handle_upgrade_progress_callback,2,szDev);
	ui->SetProgressType(RecoveryUI::EMPTY);
    if (!bRet)
    {
    	status = INSTALL_ERROR;
		printf("USB upgrade failed!\n");
    }
    else
    {
    	status = INSTALL_SUCCESS;
//		if(bIfUpdateLoader)
//		{
//			bNeedClearMisc = false;
//		}
 		bAutoUpdateComplete=true;
		printf("USB upgrade ok.\n");
    }
		
	return status;
}


void print_arg(int argc,char**argv)
{
	printf("list arg=%d:\n",argc);
	int i;
	for (i=0;i<argc;i++)
		printf("%s\n",argv[i]);
}
int check_sdboot()
{
    char param[1024];
    int fd, ret;
    char *s=NULL;
    
    memset(param,0,1024);
    fd= open("/proc/cmdline", O_RDONLY);
    ret = read(fd, (char*)param, 1024);
	printf("cmdline=%s\n",param);
    s = strstr(param,"sdfwupdate");
    if(s!= NULL)
		return 0;
    else
		return -1;
}
int check_usbboot()
{
    char param[1024];
    int fd, ret;
    char *s=NULL;
    
    memset(param,0,1024);
    fd= open("/proc/cmdline", O_RDONLY);
    ret = read(fd, (char*)param, 1024);
	printf("cmdline=%s\n",param);
    s = strstr(param,"usbfwupdate");
    if(s!= NULL)
		return 0;
    else
		return -1;
}


int main(int argc, char **argv) {
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);

#ifdef TARGET_RK30
    freopen("/dev/ttyFIQ0", "a", stdout); setbuf(stdout, NULL);
    freopen("/dev/ttyFIQ0", "a", stderr); setbuf(stderr, NULL);
#else
    freopen("/dev/ttyS1", "a", stdout); setbuf(stdout, NULL);
    freopen("/dev/ttyS1", "a", stderr); setbuf(stderr, NULL);
#endif
	struct selinux_opt seopts[] = {
		  { SELABEL_OPT_PATH, "/file_contexts" }
		};
	char *send_intent = NULL;
	char *fw_path=NULL;
	int arg;
	bool bFreeArg=false;
	bool bSDBoot=false;
	bool bUsbBoot=false;
	bool bRet,bFactoryMode = false;
	int status = INSTALL_SUCCESS;
	int st_cur, st_max;
	Device* device = make_device();
    // If this binary is started with the single argument "--adbd",
    // instead of being the normal recovery binary, it turns into kind
    // of a stripped-down version of adbd that only supports the
    // 'sideload' command.  Note this must be a real argument, not
    // anything in the command file or bootloader control block; the
    // only way recovery should be run with this argument is when it
    // starts a copy of itself from the apply_from_adb() function.
    if (argc == 2 && strcmp(argv[1], "--adbd") == 0) {
        adb_main();
        return 0;
    }
	//sleep(2);//sleep for showing debug info normally
    printf("Starting recovery on %s", ctime(&start));
	
	if(check_sdboot()==0)
		bSDBoot = true;
	else if(check_usbboot()==0)
		bUsbBoot = true;
    load_volume_table();
	SetSdcardRootPath();
    ensure_path_mounted(LAST_LOG_FILE);
    rotate_last_logs(10);

	
//initialize user interface
	if (recovery_locale == NULL) {
        load_locale_from_cache();
    }
    
    ui = device->GetUI();
    ui->Init();

	
    if ((stage != NULL) && (sscanf(stage, "%d/%d", &st_cur, &st_max) == 2) )
	{
        ui->SetStage(st_cur, st_max);
    }  

    ui->SetLocale(recovery_locale);

	ui->Print("Recovery system v4.4.2 \n\n");
	printf("Recovery system v4.4.2 \n");

	if (bSDBoot)
	{
		bRet = get_args_from_sd(&argc,&argv,&bFreeArg);
		bNeedClearMisc = false;
	}
	else if(bUsbBoot)
	{
		bRet = get_args_from_usb(&argc,&argv,&bFreeArg);
		bNeedClearMisc = false;
	}
	else
	{
    	bRet = get_args(&argc, &argv,&bFreeArg);
		bNeedClearMisc = true;
	}

	if (!bRet)
	{
		status = INSTALL_ERROR;
		goto Exit_Main;
	}
	print_arg(argc,argv);

    sehandle = selabel_open(SELABEL_CTX_FILE, seopts, 1);

    if (!sehandle) {
        ui->Print("Warning: No file_contexts\n");
    }
	char bootmode[256];
    property_get("ro.bootmode", bootmode, "unknown");
	if(!strcmp(bootmode, "emmc"))
		bEmmc = true;
	else
		bEmmc = false;
    printf("bootmode = %s \n", bootmode);
    property_get("UserVolumeLabel", gVolume_label, "");

	property_list(print_property, NULL);
    property_get("ro.build.display.id", recovery_version, "");
    // D_STR(recovery_version);

    device->StartRecovery();
    SureCacheMount();
    SureMetadataMount();

	

	while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'f': //factory_mode
        	bFactoryMode = true;
			status = do_factory_mode_pcba(optarg);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
        case 's': //send_intent
        	send_intent = strdup(optarg);
			goto Exit_Main;
			break;
        case 'u': //update_package
        	handle_fw_path(&fw_path,optarg);
			strcpy(updatepath,fw_path);
			status = do_update_package(fw_path);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
		case 'z': //update_ru_package
        	handle_fw_path(&fw_path,optarg);
			strcpy(updatepath,fw_path);
			status = do_ru_package(fw_path);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
        case 'r': //update_rkimage
        	handle_fw_path(&fw_path,optarg);
			strcpy(updatepath,fw_path);
			status = do_update_rkimage(fw_path);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
        case 'w'://wipe_data
        	status = do_wipe_data(device);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
        case 'c'://wipe_cache
        	status = do_wipe_cache(device);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
        case 't': //show_text
        	ui->ShowText(true);
			break;
        case 'w'+'a'://wipe_all
        	ui->ShowText(true);
			status = do_wipe_all(device);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
        case 'x': //just_exit
        	status = INSTALL_NONE;
        	ui->SetBackground(RecoveryUI::NO_COMMAND);
			goto Exit_Main;
			break;
        case 'l': //locale
        	recovery_locale = strdup(optarg);
			ui->SetLocale(recovery_locale);
			break;
        case 'g': //stages
        {
            if ((stage == NULL) || (*stage == '\0'))
			{
                char buffer[20] = "1/";
                strlcat(buffer, optarg, sizeof(buffer)-3);
                stage = strdup(buffer);
				if (sscanf(stage, "%d/%d", &st_cur, &st_max) == 2) 
				{
			        ui->SetStage(st_cur, st_max);
			    }  
            }
            break;
        }
			
		case 'p': //pcba_test
			status = do_pcba_test(optarg);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
		case 'f'+'w': //fw_update
			if((optarg)&&(!fw_path))
				fw_path = strdup(optarg);
			if (bSDBoot)
				status = do_sd_mode_update(optarg);
			else
				status = do_usb_mode_update(optarg);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
		case 'd': //demo_copy
			if (bSDBoot)
				status = do_sd_demo_copy(optarg);
			else
				status = do_usb_demo_copy(optarg);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
		case 'v': //volume_label
			strcpy(gVolume_label,optarg);
			break;
		case 'r'+'p'://resize partition
			status = do_resize_partition(optarg);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
			break;
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    //check auto-update function,when not doing update before.
	if(!fw_path)
	{
		get_auto_sdcard_update_path(&fw_path);
		if (fw_path)
		{
			status = do_update_rkimage(fw_path);
			if (status!=INSTALL_SUCCESS)
				goto Exit_Main;
		}
	}
Exit_Main:
    if (status == INSTALL_ERROR || status == INSTALL_CORRUPT) {
        copy_logs();
        ui->SetBackground(RecoveryUI::ERROR);
    }
	if (bSDBoot)
	{
		ui->ShowText(true);
		if (status==INSTALL_SUCCESS)
			ui->Print("Doing Actions succeeded.please remove the sdcard......\n");
		else
			ui->Print("Doing Actions failed!please remove the sdcard......\n");
		if (bSDMounted)
			checkSDRemoved();
		else
			check_power_key_press();
	}
	else if (bUsbBoot)
	{
		ui->ShowText(true);
		if (status==INSTALL_SUCCESS)
			ui->Print("Doing Actions succeeded.please remove the usb disk......\n");
		else
			ui->Print("Doing Actions failed!please remove the usb disk......\n");
		if (bUsbMounted)
			checkUSBRemoved();
		else
			check_power_key_press();
	}
	else
	{
		if (!bFactoryMode)
		{
			if ((status != INSTALL_SUCCESS)||(argc==1)) 
			{
#ifdef USE_RADICAL_UPDATE
				if (argc==1)
				{
                    if ( has_ru_pkg_been_applied() )
                    {
                        I("a ru_pkg has been applied, to restore backup_of_fw_files_of_ota_ver to system_partition.");

                        ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
                        ui->ShowText(true);
                        ui->Print("Try to roll GPU driver back to version before ru_pkg installed. \n");

                        if ( 0 == restore_fw_files_of_ota_ver() )
                        {
                            I("success to restore fws_in_ota_ver to system_partition.");
                            ui->Print("Success to roll GPU driver back! Device will reboot. \n");
                            status = INSTALL_SUCCESS;
                            sleep(2);
                        }
                        else
                        {
                            W("fail to restore fw_files_of_ota_ver, to prompt user and wait.");
                            status = INSTALL_ERROR;
                            ui->Print("Rolling back GPU driver failed! \n");
		                    prompt_and_wait(device, status);
                        }
                    }
                    else
                    {
                        W("no ru_pkg has been applied.");
		                prompt_and_wait(device, status);
                    }
				}
                else 
                {
		            prompt_and_wait(device, status);
                }
#else
		        prompt_and_wait(device, status);
#endif
		    }
		}
	}
    
    // Otherwise, get ready to boot the main system...
   
    finish_recovery(send_intent);
	
	if (bFreeArg)
		free_arg(&argc, &argv);
	if (stage)
		free(stage);
	if (recovery_locale)
		free(recovery_locale);
	if (send_intent)
		free(send_intent);
	if (fw_path)
		free(fw_path);

    ui->Print("Rebooting...\n");
    //property_set(ANDROID_RB_PROPERTY, "reboot,");
	android_reboot(ANDROID_RB_RESTART, 0, 0);
    return EXIT_SUCCESS;
}
