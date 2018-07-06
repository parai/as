/* -------------------------------- Arctic Core ------------------------------
 * Arctic Core - the open source AUTOSAR platform http://arccore.com
 *
 * Copyright (C) 2009  ArcCore AB <contact@arccore.com>
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation; See <http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 * -------------------------------- Arctic Core ------------------------------*/

/** @reqSettings DEFAULT_SPECIFICATION_REVISION=3.1.5 */

/* Code TODO
 * - REMOVE1
 * - REMOVE2
 * - Enable SOFT_RESET to guarantee that we get the same state every time
 *   (do in Can_InitController ?)
 */

/* ----------------------------[information]----------------------------------*/
/*
 * Author: mahi
 *
 * Part of Release:
 *   3.1.5
 *
 * Description:
 *   Implements the Can Driver module
 *
 * Support:
 *   General                  Have Support
 *   -------------------------------------------
 *   CAN_DEV_ERROR_DETECT            Y
 *   CAN_HW_TRANSMIT_CANCELLATION    N
 *   CAN_IDENTICAL_ID_CANCELLATION   N
 *   CAN_INDEX                       N
 *   CAN_MULTIPLEXED_TRANSMISSION    N
 *   CAN_TIMEOUT_DURATION            N
 *   CAN_VERSION_INFO_API            N
 *
 *   Controller                  Have Support
 *   -------------------------------------------
 *   CAN_BUSOFF_PROCESSING           N  , Interrupt only
 *   CAN_RX_PROCESSING               N  , Interrupt only
 *   CAN_TX_PROCESSING               N  , Interrupt only
 *   CAN_WAKEUP_PROCESSING           N  , Interrupt only
 *   CAN_CPU_CLOCK_REFERENCE         N  , *)
 *   CanWakeupSourceRef              N  , **)
 *
 *   *) It assumes that there is a PERIPHERAL clock defined.
 *   **) The flexcan hardware cannot detect wakeup (at least now the ones
 *       this driver supports)
 *
 *   Devices    CLK_SRC
 *   ----------------------------------------------
 *   MPC5604B   ?
 *   MPC5606S   Only sys-clk it seems
 *   MPC551x    Both sys-clk and XOSC, See 3.3
 *   MPC5567    ?
 *   MPC5668    ?
 *
 *   MPC5554 is NOT supported (no individual mask)
 *
 * Implementation Notes:
 *   - Individual RXMASKs are always ON. No support for "old" chips.
 *   FIFO
 *   - Is always ON.
 *   - Is masked ONLY against 8 extended IDs
 *   - Have depth 6, with 8 ID and 8 RXMASK
 *
 *   MBOX ALLOCATION
 *     RX
 *       - First 8 boxes always allocated for RX FIFO
 *       - software filter for RX FIFO, to identify the right HRH.
 *       - HRHs are global indexed from 00
 *     TX
 *       - One HTH for each HOH (the HOH can have a number of mailboxes, in sequence)
 *       - HTHs are global (at least for each driver) indexed from 0
 *
 *    EXAMPLE ALLOCATION
 *      0 ,
 *      1  |
 *      2  |
 *      3  |   RX FIFO -> software lookup to map to right HTH
 *      4  |              1 FULL_CAN    HOH_0
 *      5  |              1 BASIC_CAN   HOH_1
 *      6  |
 *      7 ,
 *
 *      8   RX FULL_CAN  -  HOH_2
 *      9   RX FULL_CAN  -  HOH_3
 *      10  RX BASIC_CAN  - HOH_4   |  RX with 2 boxes
 *      11                - HOH_4   |
 *      12  TX FULL_CAN   - HOH_5
 *      13  TX BASIC_CAN  - HOH_6   |  TX with 2 boxes
 *      14                - HOH_6   |
 *
 *    LOOKUP
 *      TX
 *        Can_Write(..) sends with HTH as argument. We need find the HOH first
 *        and then find one or more TX mailboxes.
 *      RX
 *        TODO.
 *
 *    RAM USAGE:
 *      A "Can Unit":
 *      - mbToHrh, max 64  (could be moved to const... but PB problems?)
 *      - swPduHandles, max 64*2
 *      Global:
 *      - Can_HthToUnitIdMap, Number of HTHs
 *      - Can_HthToHohMap, Number of HTHs

 *  64 + 64*2 = 192*2 =
 *
 *  OBJECT MODEL
 *
 *     Can_ControllerConfigType
 *     |--- Can_Arc_Hoh -->  CanHardwareObjectConfig_CTRL_A
 *
 *     CanHardwareObjectConfig_CTRL_A
 *     |--- CanObjectId
 *
 *
 *
 *
 * Things left:
 *   All tags. Tags only on public functions now.
 *
 */


/* ----------------------------[includes]------------------------------------*/
#include "Can.h"
#include <stdio.h>
#include "mpc56xx.h"
#include "Mcu.h"
#include "CanIf_Cbk.h"
#if defined(USE_DET)
#include "Det.h"
#endif
#if defined(USE_DEM)
#include "Dem.h"
#endif
#include <stdlib.h>
#include <string.h>
#include "Os.h"
#include "asdebug.h"

#ifndef ISR_INSTALL_ISR2
#include "IntcInterrupts.h"
#define ISR_INSTALL_ISR2(_name, _can_entry, _vector, _priority, _app) \
	INTC_InstallINTCInterruptHandler(_can_entry, _vector, _priority)
#endif
/* ----------------------------[private define]------------------------------*/

#define AS_LOG_CAN 1
#define MAX_NUM_OF_MAILBOXES    64
#ifdef USE_SHELL
#define USE_CAN_STATISTICS      STD_ON
#else
#define USE_CAN_STATISTICS      STD_OFF
#endif

// Message box status defines
#define MB_TX_ONCE              0xc
#define MB_INACTIVE             0x8
#define MB_RX                   0x4
#define MB_ABORT                0x9
#define MB_RX_OVERRUN           0x6

/* Registers */
#define ESR_ERRINT		(1<<1)

/* ----------------------------[private macro]-------------------------------*/

#define CTRL_TO_UNIT_PTR(_controller)   &CanUnit[Can_Global.config->CanConfigSet->ArcCtrlToUnit[_controller]]
#define VALID_CONTROLLER(_ctrl)         (Can_Global.configuredMask & (1<<(_ctrl)))
#define GET_CALLBACKS()                 (Can_Global.config->CanConfigSet->CanCallbacks)
#if defined(CFG_MPC5604P)
#define GET_CONTROLLER(_controller) 	\
        					((flexcan_t *)(0xFFFC0000 + 0x28000*(_controller)))
#else
#define GET_CONTROLLER(_controller) 	\
        					((flexcan_t *)(0xFFFC0000 + 0x4000*(_controller)))
#endif

#define INSTALL_HANDLER4(_name, _can_entry, _vector, _priority, _app)\
	do { \
		ISR_INSTALL_ISR2(_name, _can_entry, _vector+0, _priority, _app); \
		ISR_INSTALL_ISR2(_name, _can_entry, _vector+1, _priority, _app); \
		ISR_INSTALL_ISR2(_name, _can_entry, _vector+2, _priority, _app); \
		ISR_INSTALL_ISR2(_name, _can_entry, _vector+3, _priority, _app); \
	} while(0)

#define INSTALL_HANDLER16(_name, _can_entry, _vector, _priority, _app)\
	do { \
		INSTALL_HANDLER4(_name, _can_entry, _vector+0, _priority, _app); \
		INSTALL_HANDLER4(_name, _can_entry, _vector+4, _priority, _app); \
		INSTALL_HANDLER4(_name, _can_entry, _vector+8, _priority, _app); \
		INSTALL_HANDLER4(_name, _can_entry, _vector+12,_priority, _app); \
	} while(0)

//-------------------------------------------------------------------

#if ( CAN_DEV_ERROR_DETECT == STD_ON )
#define VALIDATE(_exp,_api,_err ) \
        if( !(_exp) ) { \
          Det_ReportError(MODULE_ID_CAN,0,_api,_err); \
          return CAN_NOT_OK; \
        }

#define VALIDATE_NO_RV(_exp,_api,_err ) \
        if( !(_exp) ) { \
          Det_ReportError(MODULE_ID_CAN,0,_api,_err); \
          return; \
        }

#define DET_REPORTERROR(_x,_y,_z,_q) Det_ReportError(_x, _y, _z, _q)
#else
#define VALIDATE(_exp,_api,_err )
#define VALIDATE_NO_RV(_exp,_api,_err )
#define DET_REPORTERROR(_x,_y,_z,_q)
#endif

#if defined(USE_DEM)
#define VALIDATE_DEM_NO_RV(_exp,_err ) \
        if( !(_exp) ) { \
          Dem_ReportErrorStatus(_err, DEM_EVENT_STATUS_FAILED); \
          return; \
        }
#else
#define VALIDATE_DEM_NO_RV(_exp,_err )
#endif


/* ----------------------------[private typedef]-----------------------------*/


typedef struct FLEXCAN2_tag flexcan_t;

#if defined(CFG_CAN_TEST)
Can_TestType Can_Test;
#endif

typedef enum {
    CAN_UNINIT = 0, CAN_READY
} Can_DriverStateType;


/* Type for holding global information used by the driver */
typedef struct {
    Can_DriverStateType     initRun;            /* If Can_Init() have been run */
    const Can_ConfigType *  config;             /* Pointer to config */
    uint32                  configuredMask;     /* What units are configured */
} Can_GlobalType;


/* Type for holding information about each controller */
typedef struct {
    /* This unit have uses controller */
    CanControllerIdType                 controllerId;
    CanIf_ControllerModeType            state;
    const Can_ControllerConfigType *    cfgCtrlPtr;     /* Pointer to controller config  */
    const Can_HardwareObjectType *      cfgHohPtr;     /* List of HOHs */
    flexcan_t *                         hwPtr;
    uint32      lock_cnt;
    uint64      Can_Arc_RxMbMask;
    uint64      Can_Arc_TxMbMask;
    uint64      mbTxFree;
    uint8_t     mbMax;                              /* Max number of mailboxes used for this unit */
//    PduIdType   swPduHandles[MAX_NUM_OF_MAILBOXES]; /* Data stored for Txconfirmation callbacks to CanIf */
//    uint8_t     mbToHrh[MAX_NUM_OF_MAILBOXES];      /* Mapping from mailbox to HRH */
#if (USE_CAN_STATISTICS == STD_ON)
    Can_Arc_StatisticsType stats;
#endif
} Can_UnitType;

/* ----------------------------[private function prototypes]-----------------*/
static void Can_Isr(int unit);
static void Can_Err(int unit);
static void Can_BusOff(int unit);

/* ----------------------------[private variables]---------------------------*/

Can_UnitType    CanUnit[CAN_ARC_CTRL_CONFIG_CNT];
Can_GlobalType  Can_Global = { .initRun = CAN_UNINIT, };
/* Used by ISR */
//uint8_t         Can_CtrlToUnitMap[CAN_ARC_CTRL_CONFIG_CNT];
/* Used by Can_Write() */
//uint8_t         Can_HthToUnitIdMap[NUM_OF_HTHS];
//uint8_t         Can_HthToHohMap[NUM_OF_HTHS];

/* ----------------------------[private functions]---------------------------*/

static void clearMbFlag( flexcan_t * hw,  uint8_t mb ) {
    if( mb >= 32) {
#if defined(CFG_MPC563XM)
		if(hw==GET_CONTROLLER(FLEXCAN_A)){
	        hw->IFRH.R = (1<<(mb-32));
		}
#else
        hw->IFRH.R = (1<<(mb-32));
#endif
    } else {
        hw->IFRL.R = (1<<mb);
    }
}

static inline uint64_t ilog2_64( uint64_t val ) {
    uint32_t t = val >> 32;

    if( t != 0) {
        return ilog2(t) + 32;
    }
    return ilog2((uint32_t)val);
}


#if defined(CFG_MPC563XM)
void Can_A_Isr( void ) {Can_Isr(CAN_CTRL_A);}
void Can_C_Isr( void ) {Can_Isr(CAN_CTRL_C);}
void Can_A_Err( void ) {Can_Err(CAN_CTRL_A);}
void Can_C_Err( void ) {Can_Err(CAN_CTRL_C);}
void Can_A_BusOff( void ) {Can_BusOff(CAN_CTRL_A);}
void Can_C_BusOff( void ) {Can_BusOff(CAN_CTRL_C);}

#else

void Can_A_Isr( void ) {Can_Isr(CAN_CTRL_A);}
void Can_B_Isr( void ) {Can_Isr(CAN_CTRL_B);}

#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5567) || defined(CFG_MPC5668) || defined(CFG_MPC560XB)
void Can_C_Isr( void ) {Can_Isr(CAN_CTRL_C);}
void Can_D_Isr( void ) {Can_Isr(CAN_CTRL_D);}
void Can_E_Isr( void ) {Can_Isr(CAN_CTRL_E);}
#endif
#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5668) || defined(CFG_MPC560XB)
void Can_F_Isr( void ) {Can_Isr(CAN_CTRL_F);}
#endif

void Can_A_Err( void ) {Can_Err(CAN_CTRL_A);}
void Can_B_Err( void ) {Can_Err(CAN_CTRL_B);}
#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5567) || defined(CFG_MPC5668) || defined(CFG_MPC560XB)
void Can_C_Err( void ) {Can_Err(CAN_CTRL_C);}
void Can_D_Err( void ) {Can_Err(CAN_CTRL_D);}
void Can_E_Err( void ) {Can_Err(CAN_CTRL_E);}
#endif
#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5668) || defined(CFG_MPC560XB)
void Can_F_Err( void ) {Can_Err(CAN_CTRL_F);}
#endif

void Can_A_BusOff( void ) {Can_BusOff(CAN_CTRL_A);}
void Can_B_BusOff( void ) {Can_BusOff(CAN_CTRL_B);}
#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5567) || defined(CFG_MPC5668) || defined(CFG_MPC560XB)
void Can_C_BusOff( void ) {Can_BusOff(CAN_CTRL_C);}
void Can_D_BusOff( void ) {Can_BusOff(CAN_CTRL_D);}
void Can_E_BusOff( void ) {Can_BusOff(CAN_CTRL_E);}
#endif
#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5668) || defined(CFG_MPC560XB)
void Can_F_BusOff( void ) {Can_BusOff(CAN_CTRL_F);}
#endif

#endif /* defined(CFG_MPC563XM) */

//-------------------------------------------------------------------

#if defined(CFG_CAN_TEST)
Can_TestType * Can_Arc_GetTestInfo( void ) {
	return &Can_Test;
}
#endif

/**
 * Hardware error ISR for CAN
 *
 * @param unit CAN controller number( from 0 )
 */

static void Can_Err(int unit)
{
    flexcan_t *canHw = GET_CONTROLLER(unit);
    Can_Arc_ErrorType err;
    uint32 esr;

	err.R = 0;

    /* Clear bits 16-23 by read */
    esr = canHw->ESR.R;
    ASLOG(CAN, "CAN%d esr = %X\n", unit, esr);
    if( esr & ESR_ERRINT )
    {
		if (GET_CALLBACKS()->Arc_Error != NULL) {
			GET_CALLBACKS()->Arc_Error(unit, err);
		}

		Can_SetControllerMode(unit, CAN_T_STOP); // CANIF272 Same handling as for busoff
		// Clear ERRINT
		canHw->ESR.R = ESR_ERRINT;
    }
}

/**
 *
 * @param canHw
 * @param canUnit
 */
// Uses 25.4.5.1 Transmission Abort Mechanism
#if defined(CFG_MPC563XM)

static void Can_AbortTx(flexcan_t *canHw, Can_UnitType *canUnit)
{
    uint64_t mbMask;
	uint8 mbNr;
	// Find our Tx boxes.
	mbMask = canUnit->Can_Arc_TxMbMask;
	// Loop over the Mb's set to abort
	for (; mbMask; mbMask &= ~(1ull << mbNr)) {
		mbNr = ilog2_64(mbMask);
		canHw->BUF[mbNr].CS.B.CODE = MB_ABORT;
		// Did it take
		if (canHw->BUF[mbNr].CS.B.CODE != MB_ABORT) {
			// nope..
			/* it's not sent... or being sent.
			 * Just wait for it
			 */
			int i = 0;
		    volatile uint64_t iFlag;
			do{
			    if(canHw==GET_CONTROLLER(FLEXCAN_A)){
			    	iFlag = *(uint64_t *)(&canHw->IFRH.R);
			    }else{
			    	iFlag = (uint64_t)(*(uint32_t *)(&canHw->IFRL.R));
			    }
				i++;
				if (i > 1000) {
					break;
				}
			} while (iFlag & (1ull << mbNr));
		}

		// Clear interrupt
		clearMbFlag(canHw,mbNr);
		canUnit->mbTxFree |= (1ull << mbNr);
	}
}

#else

static void Can_AbortTx(flexcan_t *canHw, Can_UnitType *canUnit)
{
 uint64_t mbMask;
    uint8 mbNr;
    // Find our Tx boxes.
    mbMask = canUnit->Can_Arc_TxMbMask;
    // Loop over the Mb's set to abort
    for (; mbMask; mbMask &= ~(1ull << mbNr)) {
        mbNr = ilog2_64(mbMask);
        canHw->BUF[mbNr].CS.B.CODE = MB_ABORT;
        // Did it take
        if (canHw->BUF[mbNr].CS.B.CODE != MB_ABORT) {
            // nope..
            /* it's not sent... or being sent.
             * Just wait for it
             */
            int i = 0;
            while (*(uint64_t *) (&canHw->IFRH.R) & (1ull << mbNr)) {
                i++;
                if (i > 1000) {
                    break;
                }
            }
        }

        // Clear interrupt
        clearMbFlag(canHw,mbNr);
        canUnit->mbTxFree |= (1ull << mbNr);
    }
}

#endif



/**
 * BusOff ISR for CAN
 *
 * @param unit CAN controller number( from 0 )
 */
static void Can_BusOff(int unit)
{
    flexcan_t *canHw = GET_CONTROLLER(unit);
    Can_UnitType *canUnit = CTRL_TO_UNIT_PTR(unit);
    Can_Arc_ErrorType err;
    err.R = 0;

#if (USE_CAN_STATISTICS == STD_ON)
    if (canHw->ESR.B.TWRNINT) {
        err.B.TXWRN = canHw->ESR.B.TXWRN;
        canUnit->stats.txErrorCnt++;
        canHw->ESR.B.TWRNINT = 1;
    }

    if (canHw->ESR.B.RWRNINT) {
        err.B.RXWRN = canHw->ESR.B.RXWRN;
        canUnit->stats.rxErrorCnt++;
        canHw->ESR.B.RWRNINT = 1;
    }
#endif

    if (canHw->ESR.B.BOFFINT) {
        ASLOG(CAN,"CAN%d bus off\n", unit);
#if (USE_CAN_STATISTICS == STD_ON)
        canUnit->stats.boffCnt++;
#endif
        if (GET_CALLBACKS()->ControllerBusOff != NULL) {
            GET_CALLBACKS()->ControllerBusOff(unit);
        }
        Can_SetControllerMode(unit, CAN_T_STOP); // CANIF272

        canHw->ESR.B.BOFFINT = 1;

        Can_AbortTx(canHw, canUnit); // CANIF273

        if (canHw->CR.B.BOFFREC) {
        	canHw->CR.B.BOFFREC = 0;
        	canHw->CR.B.BOFFREC = 1;
        }
    }
}

/**
 *
 * @param uPtr
 */
static void Can_Isr_Tx(Can_UnitType *uPtr)
{
    uint8 mbNr;
    flexcan_t *canHw;
    PduIdType pduId;


    canHw = uPtr->hwPtr;

#if defined(CFG_MPC563XM)
    uint64_t mbMask;
    if(canHw==GET_CONTROLLER(FLEXCAN_A)){
    	mbMask = *(uint64_t *)(&canHw->IFRH.R);
    }else{
    	mbMask = (uint64_t)(*(uint32_t *)(&canHw->IFRL.R));
    }
#else
    uint64_t mbMask = *(uint64_t *) (&canHw->IFRH.R);
#endif
    mbMask &= uPtr->Can_Arc_TxMbMask;

    /*
     * Tx
     */
#if defined(CFG_CAN_TEST)
    Can_Test.mbMaskTx |= mbMask;
#endif


    for (; mbMask; mbMask &= ~(1ull << mbNr)) {
        mbNr = ilog2_64(mbMask);

        pduId = uPtr->cfgCtrlPtr->Can_Arc_TxPduHandles[mbNr-uPtr->cfgCtrlPtr->Can_Arc_TxMailboxStart];
        uPtr->cfgCtrlPtr->Can_Arc_TxPduHandles[mbNr-uPtr->cfgCtrlPtr->Can_Arc_TxMailboxStart] = 0;

        // Clear interrupt and mark txBox as Free
        clearMbFlag(canHw,mbNr);
        uPtr->mbTxFree |= (1ull << mbNr);

        if (GET_CALLBACKS()->TxConfirmation != NULL) {
            GET_CALLBACKS()->TxConfirmation(pduId);
        }
    }
}

static void Can_Isr_Rx(Can_UnitType *uPtr)
{
    uint8 mbNr;
    uint32 id;
    uint32 mask;
    uint32 tmp = 0;

    flexcan_t *canHw;
    const Can_HardwareObjectType *hohPtr;

    canHw = uPtr->hwPtr;

#if defined(CFG_MPC563XM)
    uint64_t iFlag;
    if(canHw==GET_CONTROLLER(FLEXCAN_A)){
    	iFlag = *(uint64_t *)(&canHw->IFRH.R);
    }else{
    	iFlag = (uint64_t)(*(uint32_t *)(&canHw->IFRL.R));
    }
#else
    uint64_t iFlag = *(uint64_t*) (&canHw->IFRH.R);
#endif

#if defined(CFG_CAN_TEST)
    Can_Test.mbMaskRx |= iFlag & uPtr->Can_Arc_RxMbMask;
#endif

    while (iFlag & uPtr->Can_Arc_RxMbMask) {

        /* Find mailbox */
        mbNr = ilog2_64(iFlag & uPtr->Can_Arc_RxMbMask);

        /* Check for FIFO interrupt */
        if (canHw->MCR.B.FEN && ((uint32_t)iFlag & (1 << 5))) {

        	tmp++;
//        	if( tmp > 2) {
//        		while(1){}
//        	}

            /* Check overflow */
            if (iFlag & (1 << 7)) {
#if (USE_CAN_STATISTICS == STD_ON)
                uPtr->stats.fifoOverflow++;
#endif
                clearMbFlag(canHw, 7);
                DET_REPORTERROR(MODULE_ID_CAN,0,0, CAN_E_DATALOST); /** @req 4.0.3/CAN395 */
            }

#if (USE_CAN_STATISTICS == STD_ON)
            /* Check warning */
            if (iFlag & (1 << 6)) {
                uPtr->stats.fifoWarning++;
                clearMbFlag(canHw, 6);
            }
#endif

            /* The manual is "fuzzy" here, read control and status */
            (void) canHw->BUF[0].CS.R;

            if (canHw->BUF[0].CS.B.IDE) {
                id = canHw->BUF[0].ID.R;
                id |= 0x80000000;
            } else {
                id = canHw->BUF[0].ID.B.STD_ID;
            }

            /* Must now do a manual match to find the right CanHardwareObject
             * to pass to CanIf. We know that the FIFO objects are sorted first.
             */
            hohPtr = uPtr->cfgCtrlPtr->Can_Arc_Hoh;

            /* Match in order */
            vuint32_t *fifoIdPtr = (vuint32_t *) &canHw->BUF[6];


            for (uint8 fifoNr = 0; fifoNr < uPtr->cfgCtrlPtr->Can_Arc_HohFifoCnt; fifoNr++) {
                mask = canHw->RXIMR[fifoNr].R;

                if ((id & mask) != (fifoIdPtr[fifoNr] & mask)) {
                    continue;
                }

                if (GET_CALLBACKS()->RxIndication != NULL) {
                    GET_CALLBACKS()->RxIndication(uPtr->cfgCtrlPtr->Can_Arc_MailBoxToHrh[fifoNr],
                    		id,
                            canHw->BUF[0].CS.B.LENGTH,
                            (uint8 *) &canHw->BUF[0].DATA.W[0]);
                }
                break;
            }

            // Clear the interrupt
            clearMbFlag(canHw, 5);
            if( canHw->IFRL.B.BUF05I == 0 ) {
                iFlag ^= 1ull << mbNr;
            }

        } else {
            /* Not FIFO */
            iFlag ^= 1ull << mbNr;


            /* activate the internal lock with a read*/
            (void) canHw->BUF[mbNr].CS.R;


            if (canHw->BUF[mbNr].CS.B.IDE) {
                id = canHw->BUF[mbNr].ID.R;
                id |= 0x80000000;
            } else {
                id = canHw->BUF[mbNr].ID.B.STD_ID;
            }

#if defined(USE_DET)
            if( canHw->BUF[mbNr].CS.B.CODE == MB_RX_OVERRUN ) {
                /* We have overwritten one frame */
                Det_ReportError(MODULE_ID_CAN,0,0,CAN_E_DATALOST); /** @req 4.0.3/CAN395 */
            }
#endif

            if (GET_CALLBACKS()->RxIndication != NULL) {
                GET_CALLBACKS()->RxIndication(uPtr->cfgCtrlPtr->Can_Arc_MailBoxToHrh[mbNr], id,
                        canHw->BUF[mbNr].CS.B.LENGTH,
                        (uint8 *) &canHw->BUF[mbNr].DATA.W[0]);
            }
#if (USE_CAN_STATISTICS == STD_ON)
            uPtr->stats.rxSuccessCnt++;
#endif
            /* unlock MB (dummy read timer) */
            (void) canHw->TIMER.R;

            // Clear interrupt
            clearMbFlag(canHw, mbNr);
        }

    }
}


static void Can_Isr(int controller )
{
    Can_UnitType *uPtr = CTRL_TO_UNIT_PTR(controller);

    if(uPtr->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_TX_PROCESSING_INTERRUPT ){
        Can_Isr_Tx(uPtr);
    }

    if(uPtr->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_RX_PROCESSING_INTERRUPT ){
        Can_Isr_Rx(uPtr);
    }
}


//-------------------------------------------------------------------

// This initiates ALL can controllers
void Can_Init(const Can_ConfigType *config)
{
    /** @req 3.1.5/CAN223 */
    /** !req 3.1.5/CAN176 */

    Can_UnitType *unitPtr;

    /** @req 3.1.5/CAN174 */
    VALIDATE_NO_RV( (Can_Global.initRun == CAN_UNINIT), CAN_INIT_SERVICE_ID, CAN_E_TRANSITION );
    /** @req 3.1.5/CAN175 */
    VALIDATE_NO_RV( (config != NULL ), CAN_INIT_SERVICE_ID, CAN_E_PARAM_POINTER );

    // Save config
    Can_Global.config = config;
    Can_Global.initRun = CAN_READY;

    for (int configId = 0; configId < CAN_ARC_CTRL_CONFIG_CNT; configId++) {
        const Can_ControllerConfigType *cfgCtrlPtr  = &Can_Global.config->CanConfigSet->CanController[configId];

        Can_Global.configuredMask |= (1 << cfgCtrlPtr->CanControllerId);
        unitPtr = &CanUnit[configId];

        memset(unitPtr, 0, sizeof(Can_UnitType));

        unitPtr->controllerId = cfgCtrlPtr->CanControllerId;
        unitPtr->hwPtr = GET_CONTROLLER(cfgCtrlPtr->CanControllerId);
        unitPtr->cfgCtrlPtr = cfgCtrlPtr;
        unitPtr->state = CANIF_CS_STOPPED;
        unitPtr->cfgHohPtr = cfgCtrlPtr->Can_Arc_Hoh;

        unitPtr->Can_Arc_RxMbMask = cfgCtrlPtr->Can_Arc_RxMailBoxMask;
        unitPtr->Can_Arc_TxMbMask = cfgCtrlPtr->Can_Arc_TxMailBoxMask;

        switch (cfgCtrlPtr->CanControllerId) {
#if defined(CFG_MPC560X)
        case CAN_CTRL_A:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
			ISR_INSTALL_ISR2( "Can", Can_A_BusOff, FLEXCAN_0_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
			ISR_INSTALL_ISR2( "Can", Can_A_Err, FLEXCAN_0_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
			INSTALL_HANDLER4( "Can", Can_A_Isr, FLEXCAN_0_BUF_00_03, 2, 0 ); /* 0-3, 4-7, 8-11, 12-15 */
			ISR_INSTALL_ISR2( "Can", Can_A_Isr, FLEXCAN_0_BUF_16_31, 2, 0 );
#if !defined (CFG_MPC5604P)
			ISR_INSTALL_ISR2( "Can", Can_A_Isr, FLEXCAN_0_BUF_32_63, 2, 0 );
#endif
		}
        break;
        case CAN_CTRL_B:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_B_BusOff, FLEXCAN_1_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_B_Err, FLEXCAN_1_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
			INSTALL_HANDLER4( "Can", Can_B_Isr, FLEXCAN_1_BUF_00_03, 2, 0 ); /* 0-3, 4-7, 8-11, 12-15 */
			ISR_INSTALL_ISR2( "Can", Can_B_Isr, FLEXCAN_1_BUF_16_31, 2, 0 );
#if !defined (CFG_MPC5604P)
			ISR_INSTALL_ISR2( "Can", Can_B_Isr, FLEXCAN_1_BUF_32_63, 2, 0 );
#endif
		}
        break;
	#if defined(CFG_MPC560XB)
        case CAN_CTRL_C:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_C_BusOff, FLEXCAN_2_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_C_Err, FLEXCAN_2_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER4( "Can", Can_C_Isr, FLEXCAN_2_BUF_00_03, 2, 0 ); /* 0-3, 4-7, 8-11, 12-15 */
	        ISR_INSTALL_ISR2( "Can", Can_C_Isr, FLEXCAN_2_BUF_16_31, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_C_Isr, FLEXCAN_2_BUF_32_63, 2, 0 );
		}
        break;
        case CAN_CTRL_D:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_D_BusOff, FLEXCAN_3_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_D_Err, FLEXCAN_3_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER4( "Can", Can_D_Isr, FLEXCAN_3_BUF_00_03, 2, 0 ); /* 0-3, 4-7, 8-11, 12-15 */
	        ISR_INSTALL_ISR2( "Can", Can_D_Isr, FLEXCAN_3_BUF_16_31, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_D_Isr, FLEXCAN_3_BUF_32_63, 2, 0 );
		}
        break;
        case CAN_CTRL_E:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_E_BusOff, FLEXCAN_4_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_E_Err, FLEXCAN_4_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER4( "Can", Can_E_Isr, FLEXCAN_4_BUF_00_03, 2, 0 ); /* 0-3, 4-7, 8-11, 12-15 */
	        ISR_INSTALL_ISR2( "Can", Can_E_Isr, FLEXCAN_4_BUF_16_31, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_E_Isr, FLEXCAN_4_BUF_32_63, 2, 0 );
		}
        break;
        case CAN_CTRL_F:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_F_BusOff, FLEXCAN_5_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_F_Err, FLEXCAN_5_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER4( "Can", Can_F_Isr, FLEXCAN_5_BUF_00_03, 2, 0 ); /* 0-3, 4-7, 8-11, 12-15 */
	        ISR_INSTALL_ISR2( "Can", Can_F_Isr, FLEXCAN_5_BUF_16_31, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_F_Isr, FLEXCAN_5_BUF_32_63, 2, 0 );
		}
        break;
	#endif
#elif defined(CFG_MPC563XM)
        case CAN_CTRL_A:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_A_BusOff, FLEXCAN_A_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_A_Err, FLEXCAN_A_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_A_Isr, FLEXCAN_A_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_A_Isr, FLEXCAN_A_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_A_Isr, FLEXCAN_A_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
        case CAN_CTRL_C:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_C_BusOff, FLEXCAN_C_ESR_BOFF_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_C_Err, FLEXCAN_C_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_C_Isr, FLEXCAN_C_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_C_Isr, FLEXCAN_C_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_C_Isr, FLEXCAN_C_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
#elif defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5567) || defined(CFG_MPC5668)
        case CAN_CTRL_A:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_A_BusOff, FLEXCAN_A_ESR_BOFF_INT, 2, 0);
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_A_Err, FLEXCAN_A_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_A_Isr, FLEXCAN_A_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_A_Isr, FLEXCAN_A_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_A_Isr, FLEXCAN_A_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
        case CAN_CTRL_B:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_B_BusOff, FLEXCAN_B_ESR_BOFF_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_B_Err, FLEXCAN_B_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_B_Isr, FLEXCAN_B_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_B_Isr, FLEXCAN_B_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_B_Isr, FLEXCAN_B_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
        case CAN_CTRL_C:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_C_BusOff, FLEXCAN_C_ESR_BOFF_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_C_Err, FLEXCAN_C_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_C_Isr, FLEXCAN_C_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_C_Isr, FLEXCAN_C_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_C_Isr, FLEXCAN_C_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
        case CAN_CTRL_D:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_D_BusOff, FLEXCAN_D_ESR_BOFF_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_D_Err, FLEXCAN_D_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_D_Isr, FLEXCAN_D_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_D_Isr, FLEXCAN_D_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_D_Isr, FLEXCAN_D_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
        case CAN_CTRL_E:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_E_BusOff, FLEXCAN_E_ESR_BOFF_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_E_Err, FLEXCAN_E_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_E_Isr, FLEXCAN_E_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_E_Isr, FLEXCAN_E_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_E_Isr, FLEXCAN_E_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
	#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5668)
        case CAN_CTRL_F:
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_F_BusOff, FLEXCAN_F_ESR_BOFF_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT){
	        ISR_INSTALL_ISR2( "Can", Can_F_Err, FLEXCAN_F_ESR_ERR_INT, 2, 0 );
		}
		if(cfgCtrlPtr->Can_Arc_Flags &  (CAN_CTRL_TX_PROCESSING_INTERRUPT | CAN_CTRL_RX_PROCESSING_INTERRUPT)){
	        INSTALL_HANDLER16( "Can", Can_F_Isr, FLEXCAN_F_IFLAG1_BUF0I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_F_Isr, FLEXCAN_F_IFLAG1_BUF31_16I, 2, 0 );
	        ISR_INSTALL_ISR2( "Can", Can_F_Isr, FLEXCAN_F_IFLAG1_BUF63_32I, 2, 0 );
		}
        break;
	#endif
#endif
        default:
            asAssert(0);
        }
    }
    return;
}

// Unitialize the module
void Can_DeInit()
{
    Can_UnitType *canUnit;
    const Can_ControllerConfigType *cfgCtrlPtr;
    uint32 ctlrId;

    for (int configId = 0; configId < CAN_ARC_CTRL_CONFIG_CNT; configId++) {
        cfgCtrlPtr = &Can_Global.config->CanConfigSet->CanController[(configId)];
        ctlrId = cfgCtrlPtr->CanControllerId;

        canUnit = CTRL_TO_UNIT_PTR(ctlrId);

        Can_DisableControllerInterrupts(ctlrId);
 
        canUnit->state = CANIF_CS_UNINIT;

        canUnit->lock_cnt = 0;

#if (USE_CAN_STATISTICS == STD_ON)
        // Clear stats
        memset(&canUnit->stats, 0, sizeof(Can_Arc_StatisticsType));
#endif
    }

    Can_Global.config = NULL;
    Can_Global.initRun = CAN_UNINIT;

    return;
}

void Can_InitController(uint8 controller,
        const Can_ControllerConfigType *config)
{
    /** @req 3.1.5/CAN229 */
    /** !req 3.1.5/CAN192 */

    flexcan_t *canHw;
    uint8_t tq;
    uint8_t tq1;
    uint8_t tq2;
    uint32_t clock;
    Can_UnitType *canUnit;
    const Can_ControllerConfigType *cfgCtrlPtr;
    const Can_HardwareObjectType *hohPtr;
    uint8_t fifoNr;

    /** @req 3.1.5/CAN187 */
    VALIDATE_NO_RV( (Can_Global.initRun == CAN_READY), CAN_INITCONTROLLER_SERVICE_ID, CAN_E_UNINIT );
    /** @req 3.1.5/CAN188 */
    VALIDATE_NO_RV( (config != NULL ), CAN_INITCONTROLLER_SERVICE_ID,CAN_E_PARAM_POINTER);
    /** !req 3.1.5/CAN189 Controller is controller number, not a sequence number? */
    VALIDATE_NO_RV( VALID_CONTROLLER(controller) , CAN_INITCONTROLLER_SERVICE_ID, CAN_E_PARAM_CONTROLLER );

    canUnit = CTRL_TO_UNIT_PTR(controller);
    /** @req 3.1.5/CAN190 */
    VALIDATE_NO_RV( (canUnit->state==CANIF_CS_STOPPED), CAN_INITCONTROLLER_SERVICE_ID, CAN_E_TRANSITION );

    canHw = canUnit->hwPtr;
    cfgCtrlPtr = canUnit->cfgCtrlPtr;

    // Start this baby up
    canHw->MCR.B.MDIS = 0;

    // Wait for it to reset
    // Freeze to write all mem mapped registers ( see 25.4.8.1 )
    canHw->MCR.B.FRZ = 1;
    canHw->MCR.B.HALT = 1;
    if( config->Can_Arc_Flags & CAN_CTRL_FIFO ) {
        canHw->MCR.B.FEN = 1;       /*  Enable FIFO */
        canHw->MCR.B.IDAM = 0;      /* We want extended id's to match with */
    }
//    canHw->MCR.B.BCC = 1;           /* Enable all nice features */
    canHw->MCR.B.AEN = 1;           /* Enable Abort */

    /* Use Fsys derivate */
#ifdef CFG_CAN_OSCILLATOR_CLOCK
    canHw->CR.B.CLKSRC = 0;
#else
    canHw->CR.B.CLKSRC = 1;
#endif
    canHw->MCR.B.MAXMB = cfgCtrlPtr->Can_Arc_MailboxMax - 1;


    /* Disable self-reception, if not loopback  */
    canHw->MCR.B.SRXDIS = (config->Can_Arc_Flags & CAN_CTRL_LOOPBACK) ? 0 : 1;

    /* Clock calucation
     * -------------------------------------------------------------------
     *
     * * 1 TQ = Sclk period( also called SCK )
     * * Ftq = Fcanclk / ( PRESDIV + 1 ) = Sclk
     *   ( Fcanclk can come from crystal or from the peripheral dividers )
     *
     * -->
     * TQ = 1/Ftq = (PRESDIV+1)/Fcanclk --> PRESDIV = (TQ * Fcanclk - 1 )
     * TQ is between 8 and 25
     */

    /* Calculate the number of timequanta's (from "Protocol Timing"( chap. 25.4.7.4 )) */
    tq1 = (config->CanControllerPropSeg + config->CanControllerSeg1 + 2);
    tq2 = (config->CanControllerSeg2 + 1);
    tq = 1 + tq1 + tq2;

    // Check TQ limitations..
    VALIDATE_DEM_NO_RV(( (tq1>=4) && (tq1<=16)), CAN_E_TIMEOUT );
    VALIDATE_DEM_NO_RV(( (tq2>=2) && (tq2<=8)), CAN_E_TIMEOUT );
    VALIDATE_DEM_NO_RV(( (tq>8) && (tq<25 )), CAN_E_TIMEOUT );

    // Assume we're using the peripheral clock instead of the crystal.
    if (canHw->CR.B.CLKSRC == 1) {
    	clock = McuE_GetPeripheralClock((McuE_PeriperalClock_t) config->CanCpuClockRef);
    } else {
    	clock = McuE_GetClockReferencePointFrequency();
    }

    canHw->CR.B.PRESDIV = clock / (config->CanControllerBaudRate * 1000 * tq) - 1;
    canHw->CR.B.PROPSEG = config->CanControllerPropSeg;
    canHw->CR.B.PSEG1   = config->CanControllerSeg1;
    canHw->CR.B.PSEG2   = config->CanControllerSeg2;
    canHw->CR.B.RJW     = config->CanControllerRJW;
    canHw->CR.B.SMP     = 1; // 3 samples better than 1 ??
    canHw->CR.B.LPB     = (config->Can_Arc_Flags & CAN_CTRL_LOOPBACK) ? 1 : 0;
    canHw->CR.B.BOFFREC = 1; // Disable bus off recovery

    /* Setup mailboxes for this controller */
    hohPtr = cfgCtrlPtr->Can_Arc_Hoh;
    uint8_t mbNr = 8;
    fifoNr = 0;

    asAssert( hohPtr != NULL );

    struct FLEXCAN_RXFIFO_t *fifoIdPtr = (struct FLEXCAN_RXFIFO_t *)&canHw->BUF[0];

    memset(&canHw->BUF[0],0,sizeof(struct FLEXCAN_BUF_t)*cfgCtrlPtr->Can_Arc_MailboxMax);

    /* Rx Global Mask, don't care */
    canHw->RXGMASK.R = 0;

#if 0
    for( int i=0; i < 8;i++) {
        canHw->RXIMR[i].R = 0xfffffffful;
        fifoIdPtr->IDTABLE[i].R = 0x0;
    }
#endif

    /* The HOHs are sorted by FIFO(FULL_CAN), FIFO(BASIC_CAN),
     * FULL_CAN(no FIFO) and last BASIC_CAN(no FIFO) */
    for( int i=0; i < cfgCtrlPtr->Can_Arc_HohCnt;i++,hohPtr++)
    {
        if( ( hohPtr->CanObjectType != CAN_OBJECT_TYPE_RECEIVE) ) {
            continue;
        }

        /* Assign FIFO first it will search for match first there (its the first MBs) */
        if( fifoNr < cfgCtrlPtr->Can_Arc_HohFifoCnt ) {
            /* TODO : Set IDAM */

            /* Set the ID */
        	if (hohPtr->CanIdType == CAN_ID_TYPE_EXTENDED) {
        		fifoIdPtr->IDTABLE[fifoNr].R =  ((hohPtr->CanIdValue << 1) | 0x40000000) ;
        	} else {
        		fifoIdPtr->IDTABLE[fifoNr].R =  (hohPtr->CanIdValue << 19) ;
        	}

            /* The Mask (we have FULL_CAN here) */
            //canHw->RXIMR[fifoNr].R = hohPtr->CanFilterMaskRef;
            fifoNr++;
        } else {
        	/* loop for multiplexed mailboxes, set as same as first */
        	uint64  mbMask = hohPtr->ArcMailboxMask;
        	uint8   mbTmp;

            for (; mbMask; mbMask &= ~(1ull << mbTmp)) {
            	mbTmp = ilog2_64(mbMask);

				canHw->BUF[mbTmp].CS.B.CODE  = MB_RX;

				//canHw->RXIMR[mbTmp].R        = hohPtr->CanFilterMaskRef;

				if (hohPtr->CanIdType == CAN_ID_TYPE_EXTENDED) {
					canHw->BUF[mbTmp].CS.B.IDE    = 1;
					canHw->BUF[mbTmp].ID.R        = hohPtr->CanIdValue;
				} else {
					canHw->BUF[mbTmp].CS.B.IDE    = 0;
					canHw->BUF[mbTmp].ID.B.STD_ID = hohPtr->CanIdValue;
				}

				mbNr++;
            }
        }
    }

    canUnit->mbTxFree = canUnit->Can_Arc_TxMbMask;
    /* @req 3.1.5/CAN260 */
    canUnit->state = CANIF_CS_STOPPED;

    return;
}

Can_ReturnType Can_SetControllerMode(uint8 controller,
        Can_StateTransitionType transition)
{
    /** @req 3.1.5/CAN230 */
    /** @req 3.1.5/CAN017 */
    /** !req 3.1.5/CAN294 Wakeup not supported */
    /** !req 3.1.5/CAN197 */
    /** !req 3.1.5/CAN201 */

    flexcan_t *canHw;
    imask_t state;
    Can_ReturnType rv = CAN_OK;

    /** @req 3.1.5/CAN198 */
    VALIDATE( (Can_Global.initRun == CAN_READY), CAN_SETCONTROLLERMODE_SERVICE_ID, CAN_E_UNINIT );
    /** @req 3.1.5/CAN199 */
    VALIDATE( VALID_CONTROLLER(controller), CAN_SETCONTROLLERMODE_SERVICE_ID, CAN_E_PARAM_CONTROLLER );
    Can_UnitType *canUnit = CTRL_TO_UNIT_PTR(controller);
    VALIDATE( (canUnit->state!=CANIF_CS_UNINIT), CAN_SETCONTROLLERMODE_SERVICE_ID, CAN_E_UNINIT );

    canHw = canUnit->hwPtr;

    switch (transition) {
    case CAN_T_START:
        ASLOG(CAN,"start CAN%d\n", controller);
        canHw->MCR.B.HALT = 0;
        canUnit->state = CANIF_CS_STARTED;
        Irq_Save(state);
        /** @req 3.1.5/CAN196 */
        if (canUnit->lock_cnt == 0)
        {
            Can_EnableControllerInterrupts(controller);
        }
        Irq_Restore(state);
        break;
    case CAN_T_WAKEUP: //CAN267
    case CAN_T_SLEEP: //CAN258, CAN290
        // Should be reported to DEM but DET is the next best
        VALIDATE(canUnit->state == CANIF_CS_STOPPED, 0x3, CAN_E_TRANSITION);
    case CAN_T_STOP:
        ASLOG(CAN,"stop CAN%d\n", controller);
        // Stop
        canHw->MCR.B.HALT = 1;
        canUnit->state = CANIF_CS_STOPPED;
        Can_AbortTx(canHw, canUnit); // CANIF282
        break;
    default:
        // Should be reported to DEM but DET is the next best
        /** @req 3.1.5/CAN200 */
        VALIDATE(canUnit->state == CANIF_CS_STOPPED, 0x3, CAN_E_TRANSITION);
        break;
    }

    return rv;
}

void Can_DisableControllerInterrupts(uint8 controller)
{
    /** @req 3.1.5/CAN231 */
    /** @req 3.1.5/CAN202 */
    /** !req 3.1.5/CAN204 */
    /** !req 3.1.5/CAN292 */

	VALIDATE_NO_RV( (Can_Global.initRun == CAN_READY), CAN_DISABLECONTROLLERINTERRUPTS_SERVICE_ID, CAN_E_UNINIT );
    /** @req 3.1.5/CAN206 */
    VALIDATE_NO_RV( VALID_CONTROLLER(controller) , CAN_DISABLECONTROLLERINTERRUPTS_SERVICE_ID, CAN_E_PARAM_CONTROLLER );
    Can_UnitType *canUnit = CTRL_TO_UNIT_PTR(controller);
    flexcan_t *canHw;
    imask_t state;

    /** @req 3.1.5/CAN205 */
    VALIDATE_NO_RV( (canUnit->state!=CANIF_CS_UNINIT), CAN_DISABLECONTROLLERINTERRUPTS_SERVICE_ID, CAN_E_UNINIT );



    Irq_Save(state);
    /** @req 3.1.5/CAN049 */
    if (canUnit->lock_cnt > 0) {
        // Interrupts already disabled
        canUnit->lock_cnt++;
        Irq_Restore(state);
        return;
    }
    canUnit->lock_cnt++;
    Irq_Restore(state);

    /* Don't try to be intelligent, turn everything off */
    canHw = GET_CONTROLLER(controller);

    /* Turn off the interrupt mailboxes */
#if defined(CFG_MPC563XM)
    if(controller==FLEXCAN_A){
        canHw->IMRH.R = 0;
    }
    canHw->IMRL.R = 0;
#else
    canHw->IMRH.R = 0;
    canHw->IMRL.R = 0;
#endif

    /* Turn off the bus off/tx warning/rx warning and error */
    canHw->MCR.B.WRNEN = 0; /* Disable warning int */
    canHw->CR.B.ERRMSK = 0; /* Disable error interrupt */
    canHw->CR.B.BOFFMSK = 0; /* Disable bus-off interrupt */
    canHw->CR.B.TWRNMSK = 0; /* Disable Tx warning */
    canHw->CR.B.RWRNMSK = 0; /* Disable Rx warning */
}

void Can_EnableControllerInterrupts(uint8 controller)
{
    /** @req 3.1.5/CAN232 */
    /** @req 3.1.5/CAN050 */
    /** !req 3.1.5/CAN293 Not applicable */

    Can_UnitType *canUnit;
    flexcan_t *canHw;
    imask_t state;

    /** @req 3.1.5/CAN209 */
	VALIDATE_NO_RV( (Can_Global.initRun == CAN_READY), CAN_ENABLECONTROLLERINTERRUPTS_SERVICE_ID, CAN_E_UNINIT );
    /** @req 3.1.5/CAN210 */
    VALIDATE_NO_RV( VALID_CONTROLLER(controller), CAN_ENABLECONTROLLERINTERRUPTS_SERVICE_ID, CAN_E_PARAM_CONTROLLER );
    canUnit = CTRL_TO_UNIT_PTR(controller);
    VALIDATE_NO_RV( (canUnit->state!=CANIF_CS_UNINIT), CAN_ENABLECONTROLLERINTERRUPTS_SERVICE_ID, CAN_E_UNINIT );

    Irq_Save(state);
    if (canUnit->lock_cnt > 1) {
        /** @req 3.1.5/CAN208 */
        // IRQ should still be disabled so just decrement counter
        canUnit->lock_cnt--;
        Irq_Restore(state);
        return;
    } else if (canUnit->lock_cnt == 1) {
        canUnit->lock_cnt = 0;
    }
    Irq_Restore(state);

    canHw = canUnit->hwPtr;

#if defined(CFG_MPC563XM)
    if(controller==FLEXCAN_A){
        canHw->IMRH.R = 0;
    }
    canHw->IMRL.R = 0;
#else
    canHw->IMRH.R = 0;
    canHw->IMRL.R = 0;
#endif

    if (canUnit->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_RX_PROCESSING_INTERRUPT ) {
        /* Turn on the interrupt mailboxes */
#if defined(CFG_MPC563XM)
        canHw->IMRL.R = canUnit->Can_Arc_RxMbMask;
		if(controller==FLEXCAN_A){
	        canHw->IMRH.R = (uint32_t)(canUnit->Can_Arc_RxMbMask>>32);
		}
#else
        canHw->IMRL.R = canUnit->Can_Arc_RxMbMask;
        canHw->IMRH.R = (uint32_t)(canUnit->Can_Arc_RxMbMask>>32);
#endif
    }

    if (canUnit->cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_TX_PROCESSING_INTERRUPT) {
        /* Turn on the interrupt mailboxes */
#if defined(CFG_MPC563XM)
        canHw->IMRL.R |= canUnit->Can_Arc_TxMbMask;
		if(controller==FLEXCAN_A){
	        canHw->IMRH.R |= (uint32_t)(canUnit->Can_Arc_TxMbMask>>32);
		}
#else
        canHw->IMRL.R |= canUnit->Can_Arc_TxMbMask;
        canHw->IMRH.R |= (uint32_t)(canUnit->Can_Arc_TxMbMask>>32);
#endif
    }

    // BusOff and warnings
    if (canUnit->cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT) {
        canHw->MCR.B.WRNEN = 1; /* Turn On warning int */
        canHw->CR.B.BOFFMSK = 1; /* Enable bus-off interrupt */

#if (USE_CAN_STATISTICS == STD_ON)
        canHw->CR.B.TWRNMSK = 1; /* Enable Tx warning */
        canHw->CR.B.RWRNMSK = 1; /* Enable Rx warning */
#endif
    }

    // errors
    if (canUnit->cfgCtrlPtr->Can_Arc_Flags &  CAN_CTRL_ERROR_PROCESSING_INTERRUPT) {
        canHw->CR.B.ERRMSK = 1; /* Enable error interrupt */
    }

    return;
}

Can_ReturnType Can_Write(Can_Arc_HTHType hth, Can_PduType *pduInfo)
{
    /** @req 3.1.5/CAN223 */
    /** !req 3.1.5/CAN213 Priorities not supported */
    /** !req 3.1.5/CAN215 Priorities not supported */
    /** @req 3.1.5/CAN214 */
    /** @req 3.1.5/CAN272 */

    uint16_t timer;
    uint64_t iflag;
    Can_ReturnType rv = CAN_OK;
    uint32_t mbNr;
    flexcan_t *canHw;
    const Can_HardwareObjectType *hohObj;
    Can_UnitType *canUnit;
    imask_t state;

    /** @req 3.1.5/CAN216 */
    VALIDATE( (Can_Global.initRun == CAN_READY), 0x6, CAN_E_UNINIT );
    /** @req 3.1.5/CAN219 */
    VALIDATE( (pduInfo != NULL), 0x6, CAN_E_PARAM_POINTER );
    /** @req 3.1.5/CAN218 */
    VALIDATE( (pduInfo->length <= 8), 0x6, CAN_E_PARAM_DLC );
    /** @req 3.1.5/CAN217 */
    VALIDATE( (hth < NUM_OF_HTHS ), 0x6, CAN_E_PARAM_HANDLE );

    canUnit = &CanUnit[Can_Global.config->CanConfigSet->ArcHthToUnit[hth]];
    hohObj  =  &canUnit->cfgHohPtr[Can_Global.config->CanConfigSet->ArcHthToHoh[hth]];
    canHw   =  canUnit->hwPtr;

    /* We have the hohObj, we need to know what box we can send on */
    Irq_Save(state);
    /* Get all free TX mboxes */
#if defined(CFG_MPC563XM)
    uint64_t iHwFlag;
    if(canHw==GET_CONTROLLER(FLEXCAN_A)){
        iHwFlag = *(uint64_t *)(&canHw->IFRH.R);  /* These are occupied */
    }else{
        iHwFlag = (uint64_t)(*(uint32_t *)(&canHw->IFRL.R));  /* These are occupied */
    }
#else
    uint64_t iHwFlag = *(uint64_t *)(&canHw->IFRH.R);  /* These are occupied */
#endif
    asAssert( (canUnit->Can_Arc_TxMbMask & hohObj->ArcMailboxMask) != 0);
    iflag = ~iHwFlag &  canUnit->mbTxFree & hohObj->ArcMailboxMask;
    /* Get the mbox(es) for this HTH */

    /** @req 3.1.5/CAN212 */
    if (iflag ) {
        mbNr = ilog2_64(iflag ); // find mb number

        /* Indicate that we are sending this MB */
        canUnit->mbTxFree &= ~(1ull<<mbNr);

        canHw->BUF[mbNr].CS.R = 0;
        canHw->BUF[mbNr].ID.R = 0;

        // Setup message box type
        if (hohObj->CanIdType == CAN_ID_TYPE_EXTENDED) {
            canHw->BUF[mbNr].CS.B.IDE = 1;
        } else if (hohObj->CanIdType == CAN_ID_TYPE_STANDARD) {
            canHw->BUF[mbNr].CS.B.IDE = 0;
        } else {
            // No support for mixed in this processor
            asAssert(0);
        }

        // Send on buf
        canHw->BUF[mbNr].CS.B.CODE = MB_INACTIVE; // Hold the transmit buffer inactive
        if (hohObj->CanIdType == CAN_ID_TYPE_EXTENDED) {
            canHw->BUF[mbNr].ID.R = pduInfo->id; // Write 29-bit MB IDs
        } else {
            asAssert( !(pduInfo->id & 0xfffff800) );
            canHw->BUF[mbNr].ID.B.STD_ID = pduInfo->id;
        }

#if defined(CFG_MPC5516) || defined(CFG_MPC5517) || defined(CFG_MPC5606S) || defined(CFG_MPC560XB) || defined(CFG_MPC5668) || defined(CFG_MPC5604P)
        canHw->BUF[mbNr].ID.B.PRIO = 1; // Set Local Priority
#endif

        memset(&canHw->BUF[mbNr].DATA, 0, 8);
        memcpy(&canHw->BUF[mbNr].DATA, pduInfo->sdu, pduInfo->length);

        canHw->BUF[mbNr].CS.B.SRR = 1;
        canHw->BUF[mbNr].CS.B.RTR = 0;

        canHw->BUF[mbNr].CS.B.LENGTH = pduInfo->length;
        canHw->BUF[mbNr].CS.B.CODE = MB_TX_ONCE; // Write tx once code
        timer = canHw->TIMER.R; // Unlock Message buffers

#if (USE_CAN_STATISTICS == STD_ON)
        canUnit->stats.txSuccessCnt++;
#endif

        // Store pdu handle in unit to be used by TxConfirmation
        canUnit->cfgCtrlPtr->Can_Arc_TxPduHandles[mbNr-canUnit->cfgCtrlPtr->Can_Arc_TxMailboxStart] = pduInfo->swPduHandle;

    } else {
        rv = CAN_BUSY;
    }
    Irq_Restore(state);

    return rv;
}

void Can_Cbk_CheckWakeup( uint8 controller ) {
    /** !req 3.1.5/CAN360 */
    /** !req 3.1.5/CAN361 */
    /** !req 3.1.5/CAN362 */
    /** !req 3.1.5/CAN363 */

    /* NOT SUPPORTED */
	(void)controller;
}


void Can_MainFunction_Write( void ) {
    /** !req 3.1.5/CAN225 */
    /** !req 3.1.5/CAN031 */
    /** !req 3.1.5/CAN178 */
    /** !req 3.1.5/CAN179 */
    Can_UnitType *uPtr;

    for(int i=0;i<CAN_ARC_CTRL_CONFIG_CNT; i++ ) {
        uPtr = &CanUnit[i];
        if( (uPtr->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_TX_PROCESSING_INTERRUPT) == 0 ) {
            Can_Isr_Tx(uPtr);
        }
    }
}

void Can_MainFunction_Read(void)
{
    /** !req 3.1.5/CAN226 */
    /** !req 3.1.5/CAN108 */
    /** !req 3.1.5/CAN180 */
    /** !req 3.1.5/CAN181 */
    Can_UnitType *uPtr;

    for(int i=0;i<CAN_ARC_CTRL_CONFIG_CNT; i++ ) {
        uPtr = &CanUnit[i];
        if( (uPtr->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_RX_PROCESSING_INTERRUPT) == 0 ) {
            Can_Isr_Rx(uPtr);
        }
    }
}

void Can_MainFunction_BusOff(void)
{
    /** !req 3.1.5/CAN227 */
    /** !req 3.1.5/CAN109 */
    /** !req 3.1.5/CAN183 */
    /** !req 3.1.5/CAN184 */
    /* Bus-off polling events */
    Can_UnitType *uPtr;

    for(int i=0;i<CAN_ARC_CTRL_CONFIG_CNT; i++ ) {
        uPtr = &CanUnit[i];
        if( (uPtr->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_BUSOFF_PROCESSING_INTERRUPT) == 0 ) {
        	Can_BusOff(uPtr->controllerId);
        }
    }
}

void Can_MainFunction_Error(void)
{
    /** !req 3.1.5/CAN227 */
    /** !req 3.1.5/CAN109 */
    /** !req 3.1.5/CAN183 */
    /** !req 3.1.5/CAN184 */
    /* Error polling events */
    Can_UnitType *uPtr;

    for(int i=0;i<CAN_ARC_CTRL_CONFIG_CNT; i++ ) {
        uPtr = &CanUnit[i];
        if( (uPtr->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_ERROR_PROCESSING_INTERRUPT) == 0 ) {
        	Can_Err(uPtr->controllerId);
        }
    }
}

void Can_MainFunction_Wakeup(void)
{
    /** !req 3.1.5/CAN228 */
    /** !req 3.1.5/CAN112 */
    /** !req 3.1.5/CAN185 */
    /** !req 3.1.5/CAN186 */
    /* Wakeup polling events */

    /* NOT SUPPORTED */
}

#if 0
Can_Arc_ProcessType Can_Arc_ProcessingMode( uint8 controller ) {
    Can_UnitType *uPtr = &CanUnit[controller];


    if(uPtr->cfgCtrlPtr->Can_Arc_Flags & CAN_CTRL_TX_PROCESSING_INTERRUPT ){
        Can_Isr_Tx(uPtr);
    }

}
#endif


#if (USE_CAN_STATISTICS == STD_ON)
/**
 * Get send/receive/error statistics for a controller
 *
 * @param controller The controller
 * @param stats Pointer to data to copy statistics to
 */


void Can_Arc_GetStatistics(uint8 controller, Can_Arc_StatisticsType *stats)
{
	if(Can_Global.initRun == CAN_READY)
	{
		Can_UnitType *canUnit = CTRL_TO_UNIT_PTR(controller);
		*stats = canUnit->stats;
	}
}
#endif

#ifdef USE_SHELL
#include "shell.h"
void statCan(void)
{
	Can_UnitType *uPtr;
	if(Can_Global.initRun == CAN_READY)
	{
		for(int i=0;i<CAN_ARC_CTRL_CONFIG_CNT; i++ ) {
			uPtr = &CanUnit[i];
			SHELL_printf("CAN%d state is %d\n"
					"  txSuccessCnt=%d, rxSuccessCnt=%d,\n"
					"  txErrorCnt=%d, rxErrorCnt=%d,\n"
					"  busOffCnt=%d, fifoOverflow=%d, fifoWarning=%d\n",
					i, uPtr->state,
					uPtr->stats.txSuccessCnt,
					uPtr->stats.rxSuccessCnt,
					uPtr->stats.txErrorCnt,
					uPtr->stats.rxErrorCnt,
					uPtr->stats.boffCnt,
					uPtr->stats.fifoOverflow,
					uPtr->stats.fifoWarning);
		}
	}
}
#endif

