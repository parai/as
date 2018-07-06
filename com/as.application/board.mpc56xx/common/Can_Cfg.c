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
#include "Can.h"
#include "CanIf.h"
#include "CanIf_Cbk.h"
/* ============================ [ MACROS    ] ====================================================== */
/* ============================ [ TYPES     ] ====================================================== */
/* ============================ [ DECLARES  ] ====================================================== */
/* ============================ [ DATAS     ] ====================================================== */
static const Can_HardwareObjectType CAN_CTRL_A_HOHCfgData[]=
{

    {    /* CanAHrh */
        .CanHandleType=CAN_ARC_HANDLE_TYPE_BASIC,
        .CanIdType=CAN_ID_TYPE_STANDARD,
        .CanIdValue=0x00,/* Accept Any message */
        .CanObjectId=CanAHrh,
        .CanObjectType=CAN_OBJECT_TYPE_RECEIVE,
        .CanFilterMaskRef=0,/*0: Accept Any message*/
        .ArcMailboxMask=0x00000001,/* 1st used as RX */
    },

    {    /* CanAHth */
        .CanHandleType=CAN_ARC_HANDLE_TYPE_BASIC,
        .CanIdType=CAN_ID_TYPE_STANDARD,
        .CanIdValue=0x00,
        .CanObjectId=CanAHth,
        .CanObjectType=CAN_OBJECT_TYPE_TRANSMIT,
        .CanFilterMaskRef=0,
        .ArcMailboxMask=0x00000002,/* 2nd used as TX */
    }
};

static uint8 CAN_CTRL_A_Arc_MailBoxToHrh[2] = {0 , 1};
static PduIdType CAN_CTRL_A_Arc_TxPduHandles[2];
const Can_ControllerConfigType  Can_ControllerCfgData[]=
{
	{
		.CanControllerId=CAN_CTRL_A,
		.Can_Arc_Flags = CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT |
						 CAN_CTRL_RX_PROCESSING_INTERRUPT |
						 CAN_CTRL_TX_PROCESSING_INTERRUPT |
						 /* CAN_CTRL_LOOPBACK | Loopback for debug */
						 CAN_CTRL_ERROR_PROCESSING_INTERRUPT,
		.CanControllerBaudRate=1000, /* Kbps */
		.CanControllerPropSeg=6,
		.CanControllerSeg1=3,
		.CanControllerSeg2=3,
		.CanControllerRJW = 3,
		.CanCpuClockRef = 0,
		.Can_Arc_RxMailBoxMask = 0x01, /* MSG BOX 0 used as RX*/
		.Can_Arc_TxMailBoxMask = 0x02, /* MSG BOX 1 used as TX*/
		.Can_Arc_Hoh=CAN_CTRL_A_HOHCfgData,
		.Can_Arc_MailBoxToHrh = CAN_CTRL_A_Arc_MailBoxToHrh,
		.Can_Arc_TxPduHandles = CAN_CTRL_A_Arc_TxPduHandles,
		.Can_Arc_TxMailboxStart = 1, /*start from 1st MSG BOX */
		.Can_Arc_HohCnt = 2,
		.Can_Arc_HohFifoCnt = 0,
		.Can_Arc_MailboxMax = 64
	}
};

const Can_CallbackType CanCallbackConfigData = {
	NULL, //CanIf_CancelTxConfirmation,
	CanIf_RxIndication,
	CanIf_ControllerBusOff,
	CanIf_TxConfirmation,
	NULL, //CanIf_ControllerWakeup,
	CanIf_Arc_Error,
 };
static const uint8 CanArcHthToUnit[] = {
	CAN_CTRL_A, /* CanAHth */
};
static const uint8 CanArcHthToHoh[] = {
	1, /* CanAHth index is 1 of CAN_CTRL_A_HOHCfgData*/
};
static const CanArcCtrlToUnit[] = {
	CAN_CTRL_A,
};
const Can_ConfigSetType Can_ConfigSetData = {
	.CanController = Can_ControllerCfgData,
	.CanCallbacks = &CanCallbackConfigData,
	.ArcHthToUnit = CanArcHthToUnit,
	.ArcHthToHoh  = CanArcHthToHoh,
	.ArcCtrlToUnit = CanArcCtrlToUnit
};
const Can_ConfigType Can_ConfigData ={&Can_ConfigSetData};
/* ============================ [ LOCALS    ] ====================================================== */
/* ============================ [ FUNCTIONS ] ====================================================== */
