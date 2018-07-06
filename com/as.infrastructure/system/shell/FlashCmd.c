/**
 * AS - the open source Automotive Software on https://github.com/parai
 *
 * Copyright (C) 2018  AS <parai@foxmail.com>
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation; See <http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
/* ============================ [ INCLUDES  ] ====================================================== */
#include "Std_Types.h"
#include "shell.h"
#include "Flash.h"
#include "asdebug.h"
/* ============================ [ MACROS    ] ====================================================== */
/* ============================ [ TYPES     ] ====================================================== */
#ifndef FLASH_CMD_MAX_DATA
#define FLASH_CMD_MAX_DATA 32
#endif

#define AS_LOG_FLASH 0
/* ============================ [ DECLARES  ] ====================================================== */
static int shellFlash(int argc, char *argv[] );
/* ============================ [ DATAS     ] ====================================================== */
SHELL_CONST ShellCmdT cmdFlash  = {
		shellFlash,
		3,3,
		"flash",
		"flash erase/write addr size/data",
		"  erase or write flash memory at addr\n"
		"  flash erase addr size\n"
		"  flash write addr data\n"
		"  example: flash write 4000 1122334455667788\n",
		{NULL,NULL}
};
SHELL_CMD_EXPORT(cmdFlash)

static tFlashParam cmdFlashParam =
{
    .patchlevel  = FLASH_DRIVER_VERSION_PATCH,
    .minornumber = FLASH_DRIVER_VERSION_MINOR,
    .majornumber = FLASH_DRIVER_VERSION_MAJOR,
    .wdTriggerFct = NULL,
    .errorcode   = 0xFF,
};
/* ============================ [ LOCALS    ] ====================================================== */
static int shellFlash(int argc, char *argv[] ) {
	int rv = 0;
	char tmp[3];
	char* pStr;
	uint8* pData;
	size_t len;
	static uint32 data[FLASH_CMD_MAX_DATA/sizeof(uint32)];
	(void)argc;

	if(0xFF == cmdFlashParam.errorcode)
	{
		FlashInit(&cmdFlashParam);
		rv = -cmdFlashParam.errorcode;
	}

	cmdFlashParam.errorcode = kFlashOk;

	cmdFlashParam.address  = strtoul(argv[2], NULL, 16);
	cmdFlashParam.data  = data;

	if(0 == strcmp(argv[1], "erase"))
	{
		cmdFlashParam.length = strtoul(argv[3], NULL, 16);
		ASLOG(FLASH, "erase %08X %08X\n", cmdFlashParam.address, cmdFlashParam.length);
		FlashErase(&cmdFlashParam);
		rv = -cmdFlashParam.errorcode;
	}
	else if(0 == strcmp(argv[1], "write"))
	{
		len = strlen(argv[3]);
		if( (len <= (sizeof(data)*2)) && (0==(len&0x01)))
		{
			memset(data, 0xFF, sizeof(data));
			pData = (uint8*)data;
			pStr = argv[3];
			tmp[2] = '\0';
			while(*pStr != '\0')
			{
				tmp[0] = pStr[0];
				tmp[1] = pStr[1];
				*pData = strtoul(tmp, NULL, 16);
				pStr += 2;
				pData ++;
			}
			ASLOG(FLASH, "write %08X (len=%d) %08X%08X...\n",
					cmdFlashParam.address, len/2,
					cmdFlashParam.data[0],cmdFlashParam.data[1]);
			cmdFlashParam.length = len/2;
			FlashWrite(&cmdFlashParam);
			rv = -cmdFlashParam.errorcode;
		}
		else
		{
			rv = -11;
		}
	}
	else
	{
		rv = -44;
	}
	return rv;
}
/* ============================ [ FUNCTIONS ] ====================================================== */

