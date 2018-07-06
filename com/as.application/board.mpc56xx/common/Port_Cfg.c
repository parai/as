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
#include "Port.h"
/* ============================ [ MACROS    ] ====================================================== */
/* ============================ [ TYPES     ] ====================================================== */
/* ============================ [ DECLARES  ] ====================================================== */
/* ============================ [ DATAS     ] ====================================================== */
static const Port_PadConfigType PortPadConfig[] =
{
	/* -------- CAN A -------------- */
	{	/* CAN_A_TX_SCI_A_TX_GPIO as CAN A TX*/
		.pinId = 83,
		.level = 0,
		.regV = 0x62C
	},
	{	/* CAN_A_RX_SCI_A_RX_GPIO as CAN A RX */
		.pinId = 84,
		.level = 0,
		.regV = 0x500
	},
	/* -------- LED -------------- */
	{	/* eMIOS[9]_eTPU_A[9]_SCI_B_RX_GPIO as GPIO for LED1 */
		.pinId = 188,
		.level = STD_LOW,
		.regV = 0x200
	},
	{	/* eMIOS[10:11]_GPIO[189:190] as GPIO for LED2 */
		.pinId = 189,
		.level = STD_HIGH,
		.regV = 0x200
	},
	{	/* eMIOS[10:11]_GPIO[189:190] as GPIO for LED3 */
		.pinId = 190,
		.level = STD_LOW,
		.regV = 0x200
	},
	{	/* eMIOS[12]_DSPI_C_SOUT_eTPU_A[27]_GPIO[191] for LED4 */
		.pinId = 191,
		.level = STD_HIGH,
		.regV = 0x200
	},
	/* -------- KEY -------------- */
	{	/* eMIOS[0]_eTPU_A[0]_eTPU_A[25]_GPIO[179] as GPIO for KEY1 */
		.pinId = 179,
		.level = 0,
		.regV = 0x100
	},
	{	/* eMIOS[2]_eTPU_A[2]_GPIO[181] as GPIO for KEY2 */
		.pinId = 181,
		.level = 0,
		.regV = 0x100
	},
	{	/* eMIOS[4]_eTPU_A[4]_GPIO[183] as GPIO for KEY3 */
		.pinId = 183,
		.level = 0,
		.regV = 0x100
	},
	{	/* eMIOS[8]_eTPU_A[8]_SCI_B_TX_GPIO[187] */
		.pinId = 187,
		.level = 0,
		.regV = 0x100
	},
};
const Port_ConfigType PortConfigData =
{
	.padCnt = sizeof(PortPadConfig)/sizeof(Port_PadConfigType),
	.padConfig = PortPadConfig,
};
/* ============================ [ LOCALS    ] ====================================================== */
/* ============================ [ FUNCTIONS ] ====================================================== */

