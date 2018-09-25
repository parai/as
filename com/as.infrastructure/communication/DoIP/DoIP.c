/*-------------------------------- Arctic Core ------------------------------
 * Copyright (C) 2013, ArcCore AB, Sweden, www.arccore.com.
 * Contact: <contact@arccore.com>
 *
 * You may ONLY use this file:
 * 1)if you have a valid commercial ArcCore license and then in accordance with
 * the terms contained in the written license agreement between you and ArcCore,
 * or alternatively
 * 2)if you follow the terms found in GNU General Public License version 2 as
 * published by the Free Software Foundation and appearing in the file
 * LICENSE.GPL included in the packaging of this file or here
 * <http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>
 *-------------------------------- Arctic Core -----------------------------*/
#if defined(USE_DOIP) && ! defined (USE_SOAD)
#include "SoAd.h"
#include "SoAd_Internal.h"
#include "PduR_SoAd.h"
#include "TcpIp.h"
#include "Bsd.h"
#include "MemMap.h"

#include "Det.h"
#if defined(USE_DEM)
#include "Dem.h"
#endif

#include <string.h>
#include "asdebug.h"

#define DOIP_PROTOCOL_VERSION	2

// Generic doip header negative acknowledge codes
#define DOIP_E_INCORRECT_PATTERN_FORMAT	0x00
#define DOIP_E_UNKNOWN_PAYLOAD_TYPE		0x01
#define DOIP_E_MESSAGE_TO_LARGE			0x02
#define DOIP_E_OUT_OF_MEMORY			0x03
#define DOIP_E_INVALID_PAYLOAD_LENGTH	0x04

// Diagnostic message negative acknowledge codes
// 0x00�0x01  Reserved by document
#define DOIP_E_DIAG_INVALID_SA			0x02	// Invalid Source Address
#define DOIP_E_DIAG_UNKNOWN_TA			0x03	// Unknown Target Address
#define DOIP_E_DIAG_MESSAGE_TO_LARGE	0x04	// Diagnostic Message too large
#define DOIP_E_DIAG_OUT_OF_MEMORY		0x05	// Out of memory
#define DOIP_E_DIAG_TARGET_UNREACHABLE	0x06	// Target unreachable
#define DOIP_E_DIAG_UNKNOWN_NETWORK		0x07	// Unknown network
#define DOIP_E_DIAG_TP_ERROR			0x08	// Transport protocol error
// 0x09�0xFF  Reserved by document


static void handleTimeout(uint16 connectionIndex);

static uint8 VinGidSyncStatus = 0x10;

static enum { DOIP_LINK_UP, DOIP_LINK_DOWN } LinkStatus = DOIP_LINK_DOWN;

static uint32 DoIp_ArcAnnouncementTimer = 0;
static uint16 DoIp_ArcAnnounceWait;

// Table maintained
static DoIp_ArcDoIpSocketStatusType connectionStatus[DOIP_MAX_TESTER_CONNECTIONS];

/*
 * variable mapping from target node to last associated connection
 */
static uint16 targetConnectionMap[DOIP_TARGET_COUNT];

static uint16 pendingRoutingActivationSocket = 0xff;
static uint16 pendingRoutingActivationSa = 0xff;
static uint16 pendingRoutingActivationActivationType = 0xffff;
static uint8* pendingRoutingActivationTxBuffer = NULL;

#ifndef DOIP_RAND
static uint16 doip_rand(void) {
	// https://en.wikipedia.org/wiki/Xorshift
	static uint32 x = 123456789;
	static uint32 y = 362436069;
	static uint32 z = 521288629;
	static uint32 w = 88675123;
	uint32 t;

	t = x ^ (x << 11);
	x = y; y = z; z = w;
	w = w ^ (w >> 19) ^ (t ^ (t >> 8));
	return (uint16) w;
}

#define DOIP_RAND doip_rand
#endif

static void discardIpMessage(uint16 sockNr)
{
	// Discarding this message
}


/*
 * Compares two arrays, returns 0 if matching, otherwise non-zero.
 */
static uint8 bincmp(const uint8* arr1, const uint8* arr2, uint32 len) {
	uint32 i = 0;
	while (i < len) {
		uint8 res = *arr1 - *arr2;
		if (0 != res) {
			// non-matching!
			return res;
		}
		arr1++;
		arr2++;
		i++;
	}
	return 0;
}

void DoIp_LocalIpAddrAssignmentChg(SoAd_SoConIdType linkId, TcpIp_IpAddrStateType state) {
	switch (state) {
	case TCPIP_IPADDR_STATE_ASSIGNED:
		LinkStatus = DOIP_LINK_UP;
		DoIp_ArcAnnouncementTimer = 0;
		DoIp_ArcAnnounceWait = DOIP_RAND() % SOAD_DOIP_ANNOUNCE_WAIT;
		break;
	default:
		LinkStatus = DOIP_LINK_DOWN;
		DoIp_ArcAnnouncementTimer = 0xffff;
		break;
	}
}

static void createAndSendNack(uint16 sockNr, uint8 nackCode)
{
	uint8 txBuffer[8+1];
	uint16 bytesSent;


	txBuffer[0] = DOIP_PROTOCOL_VERSION;
	txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
	txBuffer[2] = 0x00;	// 0x0000->Negative Acknowledge
	txBuffer[3] = 0x00;
	txBuffer[4] = 0;	// Length = 0x00000001
	txBuffer[5] = 0;
	txBuffer[6] = 0;
	txBuffer[7] = 1;

	txBuffer[8] = nackCode;

	bytesSent = SoAd_SendIpMessage(sockNr, 8+1, txBuffer);
	if ( bytesSent != 8+1) {
		// Failed to send ack. Link is probably down.
		DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_CREATE_AND_SEND_NACK_ID, SOAD_E_UNEXPECTED_EXECUTION);
	}
}


static void createAndSendDiagnosticAck(uint16 sockNr, uint16 sa, uint16 ta)
{
	uint8 txBuffer[8+5];
	uint16 bytesSent;

	txBuffer[0] = DOIP_PROTOCOL_VERSION;
	txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
	txBuffer[2] = 0x80;	// 0x8002->Diagnostic Message Positive Acknowledge
	txBuffer[3] = 0x02;
	txBuffer[4] = 0;	// Length = 0x00000005
	txBuffer[5] = 0;
	txBuffer[6] = 0;
	txBuffer[7] = 5;

	txBuffer[8+0] = ta >> 8;	// TA
	txBuffer[8+1] = ta;

	txBuffer[8+2] = sa >> 8;	// SA
	txBuffer[8+3] = sa;

	txBuffer[8+4] = 0;		// ACK

	bytesSent = SoAd_SendIpMessage(sockNr, 8+5, txBuffer);
	if (bytesSent != 8+5) {
		// Failed to send ack. Link is probably down.
		DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_CREATE_AND_SEND_D_ACK_ID, SOAD_E_UNEXPECTED_EXECUTION);
	}
}


static void createAndSendDiagnosticNack(uint16 sockNr, uint16 sa, uint16 ta, uint8 nackCode)
{
	uint8 txBuffer[8+5];
	uint16 bytesSent;

	txBuffer[0] = DOIP_PROTOCOL_VERSION;
	txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
	txBuffer[2] = 0x80;	// 0x8003->Diagnostic Message Negative Acknowledge
	txBuffer[3] = 0x03;
	txBuffer[4] = 0;	// Length = 0x00000005
	txBuffer[5] = 0;
	txBuffer[6] = 0;
	txBuffer[7] = 5;

	txBuffer[8+0] = ta >> 8;	// TA
	txBuffer[8+1] = ta;

	txBuffer[8+2] = sa >> 8;	// SA
	txBuffer[8+3] = sa;

	txBuffer[8+4] = nackCode;

	bytesSent = SoAd_SendIpMessage(sockNr, 8+5, txBuffer);
	if (bytesSent != 8+5) {
		// Failed to send ack. Link is probably down.
		DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_CREATE_AND_SEND_D_NACK_ID, SOAD_E_UNEXPECTED_EXECUTION);
	}

}



static void createVehicleIdentificationResponse(uint8* txBuffer) {
	int i;

	txBuffer[0] = DOIP_PROTOCOL_VERSION;
	txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
	txBuffer[2] = 0x00;	// 0x0004->Vehicle Identification Response
	txBuffer[3] = 0x04;
	txBuffer[4] = 0;	// Length = 0x00000021 (33)
	txBuffer[5] = 0;
	txBuffer[6] = 0;
	txBuffer[7] = 33;

	// VIN field
	if (E_NOT_OK == SoAd_DoIp_Arc_GetVin(&txBuffer[8+0], 17)) {
		for (i = 0; i < 17; i++) {
			txBuffer[8+0+i] = 0;
		}
	}

	// Logical address field
	txBuffer[8+17+0] = (SoAd_Config.DoIpNodeLogicalAddress >> 8) & 0xff;
	txBuffer[8+17+1] = (SoAd_Config.DoIpNodeLogicalAddress >> 0) & 0xff;

	// EID field
	if (E_NOT_OK == SoAd_DoIp_Arc_GetEid(&txBuffer[8+19], 6)) {
		for (i = 0; i < 6; i++) {
			txBuffer[8+19+i] = 0;
		}
	}

	// GID field
	if (E_NOT_OK == SoAd_DoIp_Arc_GetGid(&txBuffer[8+25], 6)) {
		for (i = 0; i < 6; i++) {
			txBuffer[8+25+i] = 0;
		}
	}

	// Further action required field
	if (E_NOT_OK == SoAd_DoIp_Arc_GetFurtherActionRequired(&txBuffer[8+31])) {
		txBuffer[8+31] = 0x00;	// No further action required
	}

	// VIN/GID Sync Status field
	if (E_NOT_OK == SoAd_DoIp_Arc_GetFurtherActionRequired(&txBuffer[8+32])) {
		txBuffer[8+32] = VinGidSyncStatus;
	}

}

void DoIp_SendVehicleAnnouncement(uint16 sockNr) {
	uint8* txBuffer;
	uint16 bytesSent;

	if (SoAd_BufferGet(8+33, &txBuffer)) {
		createVehicleIdentificationResponse(txBuffer);

		bytesSent = SoAd_SendIpMessage(sockNr, 8+33, txBuffer);
		if (bytesSent != 8+33) {
			// Failed to send ack. Link is probably down.
			DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_VEHICLE_ID_REQ_ID, SOAD_E_UNEXPECTED_EXECUTION);
		}
		SoAd_BufferFree(txBuffer);

	} else {
		// Not enough buffers. Report error in error log.
		DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_VEHICLE_ID_REQ_ID, SOAD_E_NOBUFS);
	}
}

static void handleVehicleIdentificationReq(uint16 sockNr, uint32 payloadLength, uint8 *rxBuffer, SoAd_Arc_DoIp_VehicleIdentificationRequestType type)
{
	uint8* txBuffer;
	uint16 bytesSent;
	boolean doRespond = FALSE;

	if (SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_ALL == type) {
		if (0 == payloadLength) {
			doRespond = TRUE;

		} else {
			// Invalid payload length!
			createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
			SoAd_SocketClose(sockNr);
		}

	} else if (SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_BY_EID == type) {
		if (6 == payloadLength) {
			uint8 myEid[6];
			SoAd_DoIp_Arc_GetEid(myEid, sizeof(myEid));

			if (0 == bincmp(myEid, &rxBuffer[8], sizeof(myEid))) {
				doRespond = TRUE;
			}
		} else {
			// Invalid payload length!
			createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
			SoAd_SocketClose(sockNr);
		}
	} else if (SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_BY_VIN == type) {
		if (17 == payloadLength) {
			uint8 myVin[17];
			SoAd_DoIp_Arc_GetVin(myVin, sizeof(myVin));

			if (0 == bincmp(myVin, &rxBuffer[8], sizeof(myVin))) {
				doRespond = TRUE;
			}
		} else {
			// Invalid payload length!
			createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
			SoAd_SocketClose(sockNr);
		}
	} else {
		createAndSendNack(sockNr, DOIP_E_UNKNOWN_PAYLOAD_TYPE);
		SoAd_SocketClose(sockNr);
	}

	if (doRespond) {
		if (SoAd_BufferGet(8+33, &txBuffer)) {
			createVehicleIdentificationResponse(txBuffer);

			bytesSent = SoAd_SendIpMessage(sockNr, 8+33, txBuffer);
			if (bytesSent != 8+33) {
				// Failed to send data. Link is probably down.
				DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_VEHICLE_ID_REQ_ID, SOAD_E_UNEXPECTED_EXECUTION);
			}
			SoAd_BufferFree(txBuffer);


		} else {
			// Not enough tx buffer available. Report a Det error?
			DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_VEHICLE_ID_REQ_ID, SOAD_E_NOBUFS);
		}

	} else {
		createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
		SoAd_SocketClose(sockNr);
	}
}

static boolean isSourceAddressKnown(uint16 sa) {
	boolean saKnown = FALSE;
	uint16 i;


	for (i = 0; (i < DOIP_TESTER_COUNT) && (FALSE == saKnown); i++) {
		if (SoAd_Config.DoIpTesters[i].address  == sa) {
			saKnown = TRUE;
		}
	}

	return saKnown;
}

static boolean isRoutingTypeSupported(uint16 activationType) {
	boolean supported = FALSE;
	uint16 i;

	for (i = 0; (i < DOIP_ROUTINGACTIVATION_COUNT) && (FALSE == supported); i++) {
		if (SoAd_Config.DoIpRoutingActivations[i].activationNumber == activationType) {
			supported = TRUE;
		}
	}

	return supported;
}

static boolean isAuthenticationRequired(uint16 activationType) {
	boolean req = FALSE;
	uint16 i;

	for (i = 0; (i < DOIP_ROUTINGACTIVATION_COUNT) && (FALSE == req); i++) {
		if (SoAd_Config.DoIpRoutingActivations[i].activationNumber == activationType) {
			if (NULL == SoAd_Config.DoIpRoutingActivations[i].authenticationCallback) {
				req = FALSE;
			} else {
				req = TRUE;
			}
			return req;
		}
	}

	return req;
}

static boolean isAuthenticated(uint16 activationType) {
	// IMPROVEMENT: Authentication not supported
	return FALSE;
}

static boolean isConfirmationRequired(uint16 activationType) {
	boolean req = FALSE;
	uint16 i;

	for (i = 0; (i < DOIP_ROUTINGACTIVATION_COUNT) && (FALSE == req); i++) {
		if (SoAd_Config.DoIpRoutingActivations[i].activationNumber == activationType) {
			if (NULL == SoAd_Config.DoIpRoutingActivations[i].confirmationCallback) {
				req = FALSE;
			} else {
				req = TRUE;
			}
			return req;
		}
	}

	return req;
}

static boolean isConfirmed(uint16 activationType) {
	// IMPROVEMENT: Confirmation not supported
	return FALSE;
}

static void createAndSendAliveCheck(uint16 connectionIndex) {
	uint8 *txBuffer;
	uint16 bytesSent;

	if (SoAd_BufferGet(8, &txBuffer)) {
		txBuffer[0] = DOIP_PROTOCOL_VERSION;
		txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
		txBuffer[2] = 0x00;	// 0x0007->Alive check request
		txBuffer[3] = 0x07;
		txBuffer[4] = 0;	// Length = 0x0000000 (0)
		txBuffer[5] = 0;
		txBuffer[6] = 0;
		txBuffer[7] = 0;


		bytesSent = SoAd_SendIpMessage(connectionStatus[connectionIndex].sockNr, 8, txBuffer);
		if (bytesSent != 8) {
			/*
			 * Could not send data - socket probably broken, so let's close the socket.
			 */

			connectionStatus[connectionIndex].socketState = DOIP_ARC_CONNECTION_INVALID;

			/* If there are pending routing activations waiting, activate that one. */
			if (0xff != pendingRoutingActivationSocket) {
				handleTimeout(connectionIndex);
			} else {
				SoAd_SocketClose(connectionStatus[connectionIndex].sockNr);
			}


		}
		SoAd_BufferFree(txBuffer);

	} else {
		// No tx buffer available. Report a Det error.
		DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_CREATE_AND_SEND_ALIVE_CHECK_ID, SOAD_E_NOBUFS);
	}

}

static void startSingleSaAliveCheck(uint16 sa) {
	uint16 i;
	for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
		if (connectionStatus[i].sa == sa && FALSE == connectionStatus[i].awaitingAliveCheckResponse) {
			connectionStatus[i].awaitingAliveCheckResponse = TRUE;
			connectionStatus[i].generalInactivityTimer = 0;
			connectionStatus[i].aliveCheckTimer = 0;

			createAndSendAliveCheck(i);
		}

	}
}

static void startAllSocketsAliveCheck() {

	uint16 i;
	for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
		// No need to check connection states as this method is only called
		// when all sockets are in registered state...

		connectionStatus[i].awaitingAliveCheckResponse = TRUE;
		connectionStatus[i].generalInactivityTimer = 0;
		connectionStatus[i].aliveCheckTimer = 0;
		createAndSendAliveCheck(i);
	}
}

static void registerSocket(uint16 slotIndex, uint16 sockNr, uint16 activationType, uint16 sa) {
	asAssert(slotIndex < DOIP_MAX_TESTER_CONNECTIONS);

	connectionStatus[slotIndex].sockNr = sockNr;
	connectionStatus[slotIndex].sa = sa;

	connectionStatus[slotIndex].generalInactivityTimer = 0;
	connectionStatus[slotIndex].initialInactivityTimer = 0;

	connectionStatus[slotIndex].authenticated = FALSE;
	connectionStatus[slotIndex].confirmed = FALSE;

	connectionStatus[slotIndex].awaitingAliveCheckResponse = FALSE;

	connectionStatus[slotIndex].socketState = DOIP_ARC_CONNECTION_REGISTERED;
}

static DoIp_Arc_SocketAssignmentResultType socketHandle(uint16 sockNr, uint16 activationType, uint16 sa, uint8* routingActivationResponseCode)
{
	/*
	 * This method is intended to implement Figure 13 in ISO/FDIS 13400-2:2012(E)
	 */
	uint16 numRegisteredSockets = 0;
	uint16 i;
	boolean socketFound = FALSE;

	for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
		if (DOIP_ARC_CONNECTION_REGISTERED == connectionStatus[i].socketState) {
			numRegisteredSockets++;
		}
	}

	if (0 == numRegisteredSockets) {
		// No registered sockets, so we pick a slot for this connection:
		registerSocket(0, sockNr, activationType, sa);

		// No need to set *routingActivationResponseCode when socket assignment is successful.
		//*routingActivationResponseCode = 0;
		return DOIP_SOCKET_ASSIGNMENT_SUCCESSFUL;
	} else {
		// There is at least one registered socket already. Let's find out if it's this socket..
		for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
			if (connectionStatus[i].sockNr == sockNr) {
				socketFound = TRUE;

				// We found the TCP socket. Is it registered?
				if (connectionStatus[i].socketState == DOIP_ARC_CONNECTION_REGISTERED) {
					// We found the registered TCP socket. Is it assigned to this tester (SA)?
					if (connectionStatus[i].sa == sa) {
						/** @req DoIP-089 */
						return DOIP_SOCKET_ASSIGNMENT_SUCCESSFUL;

					} else {
						/** @req DoIP-106 */
						/* Routing activation denied because an SA different
						 * from the table connection entry was received on the
						 * already activated TCP_DATA socket. */
						*routingActivationResponseCode = 2;
						return DOIP_SOCKET_ASSIGNMENT_FAILED;
					}
				}
			}
		}
		/*
		 *  For loop terminated; that means that the current socket is currently not registered.
		 *
		 *  Next up: Check if SA is already registered to another socket...
		 */
		for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
			if ((connectionStatus[i].sa == sa) && DOIP_ARC_CONNECTION_REGISTERED == connectionStatus[i].socketState) {
				// Yes, the SA is already registered on another socket!
				/**
				 * @req DoIP-091
				 */
				// perform alive check single SA
				startSingleSaAliveCheck(sa);
				return DOIP_SOCKET_ASSIGNMENT_PENDING;
			}
		}

		/*
		 * For loop terminated; that means that the current SA is not already registered to another socket.
		 *
		 * Next up: Check to see that there is a free socket slot available..
		 */
		for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
			if (DOIP_ARC_CONNECTION_INVALID == connectionStatus[i].socketState) {
				/* Yes, we found a free slot */
				/** @req DoIp-090 */
				registerSocket(i, sockNr, activationType, sa);
				return DOIP_SOCKET_ASSIGNMENT_SUCCESSFUL;
			}
		}

		/*
		 * For loop terminated; that means that there are no free slots.
		 *
		 * Perform alive check on all registered sockets...
		 */
		/** @req DoIP-094 */
		startAllSocketsAliveCheck();
		return DOIP_SOCKET_ASSIGNMENT_PENDING;

	}
}


void fillRoutingActivationResponseData(uint8 *txBuffer, uint16 sa, uint8 routingActivationResponseCode)
{
    txBuffer[0] = DOIP_PROTOCOL_VERSION;
    txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
    txBuffer[2] = 0x00; // 0x0006->Routing Activation Response
    txBuffer[3] = 0x06;
    txBuffer[4] = 0; // Length = 0x0000000d (13)
    txBuffer[5] = 0;
    txBuffer[6] = 0;
    txBuffer[7] = 13;
    txBuffer[8] = sa >> 8;
    txBuffer[8 + 1] = sa;

	txBuffer[8 + 2] = (SoAd_Config.DoIpNodeLogicalAddress >> 8) & 0xff;
	txBuffer[8 + 3] = (SoAd_Config.DoIpNodeLogicalAddress >> 0) & 0xff;
    txBuffer[8 + 4] = routingActivationResponseCode;
    txBuffer[8 + 5] = 0; // Reserved
    txBuffer[8 + 6] = 0;
    txBuffer[8 + 7] = 0;
    txBuffer[8 + 8] = 0;
    txBuffer[8 + 9] = 0; // OEM use
    txBuffer[8 + 10] = 0;
    txBuffer[8 + 11] = 0;
    txBuffer[8 + 12] = 0;
}

static void handleRoutingActivationReq(uint16 sockNr, uint32 payloadLength, uint8 *rxBuffer)
{
	uint16 sa;
	uint16 activationType;

	uint8* txBuffer;
	uint16 bytesSent;

	/** @req SWS_DoIP_00117  is invalid (refers to the wrong service and does not match
	 * ISO13400-2:2012 issued on 2012-06-18)
	 *
	 * Decision made together with Customer's diagnostic tester group that activation
	 * type is only one byte, thus payload length may be 11 or 7. (Not 12 or 8 as
	 * AUTOSAR requirement states)
	 */
	if ((7 == payloadLength) || (11 == payloadLength)) {
		sa = (rxBuffer[8+0] << 8) + rxBuffer[8+1];
		activationType = rxBuffer[8+2];

		if (SoAd_BufferGet(8+13, &txBuffer)) {
			uint8 routingActivationResponseCode = 0x7e; // Vehicle manufacturer-specific

			if (TRUE == isSourceAddressKnown(sa)) {

				if (TRUE == isRoutingTypeSupported(activationType)) {

					DoIp_Arc_SocketAssignmentResultType socketHandleResult = socketHandle(sockNr, activationType, sa, &routingActivationResponseCode);
					if (DOIP_SOCKET_ASSIGNMENT_SUCCESSFUL == socketHandleResult) {
						boolean authenticated;

						if (TRUE == isAuthenticationRequired(activationType)) {
							authenticated = isAuthenticated(activationType);

						} else {
							authenticated = TRUE;
						}

						if (authenticated) {
							if (TRUE == isConfirmationRequired(activationType)) {
								if (TRUE == isConfirmed(activationType)) {
									/* Routing successfully activated */
									routingActivationResponseCode = 0x10;


								} else {
									/* Routing will be activated; confirmation required */
									routingActivationResponseCode = 0x11;

								}
							} else {
								/* Routing successfully activated */
								routingActivationResponseCode = 0x10;

							}


						} else {
							/* Routing activation rejected due to missing authentication */
							/** @req DoIP-061 */
							routingActivationResponseCode = 0x04;
						}

					} else if (DOIP_SOCKET_ASSIGNMENT_FAILED == socketHandleResult) { /* socketHandle */
						/* Routing activation denied because:
						 *  0x01 - all concurrently supported TCP_DATA sockets are
						 *         registered and active
						 *  0x02 - an SA different from the table connection entry
						 *         was received on the already activated TCP_DATA
						 *         socket
						 *  0x03 - the SA is already registered and active on a
						 *         different TCP_DATA socket
						 *
						 *  socketHandle() shall already have written the corresponding response code to
						 *  routingActivationResponseCode
						 */

						/* Validate response code */
						switch (routingActivationResponseCode) {
						case 0x01:
						case 0x02:
						case 0x03:
							/* OK! */
							break;
						default:
							/* Unsupported response code at this level */
							routingActivationResponseCode = 0x7e;
							DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ROUTING_ACTIVATION_REQ_ID, SOAD_E_SHALL_NOT_HAPPEN);
							break;
						}

					} else if (DOIP_SOCKET_ASSIGNMENT_PENDING == socketHandleResult) { /* socketHandle */
						/*
						 * Trying to assign a connection slot, but pending
						 * alive check responses before continuing.
						 * Continuation handled from DoIp_MainFunction (if a
						 * connection times out and thus becomes free) or from
						 * handleAliveCheckResp (if all connections remain
						 * active)
						 */
						if (NULL == pendingRoutingActivationTxBuffer) {
							pendingRoutingActivationTxBuffer = txBuffer;
							pendingRoutingActivationSa = sa;
							pendingRoutingActivationSocket = sockNr;
							pendingRoutingActivationActivationType = activationType;

						} else {
							// Socket assignment pending; alive check response already pending
							// This should not happen - the connection attempt should not have been accepted..
							DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ROUTING_ACTIVATION_REQ_ID, SOAD_E_SHALL_NOT_HAPPEN);
						}
						return;

					} else {
						/* This cannot happen */
						DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ROUTING_ACTIVATION_REQ_ID, SOAD_E_SHALL_NOT_HAPPEN);
					}

				} else { /* isRoutingTypeSupported(activationType) */
					/* Routing activation denied due to unsupported routing activation type */
					/** @req DoIP-151 */
					routingActivationResponseCode = 0x06;
				}

			} else { /* isSourceAddressKnown(sa) */
				/* Routing activation rejected due to unknown source address */
				/** @req DoIP-059 */
				routingActivationResponseCode = 0x00;
			}

			fillRoutingActivationResponseData(txBuffer, sa, routingActivationResponseCode);

			bytesSent = SoAd_SendIpMessage(sockNr, 8+13, txBuffer);
			if (bytesSent != 8+13) {
				// Could not send data - report error in development error tracer
				DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ROUTING_ACTIVATION_REQ_ID, SOAD_E_UNEXPECTED_EXECUTION);
			}
			SoAd_BufferFree(txBuffer);

			/**
			 * @req DoIP-102
			 *
			 * Negative response code --> Close socket on which the current message was received
			 */
			switch (routingActivationResponseCode) {
			case 0x10: // Routing activated.
			case 0x11: // Confirmation pending
			case 0x04: // Missing authentication
				break;


			default:
				SoAd_SocketClose(sockNr);
				break;
			}

		} else { /* if (SoAd_BufferGet(8+13, &txBuffer)) */
			DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ROUTING_ACTIVATION_REQ_ID, SOAD_E_NOBUFS);
			createAndSendNack(sockNr, DOIP_E_OUT_OF_MEMORY);
		}
	} else {
		createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
		SoAd_SocketClose(sockNr);
	}
}


typedef enum {
	LOOKUP_SA_TA_OK,
	LOOKUP_SA_TA_TAUNKNOWN,
	LOOKUP_SA_TA_SAERR
} LookupSaTaResultType;


LookupSaTaResultType lookupSaTa(uint16 connectionIndex, uint16 sa, uint16 ta, uint16* targetIndex)
{
	uint16 i;
	uint16 routingActivationIndex = 0xffff;


	if (0xffff == connectionIndex) {
		// Connection not registered!
		return LOOKUP_SA_TA_SAERR;
	}

	for (i = 0; i < DOIP_ROUTINGACTIVATION_COUNT; i++) {
		if (connectionStatus[i].activationType == SoAd_Config.DoIpRoutingActivations[i].activationNumber) {
			routingActivationIndex = i;
			break;
		}
	}

	if (0xffff == routingActivationIndex) {
		// No such routing activation!
		return LOOKUP_SA_TA_TAUNKNOWN;
	}

	for (i = 0; i < DOIP_ROUTINGACTIVATION_TO_TARGET_RELATION_COUNT; i++) {
		uint16 itTargetIndex = SoAd_Config.DoIpRoutingActivationToTargetAddressMap[i].target;

		if ((ta == SoAd_Config.DoIpTargetAddresses[itTargetIndex].addressValue) &&
			(routingActivationIndex == SoAd_Config.DoIpRoutingActivationToTargetAddressMap[i].routingActivation))
		{
			*targetIndex = itTargetIndex;
			return LOOKUP_SA_TA_OK;

		}
	}

	return LOOKUP_SA_TA_TAUNKNOWN;
}



static void handleEntityStatusReq(uint16 sockNr, uint32 payloadLength, uint8 *rxBuffer)
{
	if (0 == payloadLength) {
		uint8* txBuffer = NULL;
		if (SoAd_BufferGet(8+7, &txBuffer)) {
			uint8 maxNofSockets = SOAD_SOCKET_COUNT; // Or should we use SOAD_MAX_OPEN_SOCKETS?
			uint8 curNofOpenSockets = SoAd_GetNofCurrentlyUsedTcpSockets();
			uint32 maxDataSize = SOAD_RX_BUFFER_SIZE;
			uint16 bytesSent;

			txBuffer[0] = DOIP_PROTOCOL_VERSION;
			txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
			txBuffer[2] = 0x40;	// 0x4002->Entity status response
			txBuffer[3] = 0x02;
			txBuffer[4] = 0;	// Length = 0x00000007 (7)
			txBuffer[5] = 0;
			txBuffer[6] = 0;
			txBuffer[7] = 7;

			txBuffer[8] = SOAD_DOIP_NODE_TYPE;
			txBuffer[9] = maxNofSockets;  // Max TCP sockets
			txBuffer[10] = curNofOpenSockets; // Currently open TCP sockets

			txBuffer[11] = (maxDataSize << 24);
			txBuffer[12] = (maxDataSize << 16) & 0xff;
			txBuffer[13] = (maxDataSize << 8) & 0xff;
			txBuffer[14] = maxDataSize & 0xff;

			bytesSent = SoAd_SendIpMessage(sockNr, 8+7, txBuffer);
			if (bytesSent != 8+7) {
				/*
				 * Failed to send the message. Report error to Det.
				 */
				DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ENTITY_STATUS_REQ_ID, SOAD_E_UNEXPECTED_EXECUTION);
			}
			SoAd_BufferFree(txBuffer);

		} else {
			DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ENTITY_STATUS_REQ_ID, SOAD_E_NOBUFS);
			createAndSendNack(sockNr, DOIP_E_OUT_OF_MEMORY);

		}
	} else {
		// Invalid payload length!
		createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
		SoAd_SocketClose(sockNr);
	}
}


static void handlePowerModeCheckReq(uint16 sockNr, uint32 payloadLength, uint8 *rxBuffer)
{
	// @req SWS_DoIP_00091
	if (0 == payloadLength) {
		uint8* txBuffer = NULL;
		if (SoAd_BufferGet(8+1, &txBuffer)) {
			SoAd_DoIp_PowerMode powerMode;
			uint16 bytesSent;

			if (E_NOT_OK == SoAd_Config.DoIpConfig->DoipPowerModeCallback(&powerMode)) {
				// @req SWS_DoIP_00093
				powerMode = 0;
			}

			txBuffer[0] = DOIP_PROTOCOL_VERSION;
			txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
			txBuffer[2] = 0x40;	// 0x4002->Power mode check response
			txBuffer[3] = 0x04;
			txBuffer[4] = 0;	// Length = 0x00000001 (1)
			txBuffer[5] = 0;
			txBuffer[6] = 0;
			txBuffer[7] = 1;

			txBuffer[8] = (uint8)powerMode;

			// @req SWS_DoIP_00092
			bytesSent = SoAd_SendIpMessage(sockNr, 8+1, txBuffer);
			if (bytesSent != 8+1) {
				DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ENTITY_STATUS_REQ_ID, SOAD_E_UNEXPECTED_EXECUTION);
			}
			SoAd_BufferFree(txBuffer);

		} else {
			DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_ENTITY_STATUS_REQ_ID, SOAD_E_NOBUFS);
			createAndSendNack(sockNr, DOIP_E_OUT_OF_MEMORY);

		}
	} else {
		// Invalid payload length!
		createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
		SoAd_SocketClose(sockNr);
	}
}

static void handleAliveCheckResp(uint16 sockNr, uint16 payloadLength, uint8* rxBuffer) {
	uint16 i;
	uint16 sa;

	if (payloadLength == 2) {
		sa = (rxBuffer[8+0] << 8) | rxBuffer[8+1];

		for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
			if (connectionStatus[i].sockNr == sockNr) {
				if (connectionStatus[i].sa == sa) {
					// Alive check response received in time
					connectionStatus[i].generalInactivityTimer = 0;
					connectionStatus[i].awaitingAliveCheckResponse = FALSE;
				} else {
					// Alive check response received from the wrong SA!
					// What to do?
				}
			}
		}

		// Connections remaining to receive alive check responses for
		uint16 remainingConnections = 0;
		for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
			if (TRUE == connectionStatus[i].awaitingAliveCheckResponse) {
				remainingConnections++;
			}
		}

		if (0 == remainingConnections) {
			/*
			 * No remaining connections to receive alive check response for.
			 * This must mean that the incoming routing activation request must
			 * be denied...
			 */

			/* 0x01: Routing activation denied because all concurrently supported
			 *       TCP_DATA sockets are registered and active. */
			uint8 routingActivationResponseCode = 0x01;
			uint16 bytesSent;

			if ((pendingRoutingActivationSocket != 0xff) && (NULL != pendingRoutingActivationTxBuffer)) {
				fillRoutingActivationResponseData(pendingRoutingActivationTxBuffer, pendingRoutingActivationSa, routingActivationResponseCode);

				bytesSent = SoAd_SendIpMessage(pendingRoutingActivationSocket, 8+13, pendingRoutingActivationTxBuffer);

				if (bytesSent != (8+13)) {
					// Failed to send response. Connection probably already broken..
					DET_REPORTERROR(MODULE_ID_SOAD, 0, DOIP_HANDLE_ALIVECHECK_RESP, SOAD_E_UNEXPECTED_EXECUTION);
				}
				SoAd_BufferFree(pendingRoutingActivationTxBuffer);
				SoAd_SocketClose(pendingRoutingActivationSocket);
				pendingRoutingActivationTxBuffer = NULL;
				pendingRoutingActivationSocket = 0xff;
			}
		}

	}
}

static void handleTimeout(uint16 connectionIndex) {
	uint8 routingActivationResponseCode = 0x10;
	uint16 bytesSent;

	/*
	 * Close current connection (which has timed out anyway)
	 * and register a new connection with the pending routing activation
	 */
	SoAd_SocketClose(connectionStatus[connectionIndex].sockNr);

	if ((pendingRoutingActivationSocket != 0xff) && (NULL != pendingRoutingActivationTxBuffer)) {

		registerSocket(connectionIndex, pendingRoutingActivationSocket, pendingRoutingActivationActivationType, pendingRoutingActivationSa);

		fillRoutingActivationResponseData(pendingRoutingActivationTxBuffer, pendingRoutingActivationSa, routingActivationResponseCode);
		bytesSent = SoAd_SendIpMessage(pendingRoutingActivationSocket, 8+13, pendingRoutingActivationTxBuffer);

		if (bytesSent != (8+13)) {
			// Failed to send routing actication request..
			DET_REPORTERROR(MODULE_ID_SOAD, 0, DOIP_HANDLE_ALIVECHECK_TIMEOUT, SOAD_E_UNEXPECTED_EXECUTION);
		}
		SoAd_BufferFree(pendingRoutingActivationTxBuffer);
		pendingRoutingActivationTxBuffer = NULL;
		pendingRoutingActivationSocket = 0xff;
	} else {
		// Ordinary timeout set connection as free...
		connectionStatus[connectionIndex].socketState = DOIP_ARC_CONNECTION_INVALID;
		connectionStatus[connectionIndex].awaitingAliveCheckResponse = FALSE;
	}

}

static void associateTargetWithConnectionIndex(uint16 targetIndex, uint16 connectionIndex) {

	targetConnectionMap[targetIndex] = connectionIndex;
}

static void handleDiagnosticMessage(uint16 sockNr, uint32 payloadLength, uint8 *rxBuffer)
{
	LookupSaTaResultType lookupResult;
    BufReq_ReturnType result;
    PduInfoType* pduInfoPtr;
	uint16 sa;
	uint16 ta;
	uint16 targetIndex;
	uint16 connectionIndex;
	uint16 diagnosticMessageLength = payloadLength - 4;
	uint16 i;



	if (payloadLength >= 4) {
		sa = (rxBuffer[8] << 8) | rxBuffer[9];
		ta = (rxBuffer[10] << 8) | rxBuffer[11];


		// Find connection entry for this socket
		for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
			if ((sa == connectionStatus[i].sa) && (sockNr == connectionStatus[i].sockNr)) {
				connectionIndex = i;
				break;
			}
		}

		lookupResult = lookupSaTa(connectionIndex, sa, ta, &targetIndex);
		if (lookupResult == LOOKUP_SA_TA_OK) {
			// Send diagnostic message to PduR
			result = PduR_SoAdTpProvideRxBuffer(SoAd_Config.DoIpTargetAddresses[targetIndex].txPdu, diagnosticMessageLength, &pduInfoPtr);
			if (result == BUFREQ_OK) {
				associateTargetWithConnectionIndex(targetIndex, connectionIndex);

				(void)lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, 12, 0);
				(void)lwip_recv(SocketAdminList[sockNr].ConnectionHandle, pduInfoPtr->SduDataPtr, payloadLength - 4, 0);
				PduR_SoAdTpRxIndication(SoAd_Config.DoIpTargetAddresses[targetIndex].txPdu, NTFRSLT_OK);

				// Send diagnostic message positive ack
				createAndSendDiagnosticAck(sockNr, sa, ta);
			} else if (result != BUFREQ_BUSY){
				createAndSendDiagnosticNack(sockNr, sa, ta, DOIP_E_DIAG_OUT_OF_MEMORY);
				discardIpMessage(sockNr);
				DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_DIAG_MSG_ID, SOAD_E_SHALL_NOT_HAPPEN);
			}

		} else if (lookupResult == LOOKUP_SA_TA_TAUNKNOWN) {
			// TA not known
			createAndSendDiagnosticNack(sockNr, sa, ta, DOIP_E_DIAG_UNKNOWN_TA);
			discardIpMessage(sockNr);
		} else {
			// SA not registered on receiving socket
			createAndSendDiagnosticNack(sockNr, sa, ta, DOIP_E_DIAG_INVALID_SA);
			SoAd_SocketClose(sockNr);
		}
	} else {
		createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
		SoAd_SocketClose(sockNr);
	}
}


void DoIp_HandleTcpRx(uint16 sockNr)
{
	int nBytes;
	uint8* rxBuffer;
	uint16 payloadType;
	uint16 payloadLength;

	if (SoAd_BufferGet(SOAD_RX_BUFFER_SIZE, &rxBuffer)) {
		nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, SOAD_RX_BUFFER_SIZE, MSG_PEEK);
		SoAd_SocketStatusCheck(sockNr, SocketAdminList[sockNr].ConnectionHandle);
		if (nBytes >= 8) {
			if ((rxBuffer[0] == DOIP_PROTOCOL_VERSION) && ((uint8)(~rxBuffer[1]) == DOIP_PROTOCOL_VERSION)) {
				payloadType = rxBuffer[2] << 8 | rxBuffer[3];
				payloadLength = (rxBuffer[4] << 24) | (rxBuffer[5] << 16) | (rxBuffer[6] << 8) | rxBuffer[7];
				if ((payloadLength + 8) <= SOAD_RX_BUFFER_SIZE) {
					if ((payloadLength + 8) <= nBytes) {
						// Grab the message
						switch (payloadType) {
#if 0 /* Vehicle identification requests are not to be supported over TCP */
						case 0x0001:	// Vehicle Identification Request
							nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							handleVehicleIdentificationReq(sockNr, payloadLength, rxBuffer, SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_ALL);
							break;

						case 0x0002:	// Vehicle Identification Request with EID
							nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							handleVehicleIdentificationReq(sockNr, payloadLength, rxBuffer, SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_BY_EID);
							break;

						case 0x0003:	// Vehicle Identification Request with VIN
							nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							handleVehicleIdentificationReq(sockNr, payloadLength, rxBuffer, SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_BY_VIN);
							break;
#endif /* Vehicle identification requests are not to be supported over TCP */

						case 0x005:		// Routing Activation request
							nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							handleRoutingActivationReq(sockNr, payloadLength, rxBuffer);
							break;

						case 0x008:     // Alive check response
							nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							handleAliveCheckResp(sockNr, payloadLength, rxBuffer);
							break;

						case 0x4003:
							// @req SWS_DoIP_00090
							nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							createAndSendNack(sockNr, DOIP_E_INVALID_PAYLOAD_LENGTH);
							break;

						case 0x8001:	// Diagnostic message
							//nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							handleDiagnosticMessage(sockNr, payloadLength, rxBuffer);
							break;

						default:
							nBytes = lwip_recv(SocketAdminList[sockNr].ConnectionHandle, rxBuffer, payloadLength + 8, 0);
							createAndSendNack(sockNr, DOIP_E_UNKNOWN_PAYLOAD_TYPE);
							break;
						}
					}
				} else {
					createAndSendNack(sockNr, DOIP_E_MESSAGE_TO_LARGE);
					discardIpMessage(sockNr);
				}
			} else {
				createAndSendNack(sockNr, DOIP_E_INCORRECT_PATTERN_FORMAT);
				SoAd_SocketClose(sockNr);
			}
		}

		SoAd_BufferFree(rxBuffer);
	} else {
		// No rx buffer available. Report this in Det. Message should be handled in the next (scanSockets) loop.
		DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_TCP_RX_ID, SOAD_E_NOBUFS);
	}
}


void DoIp_HandleUdpRx(uint16 sockNr)
{
	int nBytes;
	uint8* rxBuffer;
	uint16 payloadType;
	uint16 payloadLength;
	struct sockaddr_in fromAddr;
    socklen_t fromAddrLen = sizeof(fromAddr);

	if (SoAd_BufferGet(SOAD_RX_BUFFER_SIZE, &rxBuffer)) {
	    nBytes = lwip_recvfrom(SocketAdminList[sockNr].SocketHandle, rxBuffer, SOAD_RX_BUFFER_SIZE, MSG_PEEK, (struct sockaddr*)&fromAddr, &fromAddrLen);
		SoAd_SocketStatusCheck(sockNr, SocketAdminList[sockNr].SocketHandle);
		if (nBytes >= 8) {
			if ((rxBuffer[0] == DOIP_PROTOCOL_VERSION) && ((uint8)(~rxBuffer[1]) == DOIP_PROTOCOL_VERSION)) {
				payloadType = rxBuffer[2] << 8 | rxBuffer[3];
				payloadLength = (rxBuffer[4] << 24) | (rxBuffer[5] << 16) | (rxBuffer[6] << 8) | rxBuffer[7];
				if ((payloadLength + 8) <= SOAD_RX_BUFFER_SIZE) {
					if ((payloadLength + 8) <= nBytes) {
						// Grab the message
						nBytes = lwip_recvfrom(SocketAdminList[sockNr].SocketHandle, rxBuffer, payloadLength + 8, 0, (struct sockaddr*)&fromAddr, &fromAddrLen);
						SocketAdminList[sockNr].RemotePort = fromAddr.sin_port;
						SocketAdminList[sockNr].RemoteIpAddress = fromAddr.sin_addr.s_addr;
						switch (payloadType) {
						case 0x0001:	// Vehicle Identification Request
							handleVehicleIdentificationReq(sockNr, payloadLength, rxBuffer, SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_ALL);
							break;

						case 0x0002:	// Vehicle Identification Request with EID
							handleVehicleIdentificationReq(sockNr, payloadLength, rxBuffer, SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_BY_EID);
							break;

						case 0x0003:	// Vehicle Identification Request with VIN
							handleVehicleIdentificationReq(sockNr, payloadLength, rxBuffer, SOAD_ARC_DOIP_IDENTIFICATIONREQUEST_BY_VIN);
							break;

#if 0 /* Routing activation is not to be supported over UDP */
						case 0x005:		// Routing Activation request
							handleRoutingActivationReq(sockNr, payloadLength, rxBuffer);
							break;
#endif /* Routing activation is not to be supported over UDP */

						case 0x4001:    /* DoIP entity status request */
							handleEntityStatusReq(sockNr, payloadLength, rxBuffer);
							break;

						case 0x4003:    /* DoIP power mode check request */
							handlePowerModeCheckReq(sockNr, payloadLength, rxBuffer);
							break;


#if 0 /* Diagnostic messages is not to be supported over UDP */
						case 0x8001:	// Diagnostic message
							handleDiagnosticMessage(sockNr, payloadLength, rxBuffer);
							break;
#endif  /* Diagnostic messages is not to be supported over UDP */

						default:
							createAndSendNack(sockNr, DOIP_E_UNKNOWN_PAYLOAD_TYPE);
							break;
						}
					}
				} else {
					createAndSendNack(sockNr, DOIP_E_MESSAGE_TO_LARGE);
					discardIpMessage(sockNr);
				}
			} else {
				createAndSendNack(sockNr, DOIP_E_INCORRECT_PATTERN_FORMAT);
				SoAd_SocketClose(sockNr);
			}
		}

		SoAd_BufferFree(rxBuffer);
	} else {
		// No rx buffer available. Report this in Det. Message should be handled in the next (scanSockets) loop.
		DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_UDP_RX_ID, SOAD_E_NOBUFS);
	}
}


Std_ReturnType DoIp_HandleTpTransmit(PduIdType SoAdSrcPduId, const PduInfoType* SoAdSrcPduInfoPtr)
{
	Std_ReturnType returnCode = E_OK;
	BufReq_ReturnType PdurResult;
	PduInfoType* txPduInfoBuffer;
	uint16 socketNr;
	const uint32 pduLength = SoAdSrcPduInfoPtr->SduLength;
	const uint32 payloadLength = pduLength + 4;
	uint8* txBuffer;
	uint16 bytesSent;

	/*
	 * Find which target the incoming Pdu belongs to:
	 */
	uint16 i;
	uint16 targetIndex = 0xffff;
	for (i = 0; i < DOIP_TARGET_COUNT; i++) {
		if (SoAd_Config.DoIpTargetAddresses[i].rxPdu == SoAdSrcPduId) {
			// Match!
			targetIndex = i;
			break;
		}
	}

	if (targetIndex == 0xffff) {
		// Did not find corresponding target. Most likely due to faulty configuration.
		return E_NOT_OK;
	}


	if (PduAdminList[SoAdSrcPduId].PduStatus == PDU_IDLE ) {
		socketNr = SoAd_Config.PduRoute[SoAdSrcPduId].DestinationSocketRef->SocketId;
		if ((SocketAdminList[socketNr].SocketState == SOCKET_TCP_LISTENING)
			|| (SocketAdminList[socketNr].SocketState == SOCKET_TCP_READY)
			|| (SocketAdminList[socketNr].SocketState == SOCKET_UDP_READY))
		{
				PduAdminList[SoAdSrcPduId].PduStatus = PDU_TP_REQ_BUFFER;
				PdurResult = PduR_SoAdTpProvideTxBuffer(SoAd_Config.PduRoute[SoAdSrcPduId].SourcePduId, &txPduInfoBuffer, 0);
				if ((PdurResult == BUFREQ_OK) && (txPduInfoBuffer->SduLength >= pduLength)) {
					if (SoAd_BufferGet(8+13, &txBuffer)) {

						uint16 connectionId = targetConnectionMap[targetIndex];
						uint16 ta = connectionStatus[connectionId].sa; // Target of response is the source of the initiating party...
						uint16 sa = SoAd_Config.DoIpTargetAddresses[targetIndex].addressValue;


						PduAdminList[SoAdSrcPduId].PduStatus = PDU_TP_SENDING;
						txBuffer[0] = DOIP_PROTOCOL_VERSION;
						txBuffer[1] = ~DOIP_PROTOCOL_VERSION;
						txBuffer[2] = 0x80;	// 0x8001->Diagnostic message
						txBuffer[3] = 0x01;
						txBuffer[4] = payloadLength >> 24;
						txBuffer[5] = payloadLength >> 16;
						txBuffer[6] = payloadLength >> 8;
						txBuffer[7] = payloadLength >> 0;

						txBuffer[8+0] = sa >> 8;
						txBuffer[8+1] = sa >> 0;

						txBuffer[8+2] = ta >> 8;
						txBuffer[8+3] = ta >> 0;

						// Copy the Pdu to the tx buffer
						memcpy(&txBuffer[8+4], txPduInfoBuffer->SduDataPtr, pduLength);

						// Then send the diagnostic message and confirm transmission to PduR
						bytesSent = SoAd_SendIpMessage(socketNr, pduLength + 12, txBuffer);

						if (bytesSent == pduLength + 12) {
							SoAd_BufferFree(txBuffer);
							PduR_SoAdTpTxConfirmation(SoAd_Config.PduRoute[SoAdSrcPduId].SourcePduId, NTFRSLT_OK);
						} else {
							// All bytes were not sent - discarding this and setting a Det error instead.
							SoAd_BufferFree(txBuffer);
							DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_TP_TRANSMIT_ID, SOAD_E_UNEXPECTED_EXECUTION);

							// Notify PduR abotu failed transmission.
							PduR_SoAdTpTxConfirmation(SoAd_Config.PduRoute[SoAdSrcPduId].SourcePduId, NTFRSLT_E_NOT_OK);

						}
					} else {
						// No buffer to send with. Report failure back to PduR.
						DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_TP_TRANSMIT_ID, SOAD_E_NOBUFS);
						PduR_SoAdTpTxConfirmation(SoAd_Config.PduRoute[SoAdSrcPduId].SourcePduId, NTFRSLT_E_NO_BUFFER);
					}
				} else {
					// PduR did not return a buffer as expected. This signals something wrong with the PduR or its source module (probably CanTp)
					DET_REPORTERROR(MODULE_ID_SOAD, 0, SOAD_DOIP_HANDLE_TP_TRANSMIT_ID, SOAD_E_UNEXPECTED_EXECUTION);
				}
				PduAdminList[SoAdSrcPduId].PduStatus = PDU_IDLE;
		} else {
			/* Socket not ready */
			returnCode = E_NOT_OK;
		}
	} else {
		/* A PDU is already in progress */
		returnCode = E_NOT_OK;
	}

	return returnCode;
}


void DoIp_MainFunction() {

	uint16 i;
	static uint16 numAnnouncements = 0;

	if (DOIP_LINK_UP == LinkStatus) {
		DoIp_ArcAnnouncementTimer += DOIP_MAINFUNCTION_PERIOD_TIME;
		if ((DoIp_ArcAnnounceWait <= DoIp_ArcAnnouncementTimer) &&
			((DoIp_ArcAnnounceWait + SOAD_DOIP_ANNOUNCE_NUM * SOAD_DOIP_ANNOUNCE_INTERVAL) > DoIp_ArcAnnouncementTimer))
		{
			/* Within announcement period */

			uint32 timer = DoIp_ArcAnnouncementTimer - DoIp_ArcAnnounceWait;

			if (numAnnouncements * SOAD_DOIP_ANNOUNCE_INTERVAL <= timer) {
				/* Time to send another announcement */
				DoIp_SendVehicleAnnouncement(SOAD_DOIP_ANNOUNCE_SOCKET);
				numAnnouncements++;
			}

		} else {
			numAnnouncements = 0;
		}
	} else {
		numAnnouncements = 0;
	}


	/*
	 * Handle DoIP connection timeouts..
	 */
	for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {

		if (DOIP_ARC_CONNECTION_REGISTERED == connectionStatus[i].socketState) {

			/*
			 * Handle Alive check timeouts
			 */
			if (TRUE == connectionStatus[i].awaitingAliveCheckResponse) {
				if (connectionStatus[i].aliveCheckTimer > DOIP_ALIVECHECK_RESPONSE_TIMEOUT) {
					handleTimeout(i);

				} else {
					connectionStatus[i].aliveCheckTimer += DOIP_MAINFUNCTION_PERIOD_TIME;

				}
			}

			/*
			 * Handle general inactivity timeouts
			 */
			if ((connectionStatus[i].generalInactivityTimer <= DOIP_GENERAL_INACTIVITY_TIMEOUT) ||
					(connectionStatus[i].generalInactivityTimer <= DOIP_ALIVECHECK_RESPONSE_TIMEOUT))
			{
				connectionStatus[i].generalInactivityTimer += DOIP_MAINFUNCTION_PERIOD_TIME;

			} else {
				// Timeout!
				handleTimeout(i);

			}

			/*
			 * IMPROVEMENT: Handle initial inactivity timeouts
			 */
			if ((connectionStatus[i].initialInactivityTimer <= DOIP_GENERAL_INACTIVITY_TIMEOUT) ||
					(connectionStatus[i].initialInactivityTimer <= DOIP_ALIVECHECK_RESPONSE_TIMEOUT))
			{
				connectionStatus[i].initialInactivityTimer += DOIP_MAINFUNCTION_PERIOD_TIME;
			}
		}
	}
}

void DoIp_Init() {
	uint16 i;

	for (i = 0; i < DOIP_MAX_TESTER_CONNECTIONS; i++) {
		connectionStatus[i].socketState = DOIP_ARC_CONNECTION_INVALID;
		connectionStatus[i].sockNr = 0xff;
		connectionStatus[i].awaitingAliveCheckResponse = FALSE;
		connectionStatus[i].generalInactivityTimer = 0;
		connectionStatus[i].initialInactivityTimer = 0;
		connectionStatus[i].aliveCheckTimer = 0;
	}
}
#endif
