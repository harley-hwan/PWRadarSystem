/*
 * SocketUtility.h
 *
 *  Created on: Aug 23, 2024
 *      Author: Hyeongwon Jeon
 */

#ifndef SOCKETUTILITY_H_
#define SOCKETUTILITY_H_

#ifdef _RDMA_SOURCE
#define __USE_RDMA 1
#else
#define __USE_RDMA 0
#endif

#include <stdio.h>			// For Standard Input/Output
#include <stdint.h>			// For Data Type Definition
#include <float.h>			// For Data Type Definition
#include <unistd.h>			// For UNIX Standard Input/Output
#include <string.h>			// For Memory Copy & Clear
#include <pthread.h>		// For Thread & Mutex
#include <arpa/inet.h>		// For Socket Address Set
#include <sys/socket.h>		// For Socket Communication
#include <sys/time.h>		// For Socket Timeout Set

#if(__USE_RDMA == 1)
#include <semaphore.h>
#include <rdma/rdma_cma.h>
#include "Rdma_define.h"
#include "libedrdma.h"
#endif

#define SOCKETUTILITY_VER_MAJOR		5
#define SOCKETUTILITY_VER_MINOR		0
#define SOCKETUTILITY_BUILD_DATE	20240823

// 1.0(20220503) : First Release
// 1.1(20220524) : Change structure name (st_VerInfo ->  st_VerInfo_Socket)
// 1.2(20230316) : Edit MISRA-C FalseAlarm(1st)
// 1.3(20230806) : Edit DataType Define based on stdint.h & float.h
// 2.0(20231026) : Consider Control over 2GB
// 3.0(20240105) : Edit MISRA-C FalseAlarm(2nd)
//                 Remove IB function
// 4.0(20240517) : Debug f_SocketSendTCP_IPv4(Add Memory offset during send)
// 5.0(20240823) : Add RDMA function

/* ================================================================================================= */
/*                                        Control Value Define 		                                 */
/* ================================================================================================= */
#define DISP_SOCKET_ERROR_WARNING	1					// 0:OFF, 1:ON
#define DISP_SOCKET_RESULT			0					// 0:OFF, 1:ON(Result Only), 2:On(Result + Detail Info)

#define CTRL_SOCKET_REPEAT_CONNECT	1					// 0:OFF, 1:ON
#define CTRL_SOCKET_LINK_RECOVERY	1					// 0:OFF, 1:ON

/* ================================================================================================= */
/*                                          DataType Define 		                                 */
/* ================================================================================================= */
typedef void						SOCKET_VOID;
typedef	int8_t						SOCKET_CHAR8;
typedef	uint8_t						SOCKET_UCHAR8;
typedef uint16_t					SOCKET_UINT16;
typedef int32_t						SOCKET_INT32;
typedef uint32_t					SOCKET_UINT32;
typedef int64_t						SOCKET_INT64;
typedef uint64_t					SOCKET_UINT64;

/* ================================================================================================= */
/*                                        Global Value Define 		                                 */
/* ================================================================================================= */
#define SOCKET_ALWAYS						1

#define SOCKET_PASS							1
#define SOCKET_FAIL							0

#define SOCKET_MBYTE2BYTE					(SOCKET_INT64)(1024*1024)

// TCP
#define SOCKET_TCP_QUEUE_SIZE				5		// TCP Connection Waiting Queue Size

// TCP Sync Message
#define SOCKET_SYNC_PASS_TX_LINE_CHECK 		0xAAAA
#define SOCKET_SYNC_PASS_RX_LINE_CHECK 		0xBBBB
#define SOCKET_SYNC_PASS_LISTEN		 		0xCCCC
#define SOCKET_SYNC_PASS_CONNECT		 	0xDDDD
#define SOCKET_SYNC_PASS_SEND_TO_TX	 		0xEEEE
#define SOCKET_SYNC_PASS_SEND_TO_RX 		0xFFFF

// RDMA Message
#define SOCKET_RDMA_RECV_DONE				0xFFAA

enum{
	// Socket Status
	enum_Socket_Status_Disconnected = -1,
	enum_Socket_Status_NotDefined = 0,		// Always when UDP
	enum_Socket_Status_Connected = 1,
};

enum{
	// Socket Type
	enum_Socket_Type_NotDefined = 0,

	enum_Socket_Type_UDP_IPv4Tx = 1,
	enum_Socket_Type_UDP_IPv4Rx = 2,

	enum_Socket_Type_TCP_IPv4Tx = 3,
	enum_Socket_Type_TCP_IPv4Rx = 4,

#if(__USE_RDMA == 1)
	enum_Socket_Type_RDMA_IPv4Tx = 5,
	enum_Socket_Type_RDMA_IPv4Rx = 6,
#endif
};

#if(__USE_RDMA == 1)	
enum{
	// RDMA Mode
	enum_Socket_Mode_RDMA_IPv4Rx = 0,	// Client
	enum_Socket_Mode_RDMA_IPv4Tx = 1,	// Server
};
#endif

/* ================================================================================================= */
/*                                          Structure Define 		                                 */
/* ================================================================================================= */
/*************************************** #pragma pack(push, 1) ***************************************/
#pragma pack(push, 1)

#if(__USE_RDMA == 1)
typedef struct st_CtrlInfo_RDMA
{
	SOCKET_INT32				uiTxBuffSize_Byte;
	SOCKET_INT32				uiRxBuffSize_Byte;
	SOCKET_UINT32 				uiDataOffset;
	SOCKET_UINT32 				uiDataLength;
}st_CtrlInfo_RDMA;
#endif

/* ================================================================================================= */

typedef struct st_VerInfo_SOCKET
{
	SOCKET_INT32				iMajorVersion;
	SOCKET_INT32				iMinorVersion;
	SOCKET_INT32				iBuildDate;			// YYYYMMDD
}st_VerInfo_SOCKET;

typedef struct st_Socket
{
	SOCKET_INT32 				iSockId;
	SOCKET_INT32 				iSockStatus;
	SOCKET_INT32 				iSockType;
	SOCKET_UINT32 				uiSockAddrSize;
	struct sockaddr_in 			stSockAddr;
	struct timeval 				stTimeOutVal;

#if(__USE_RDMA == 1)
	SOCKET_CHAR8				caIpAddr[100];
	SOCKET_UINT16				usPortNum;
	SOCKET_UINT32				uiBuffSize_Byte;
	struct edrdma_cb *			stpRDMA_CtrlBlock;
	st_CtrlInfo_RDMA *			stpRDMA_CtrlInfo;
#endif
}st_Socket;

#pragma pack(pop)
/*************************************** #pragma pack(push, 1) ***************************************/

/* ================================================================================================= */
/*                                        Previous Declaration                                       */
/* ================================================================================================= */
SOCKET_VOID f_SocketGetVerInfo(st_VerInfo_SOCKET *stpVerInfo);

SOCKET_INT32 f_SocketSetUDP_PartitionSize(const SOCKET_INT32 iUDP_PartitionSize_Byte);
SOCKET_VOID f_SocketGetUDP_PartitionSize(SOCKET_INT32 *ipUDP_PartitionSize_Byte);
SOCKET_INT32 f_SocketSetUDP_PartitionSendGap(const SOCKET_UINT32 uiUDP_PartitionSendGap_us);
SOCKET_VOID f_SocketGetUDP_PartitionSendGap(SOCKET_UINT32 *uipUDP_PartitionSendGap_us);

st_Socket f_SocketInitUDP_IPv4Tx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum);
st_Socket f_SocketInitUDP_IPv4Rx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us);
SOCKET_INT64 f_SocketSendUDP_IPv4_Normal(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lDataSize);
SOCKET_INT64 f_SocketSendUDP_IPv4_Partition(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize);
SOCKET_INT64 f_SocketRecvUDP_IPv4_Normal(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lMaxSize);
SOCKET_INT64 f_SocketRecvUDP_IPv4_Partition(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize);

st_Socket f_SocketInitTCP_IPv4Tx_Normal(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us);
st_Socket f_SocketInitTCP_IPv4Tx_Sync(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us);
st_Socket f_SocketInitTCP_IPv4Rx_Normal(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us);
st_Socket f_SocketInitTCP_IPv4Rx_Sync(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us, const SOCKET_INT32 iMaxSyncTime_s);
SOCKET_INT64 f_SocketSendTCP_IPv4(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize);
SOCKET_INT64 f_SocketRecvTCP_IPv4(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize);

#if(__USE_RDMA == 1)
st_Socket f_SocketInitRDMA_IPv4Tx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us);
st_Socket f_SocketInitRDMA_IPv4Rx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us);
SOCKET_INT64 f_SocketSendRDMA_IPv4_OffsetResetEveryTime(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lDataSize);
SOCKET_INT64 f_SocketSendRDMA_IPv4_OffsetResetWhenFull(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize);
SOCKET_INT64 f_SocketRecvRDMA_IPv4_OffsetResetEveryTime(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr);
SOCKET_INT64 f_SocketRecvRDMA_IPv4_OffsetResetWhenFull(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize);
#endif

SOCKET_INT32 f_SocketClose(st_Socket stSocket);

/* ================================================================================================= */
/*                                         Extern Global Value 		                                 */
/* ================================================================================================= */

#endif /* SOCKETUTILITY_H_ */