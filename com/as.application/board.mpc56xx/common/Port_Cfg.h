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

#ifndef _PORT_CFG_H_
#define _PORT_CFG_H_
/* ============================ [ INCLUDES  ] ====================================================== */
/* ============================ [ MACROS    ] ====================================================== */
/* ============================ [ TYPES     ] ====================================================== */
typedef struct
{
	uint8 pinId;
	uint8 level; /* default output level for GPIO */
	uint16 regV;
} Port_PadConfigType;

typedef struct
{
	uint16_t padCnt;
	const Port_PadConfigType * padConfig;
} Port_ConfigType;

/* ============================ [ DECLARES  ] ====================================================== */
extern const Port_ConfigType PortConfigData;
/* ============================ [ DATAS     ] ====================================================== */
/* ============================ [ LOCALS    ] ====================================================== */
/* ============================ [ FUNCTIONS ] ====================================================== */
#endif /* _PORT_CFG_H_ */
