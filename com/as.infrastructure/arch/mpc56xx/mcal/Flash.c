/**
 * AS - the open source Automotive Software on https://github.com/parai
 *
 * Copyright (C) 2015  AS <parai@foxmail.com>
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
#define FLASH_IS_ERASE_ADDRESS_ALIGNED(a) ( \
		(0x00000000 == (a)) || \
		(0x00004000 == (a)) || \
		(0x00008000 == (a)) || \
		(0x00010000 == (a)) || \
		(0x00018000 == (a)) || \
		(0x0001C000 == (a)) || \
		(0x00020000 == (a)) || \
		(0x00030000 == (a)) || \
		(0x00040000 == (a)) || \
		(0x00060000 == (a)) || \
		(0x00080000 == (a)) || \
		(0x000A0000 == (a)) || \
		(0x000C0000 == (a)) || \
		(0x000E0000 == (a)) || \
		(0x00100000 == (a)) || \
		(0x00120000 == (a)) || \
		(0x00140000 == (a)) || \
		(0x00160000 == (a)) || \
		(0x00180000 == (a))    \
	)

#ifndef FLASH_WRITE_SIZE
#define FLASH_WRITE_SIZE 8
#endif

#include "Flash.h"
#include "mpc56xx.h"
#include "asdebug.h"
/* ============================ [ MACROS    ] ====================================================== */
#if FLASH_WRITE_SIZE%8 != 0
#error FLASH_WRITE_SIZE must be n times of 8
#endif

#ifndef FLS_START_ADDR
#define FLS_START_ADDR 0x00040000
#endif
#ifndef FLS_END_ADDR
#define FLS_END_ADDR   0x00180000
#endif
/* no "=" to FLS_END_ADDR, intended, so can't program the last 8 bytes */
#define IS_FLASH_ADDRESS(a) (((a) < FLS_END_ADDR) && ((a) >= FLS_START_ADDR))

#define FLS_ADDRESS_TO_BLOCK_NUM(addr, num) \
	switch(addr)			\
	{						\
	case 0x00000000:		\
		num = 0;			\
		break;				\
	case 0x00004000:		\
		num = 1;			\
		break;				\
	case 0x00008000:		\
		num = 2;			\
		break;				\
	case 0x00010000:		\
		num = 3;			\
		break;				\
	case 0x00018000:		\
		num = 4;			\
		break;				\
	case 0x0001C000:		\
		num = 5;			\
		break;				\
	case 0x00020000:		\
		num = 6;			\
		break;				\
	case 0x00030000:		\
		num = 7;			\
		break;				\
	case 0x00040000:		\
		num = 8;			\
		break;				\
	case 0x00060000:		\
		num = 9;			\
		break;				\
	case 0x00080000:		\
		num = 10;			\
		break;				\
	case 0x000A0000:		\
		num = 11;			\
		break;				\
	case 0x000C0000:		\
		num = 12;			\
		break;				\
	case 0x000E0000:		\
		num = 13;			\
		break;				\
	case 0x00100000:		\
		num = 14;			\
		break;				\
	case 0x00120000:		\
		num = 15;			\
		break;				\
	case 0x00140000:		\
		num = 16;			\
		break;				\
	case 0x00160000:		\
		num = 17;			\
		break;				\
	default:				\
		num = -1;			\
		break;				\
	}

#define FLS_MIN_BLOCK_SIZE (16*1024)

#define FLS_ADDRESS_TO_HW(addr, hw) \
	if((addr) < 0x00080000)			\
	{								\
		hw = &(CFLASH0);			\
	}								\
	else if((addr) < 0x00100000)	\
	{								\
		hw = &(CFLASH1);			\
	}								\
	else							\
	{								\
		hw = &(CFLASH2);			\
	}

#define AS_LOG_FLS 0
/* ============================ [ TYPES     ] ====================================================== */
/* ============================ [ DECLARES  ] ====================================================== */
/* ============================ [ DATAS     ] ====================================================== */
const tFlashHeader FlashHeader =
{
	.Info.W.MCU     = 1,
	.Info.W.mask    = 2,
	.Info.W.version = 169,
	.Init      = FlashInit,
	.Deinit    = FlashDeinit,
	.Erase     = FlashErase,
	.Write     = FlashWrite,
	.Read      = FlashRead
};
/* ============================ [ LOCALS    ] ====================================================== */
/* ============================ [ FUNCTIONS ] ====================================================== */
void FlashInit(tFlashParam* FlashParam)
{
	if ( (FLASH_DRIVER_VERSION_PATCH == FlashParam->patchlevel) ||
		 (FLASH_DRIVER_VERSION_MINOR == FlashParam->minornumber) ||
		 (FLASH_DRIVER_VERSION_MAJOR == FlashParam->majornumber) )
	{
		FlashParam->errorcode = kFlashOk;
	}
	else
	{
		FlashParam->errorcode = kFlashFailed;
	}
}

void FlashDeinit(tFlashParam* FlashParam)
{
	/*  TODO: Deinit Flash Controllor */
	FlashParam->errorcode = kFlashOk;
}

void FlashErase(tFlashParam* FlashParam)
{
	tAddress address;
	tLength  length;
	uint32 num;
	uint32 tmp;
	volatile struct FLASH_tag * hw;
	if ( (FLASH_DRIVER_VERSION_PATCH == FlashParam->patchlevel) ||
		 (FLASH_DRIVER_VERSION_MINOR == FlashParam->minornumber) ||
		 (FLASH_DRIVER_VERSION_MAJOR == FlashParam->majornumber) )
	{
		length = FlashParam->length;
		address = FlashParam->address;
		if ( (FALSE == FLASH_IS_ERASE_ADDRESS_ALIGNED(address)) ||
			 (FALSE == IS_FLASH_ADDRESS(address)) )
		{
			FlashParam->errorcode = kFlashInvalidAddress;
		}
		else if( (FALSE == IS_FLASH_ADDRESS(address+length)) ||
				 (FALSE == FLASH_IS_ERASE_ADDRESS_ALIGNED(address+length)) )
		{
			FlashParam->errorcode = kFlashInvalidSize;
		}
		else
		{
			while((length > 0) && (kFlashOk == FlashParam->errorcode))
			{
				FLS_ADDRESS_TO_HW(address, hw);
				FLS_ADDRESS_TO_BLOCK_NUM(address, num);
				/* erase one block */
				hw->MCR.R = 0x00000004; /* Set ERS in MCR: Select ERS Operation */
				if(num < 8)
				{
					hw->LMSR.R = (1<<num);
					hw->HSR.R  = 0;
				}
				else if(num < 10)
				{
					hw->LMSR.R = (1<<(num+8));
					hw->HSR.R  = 0;
				}
				else if(num < 14)
				{
					hw->LMSR.R = 0;
					hw->HSR.R  = (1<<(num-10));
				}
				else if(num < 18)
				{
					hw->LMSR.R = 0;
					hw->HSR.R  = (1<<(num-14));
				}
				ASLOG(FLS, "erase block %d LMSR=%X HSR=%X @%p\n",
						num, hw->LMSR.R, hw->HSR.R, hw);
				/* Latch a Flash Address with any data */
				*(volatile uint32*)address = 0xFFFFFFFF;
				hw->MCR.R = 0x00000005; /* Set EHV in MCR: Operation Start */

				do
				{ /* wait till DONE=1 */
					tmp=hw->MCR.R;
				} while ( 0 == (tmp & 0x00000400) );
				tmp=hw->MCR.R;

				hw->MCR.R = 0x00000004; /* Reset EHV in MCR: Operation End */
				hw->MCR.R = 0x00000000; /* Reset ERS in MCR: Deselect Operation */

				if(0 == (tmp&0x00000200)) /* Check PEG flag */
				{
					ASLOG(FLS, "Not Erase Good, MCR=%X\n", tmp);
					FlashParam->errorcode = kFlashFailed;
					break;
				}
				num = 0;
				do
				{	/* locate next starting address */
					address += FLS_MIN_BLOCK_SIZE;
					length  -= FLS_MIN_BLOCK_SIZE;
					num += FLS_MIN_BLOCK_SIZE;
				} while(FALSE == FLASH_IS_ERASE_ADDRESS_ALIGNED(address));
				/* check all FF */
				for(tmp = address-num; tmp < (address); tmp+=4)
				{
					if((*(volatile uint32*)tmp) != 0xFFFFFFFF)
					{
						FlashParam->errorcode = kFlashFailed;
						break;
					}
				}
			}
		}
	}
	else
	{
		FlashParam->errorcode = kFlashFailed;
	}
}

void FlashWrite(tFlashParam* FlashParam)
{
	tAddress address;
	tLength  length;
	tData*    data;
	uint32 tmp;
	volatile struct FLASH_tag * hw;
	if ( (FLASH_DRIVER_VERSION_PATCH == FlashParam->patchlevel) ||
		 (FLASH_DRIVER_VERSION_MINOR == FlashParam->minornumber) ||
		 (FLASH_DRIVER_VERSION_MAJOR == FlashParam->majornumber) )
	{
		length = FlashParam->length;
		address = FlashParam->address;
		data = FlashParam->data;
		if ( (FALSE == FLASH_IS_WRITE_ADDRESS_ALIGNED(address)) ||
			 (FALSE == IS_FLASH_ADDRESS(address)) )
		{
			FlashParam->errorcode = kFlashInvalidAddress;
		}
		else if( (FALSE == IS_FLASH_ADDRESS(address+length)) ||
				 (FALSE == FLASH_IS_WRITE_ADDRESS_ALIGNED(length)) )
		{
			FlashParam->errorcode = kFlashInvalidSize;
		}
		else if( NULL == data )
		{
			FlashParam->errorcode = kFlashInvalidData;
		}
		else
		{
			while((length > 0) && (kFlashOk == FlashParam->errorcode))
			{
				FLS_ADDRESS_TO_HW(address, hw);
				hw->MCR.R = 0x00000010; /* Set PGM in MCR: Select PGM Operation */
				*(volatile uint32*)address = data[0];
				*(volatile uint32*)(address+4) = data[1];
				hw->MCR.R = 0x00000011; /* Set EHV in MCR: Operation Start */
				ASLOG(FLS, "program @%X %08X%08X @%p\n",
						address, data[0], data[1], hw);
				do
				{ /* wait till DONE=1*/
					tmp=hw->MCR.R;
				} while ( 0 == (tmp & 0x00000400) );
				tmp=hw->MCR.R;

				hw->MCR.R = 0x00000010; /* Reset EHV in MCR: Operation End */
				hw->MCR.R = 0x00000000; /* Reset ERS in MCR: Deselect Operation */

				if( (0 == (tmp&0x00000200)) || /* Check PEG flag */
					(*(volatile uint32*)address != data[0]) ||
					(*(volatile uint32*)(address+4) != data[1]) )
				{
					ASLOG(FLS, "Not Program Good, MCR=%X\n", tmp);
					FlashParam->errorcode = kFlashFailed;
				}
				else
				{
					data += 2;
					address += 8;
					length -= 8;
				}
			}
		}
	}
	else
	{
		FlashParam->errorcode = kFlashFailed;
	}
}

void FlashRead(tFlashParam* FlashParam)
{
	tAddress address;
	tLength  length;
	tData*    data;
	if ( (FLASH_DRIVER_VERSION_PATCH == FlashParam->patchlevel) ||
		 (FLASH_DRIVER_VERSION_MINOR == FlashParam->minornumber) ||
		 (FLASH_DRIVER_VERSION_MAJOR == FlashParam->majornumber) )
	{
		length = FlashParam->length;
		address = FlashParam->address;
		data = FlashParam->data;
		if ( (FALSE == FLASH_IS_READ_ADDRESS_ALIGNED(address)) ||
			 (FALSE == IS_FLASH_ADDRESS(address)) )
		{
			FlashParam->errorcode = kFlashInvalidAddress;
		}
		else if( (FALSE == IS_FLASH_ADDRESS(address+length)) ||
				 (FALSE == FLASH_IS_READ_ADDRESS_ALIGNED(length)) )
		{
			FlashParam->errorcode = kFlashInvalidSize;
		}
		else if( NULL == data )
		{
			FlashParam->errorcode = kFlashInvalidData;
		}
		else
		{
			tLength i;
			char* dst = (char*) data;
			const char* src = (const char*) address;

			for(i=0;i<length;i++)
			{
				dst[i] = src[i];
			}

			FlashParam->errorcode = kFlashOk;
		}
	}
	else
	{
		FlashParam->errorcode = kFlashFailed;
	}
}
