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
#ifndef _DIO_CFG_H_
#define _DIO_CFG_H_
/* ============================ [ INCLUDES  ] ====================================================== */
/* ============================ [ MACROS    ] ====================================================== */
#define DIO_DEV_ERROR_DETECT STD_OFF

#define DIO_PIN_LED1 188
#define DIO_PIN_LED2 189
#define DIO_PIN_LED3 190
#define DIO_PIN_LED4 191

#define DIO_PIN_KEY1 179
#define DIO_PIN_KEY2 181
#define DIO_PIN_KEY3 183
#define DIO_PIN_KEY4 187
/* ============================ [ TYPES     ] ====================================================== */
/* ============================ [ DECLARES  ] ====================================================== */
/* ============================ [ DATAS     ] ====================================================== */
extern const Dio_ChannelType DioChannelConfigData[];
extern const Dio_ChannelGroupType DioConfigData[];
extern const Dio_PortType DioPortConfigData[];
/* ============================ [ LOCALS    ] ====================================================== */
/* ============================ [ FUNCTIONS ] ====================================================== */
#endif /* _DIO_CFG_H_ */
