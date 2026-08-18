/*
 * SocketUtility.c
 *
 *  Created on: Aug 23, 2024
 *      Author: Hyeongwon Jeon
 */

#include "SocketUtility.h"

/* ======================================================================================== */
/*                                   Internal Function                                      */
/* ======================================================================================== */
static SOCKET_INT64 f_SocketRecvUDP_IPv4_Address(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lMaxSize);

/* ======================================================================================== */
/*                                       Global Value                                       */
/* ======================================================================================== */

/* ======================================================================================== */
/*                                       Static Value                                       */
/* ======================================================================================== */

/* ======================================================================================== */
static pthread_mutex_t sgMutex_sgiUDP_PartitionSize_Byte;
static pthread_mutex_t sgMutex_sguiUDP_PartitionSendGap_us;

// UDP Partition Size(Byte) for big size Data(Over 65507 Byte) Transmission
// UDP Max MTU = 65535 : IP Header 20 + UDP Header 8 + Packet 65507
// Ethernet Protocol support up to 1500, but that is coverage of the physical layer
// Default Value : 16384
// Minimum Value : 1
// Maximum Value : 65507
static SOCKET_INT32 sgiUDP_PartitionSize_Byte = 65507;

// UDP Partition Send Gap Time(us)
static SOCKET_UINT32 sguiUDP_PartitionSendGap_us = (SOCKET_UINT32)100;

static SOCKET_INT64 f_SocketRecvUDP_IPv4_Address(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lMaxSize)
{
	/*
	 * Receive Data via UDP(With address update)
	 * Input
	 * - stpSocket : Pointer to Socket structure
	 * - vpDataAddr : Address to Receive
	 * - lMaxSize : Max Data size to Receive
	 * Output
	 * - lReceiveSize : Receive Size
	 */

	SOCKET_INT64 lReceiveSize;

	SOCKET_UCHAR8 *ucpAddr;
	ucpAddr = vpDataAddr;

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lMaxSize > 65507)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[WARNING] %ld Byte Data cannot cover via UDP(Address)\n", lMaxSize);
	}
#endif

	// Receive Data
	// Returns the number of bytes read or -1 for errors
	lReceiveSize = recvfrom(stpSocket->iSockId, ucpAddr, (SOCKET_UINT32)lMaxSize, 0, &(stpSocket->stSockAddr), &(stpSocket->uiSockAddrSize));
#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lReceiveSize == (SOCKET_INT64)-1)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Receive UDP(Address)\n");
	}
#endif

#if(DISP_SOCKET_RESULT > 1)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Receive UDP(Address) IPv4[from %s:%u] : %ld / %ld Byte\n", \
			inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), \
			lReceiveSize, lMaxSize);
#endif

	return lReceiveSize;
}

SOCKET_VOID f_SocketGetVerInfo(st_VerInfo_SOCKET *stpVerInfo)
{
	/*
	 * Get Version Information include Build date
	 * Input
	 * - stpVerInfo : Pointer to Version Information structure
	 * Output
	 * - VOID
	 */

	// Assign MajorVersion
	stpVerInfo->iMajorVersion = SOCKETUTILITY_VER_MAJOR;

	// Assign MinorVersion
	stpVerInfo->iMinorVersion = SOCKETUTILITY_VER_MINOR;

	// Assign BuildData
	stpVerInfo->iBuildDate = SOCKETUTILITY_BUILD_DATE;

	//return NULL;
}

SOCKET_INT32 f_SocketSetUDP_PartitionSize(const SOCKET_INT32 iUDP_PartitionSize_Byte)
{
	/*
	 * Set UDP Partition Size(Byte) for big size Data(Over 65507 Byte) Transmission
	 * UDP Max MTU = 65535 : IP Header 20 + UDP Header 8 + Packet 65507
	 * Ethernet Protocol support up to 1500, but that is coverage of the physical layer
	 * Input
	 * - iUDP_PartitionSize_Byte : UDP Partition Size(Byte)
	 *   Minimum Value : 1
	 *   Maximum Value : 65507
	 * Output
	 * - 0 : Fail
	 * - 1 : Pass
	 */

	SOCKET_INT32 iResult;

	// Due to Code Coverage, Set Result value as FAIL and then update as PASS if no problem during the work
	iResult = SOCKET_FAIL;
	if( (iUDP_PartitionSize_Byte >= 1) && (iUDP_PartitionSize_Byte <= 65507) )
	{
		// Mutex Lock
		(SOCKET_VOID) pthread_mutex_lock(&sgMutex_sgiUDP_PartitionSize_Byte);

		// Set Global Value
		sgiUDP_PartitionSize_Byte = iUDP_PartitionSize_Byte;

		// Mutex UnLock
		(SOCKET_VOID) pthread_mutex_unlock(&sgMutex_sgiUDP_PartitionSize_Byte);

		iResult = SOCKET_PASS;
	}

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(iResult == SOCKET_FAIL)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] UDP Packet Size must be larger or equal than 1 and smaller or equal than 65507\n");
	}
#endif

	return iResult;
}

SOCKET_VOID f_SocketGetUDP_PartitionSize(SOCKET_INT32 *ipUDP_PartitionSize_Byte)
{
	/*
	 * Get UDP Partition Size(Byte) for big size Data(Over 65507 Byte) Transmission
	 * Input
	 * - ipUDP_PartitionSize_Byte : Pointer to UDP Partition Size(Byte)
	 * Output
	 * - VOID
	 */

	// Mutex Lock
	(SOCKET_VOID) pthread_mutex_lock(&sgMutex_sgiUDP_PartitionSize_Byte);

	// Get Global Value
	*ipUDP_PartitionSize_Byte = sgiUDP_PartitionSize_Byte;

	// Mutex UnLock
	(SOCKET_VOID) pthread_mutex_unlock(&sgMutex_sgiUDP_PartitionSize_Byte);

	//return NULL;
}

SOCKET_INT32 f_SocketSetUDP_PartitionSendGap(const SOCKET_UINT32 uiUDP_PartitionSendGap_us)
{
	/*
	 * Set UDP Partition Send Gap(us)
	 * - uiUDP_PartitionSendGap_us : UDP Partition Send Gap(us)
	 *   Minimum Value : 1
	 * Output
	 * - 0 : Fail
	 * - 1 : Pass
	 */

	SOCKET_INT32 iResult;

	// Due to Code Coverage, Set Result value as FAIL and then update as PASS if no problem during the work
	iResult = SOCKET_FAIL;
	if(uiUDP_PartitionSendGap_us >= (SOCKET_UINT32)1)
	{
		// Mutex Lock
		(SOCKET_VOID) pthread_mutex_lock(&sgMutex_sguiUDP_PartitionSendGap_us);

		// Set Global Value
		sguiUDP_PartitionSendGap_us = uiUDP_PartitionSendGap_us;

		// Mutex UnLock
		(SOCKET_VOID) pthread_mutex_unlock(&sgMutex_sguiUDP_PartitionSendGap_us);

		iResult = SOCKET_PASS;
	}

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(iResult == SOCKET_FAIL)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] UDP Packet Send Gap must be larger or equal than 1\n");
	}
#endif

	return iResult;
}

SOCKET_VOID f_SocketGetUDP_PartitionSendGap(SOCKET_UINT32 *uipUDP_PartitionSendGap_us)
{
	/*
	 * Get UDP Partition Send Gap(us)
	 * Input
	 * - uipUDP_PartitionSendGap_us : Pointer to UDP Partition Send Gap(us)
	 * Output
	 * - VOID
	 */

	// Mutex Lock
	(SOCKET_VOID) pthread_mutex_lock(&sgMutex_sguiUDP_PartitionSendGap_us);

	// Get Global Value
	*uipUDP_PartitionSendGap_us = sguiUDP_PartitionSendGap_us;

	// Mutex UnLock
	(SOCKET_VOID) pthread_mutex_unlock(&sgMutex_sguiUDP_PartitionSendGap_us);

	//return NULL;
}

st_Socket f_SocketInitUDP_IPv4Tx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum)
{
	/*
	 * Initialize UDP Socket for Tx
	 * Step1 : Define Socket
	 * Step2 : Set Receiver Address(Include Port Number)
	 * Input
	 * - cpIpAddr : IPv4 Address(Destination IP)
	 *   NULL : Self IP
	 *   Others : User Defined IP
	 * - usPortNum : Port Number
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined
	 *   stSocket.iSockType : enum_Socket_Type_UDP_IPv4Tx
	 *   stSocket.iSockId : Fail(-1) or Pass(Others)
	 */

	st_Socket stSocket;

	// Step1 : Define Socket
	/*
	 * Socket
	 * - PF_INET : IPv4, PF_INET6 : IPv6, PF_LOCAL : UNIX Protocol for LOCAL Communication, PF_PACKET : Low Level Socket Interface
	 * - SOCK_STREAM : TCP, SOCK_DGRAM : UDP, SOCK_RAW : User Defined
	 * - IPPROTO_TCP : TCP, IPPROTO_UDP : UDP, 0 : Follow SOCK_STREAM setting
	 * - Return : Success ID, Fail -1
	 */
	stSocket.iSockId = socket(PF_INET, (SOCKET_INT32)SOCK_DGRAM, (SOCKET_INT32)IPPROTO_UDP);
	stSocket.iSockStatus = enum_Socket_Status_NotDefined;
	stSocket.iSockType = enum_Socket_Type_UDP_IPv4Tx;
	stSocket.uiSockAddrSize = sizeof(struct sockaddr_in);

	// Step2 : Set Receiver Address(Include Port Number)
	/*
	 * Socket Address Setting
	 * - sin_family : AF_INET - IPv4
	 * - sin_port : htons(0000)- Port Number Set(Big Endian Short)
	 * - s_addr : htonl(INADDR_ANY) - Self(Own) IP Address Set(Big Endian Long)
	 *            inet_addr("000.000.000.000") - User Defined IP Address Set
	 */
	(SOCKET_VOID) memset(&stSocket.stSockAddr, 0, stSocket.uiSockAddrSize);
	stSocket.stSockAddr.sin_family = AF_INET;
	stSocket.stSockAddr.sin_port = htons(usPortNum);
	if(cpIpAddr != NULL)
	{
		stSocket.stSockAddr.sin_addr.s_addr = inet_addr(cpIpAddr);
	}
	else
	{
		stSocket.stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial UDP IPv4 Tx Socket[to %s:%u] --> ID %d, Status %d, Type %d\n", \
			cpIpAddr, usPortNum, stSocket.iSockId, stSocket.iSockStatus, stSocket.iSockType);
#endif

	return stSocket;
}

st_Socket f_SocketInitUDP_IPv4Rx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us)
{
	/*
	 * Initialize UDP Socket for Rx
	 * Step1 : Define Socket
	 * Step2 : Set TimeOut(if 0, Blocking on recvfrom)
	 * Step3 : Set Receiver Address(Include Port Number)
	 * Step4 : Bind
	 * Input
	 * - cpIpAddr : IPv4 Address(Destination IP)
	 *   NULL : Self IP
	 *   Others : User Defined IP
	 * - usPortNum : Port Number
	 * - lTimeOut_s : Socket Receive TimeOut(sec)
	 * - lTimeOut_us : Socket Receive TimeOut(usec)
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined
	 *   stSocket.iSockType : enum_Socket_Type_UDP_IPv4Rx
	 *   stSocket.iSockId : Fail(-1) or Pass(Others)
	 */

	st_Socket stSocket;

	SOCKET_INT32 iTemp;

	// Step1 : Define Socket
	/*
	 * Socket
	 * - PF_INET : IPv4, PF_INET6 : IPv6, PF_LOCAL : UNIX Protocol for LOCAL Communication, PF_PACKET : Low Level Socket Interface
	 * - SOCK_STREAM : TCP, SOCK_DGRAM : UDP, SOCK_RAW : User Defined
	 * - IPPROTO_TCP : TCP, IPPROTO_UDP : UDP, 0 : Follow SOCK_STREAM setting
	 * - Return : Success ID, Fail -1
	 */
	stSocket.iSockId = socket(PF_INET, (SOCKET_INT32)SOCK_DGRAM, (SOCKET_INT32)IPPROTO_UDP);
	stSocket.iSockStatus = enum_Socket_Status_NotDefined;
	stSocket.iSockType = enum_Socket_Type_UDP_IPv4Rx;
	stSocket.uiSockAddrSize = sizeof(struct sockaddr_in);
	stSocket.stTimeOutVal.tv_sec = lTimeOut_s;
	stSocket.stTimeOutVal.tv_usec = lTimeOut_us;

	// Step2 : Set TimeOut(if 0, Blocking on recvfrom)
	/*
	 * SetSockOpt
	 * - SOL_SOCKET : Level
	 * - SO_SNDTIMEO(sendto TimeOut), SO_RCVTIMEO(recvfrom TimeOut)
	 * - timeval Address
	 * - timeval Length
	 */
	if( (stSocket.stTimeOutVal.tv_sec > (SOCKET_INT64)0) || (stSocket.stTimeOutVal.tv_usec > (SOCKET_INT64)0) )
	{
		(SOCKET_VOID) setsockopt(stSocket.iSockId, SOL_SOCKET, SO_RCVTIMEO, &stSocket.stTimeOutVal, sizeof(stSocket.stTimeOutVal));
	}

	// Step3 : Set Receiver Address(Include Port Number)
	/*
	 * Socket Address Setting
	 * - sin_family : AF_INET - IPv4
	 * - sin_port : htons(0000)- Port Number Set(Big Endian Short)
	 * - s_addr : htonl(INADDR_ANY) - Self(Own) IP Address Set(Big Endian Long)
	 *            inet_addr("000.000.000.000") - User Defined IP Address Set
	 */
	(SOCKET_VOID) memset(&stSocket.stSockAddr, 0, stSocket.uiSockAddrSize);
	stSocket.stSockAddr.sin_family = AF_INET;
	stSocket.stSockAddr.sin_port = htons(usPortNum);
	if(cpIpAddr != NULL)
	{
		stSocket.stSockAddr.sin_addr.s_addr = inet_addr(cpIpAddr);
	}
	else
	{
		stSocket.stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	// Step4 : Bind
	/*
	 * bind
	 * - iSockId : SocketID(Return ID of socket())
	 * - &stSockAddr : Socket Address for Server(AF_INET : struct sockaddr_in, AF_UNIX : struct sockaddr, Same size)
	 * - uiSockAddrSize : Socket Address Structure Size
	 * - Return : Success 0, Fail -1
	 */
	iTemp = bind(stSocket.iSockId, &stSocket.stSockAddr, stSocket.uiSockAddrSize);
	if(iTemp == -1)
	{
#if(DISP_SOCKET_ERROR_WARNING == 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Fail UDP Rx Bind(%s - %u)\n", inet_ntoa(stSocket.stSockAddr.sin_addr), ntohl(stSocket.stSockAddr.sin_port));
#endif

		(SOCKET_VOID) close(stSocket.iSockId);
		stSocket.iSockId = -1;
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial UDP IPv4 Rx Socket[at %s:%u, recv Timeout %ld sec %ld usec] --> ID %d, Status %d, Type %d\n", \
			cpIpAddr, usPortNum, stSocket.stTimeOutVal.tv_sec, stSocket.stTimeOutVal.tv_usec, stSocket.iSockId, stSocket.iSockStatus, stSocket.iSockType);
#endif

	return stSocket;
}

SOCKET_INT64 f_SocketSendUDP_IPv4_Normal(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lDataSize)
{
	/*
	 * Send Data via UDP(Without Data Partition)
	 * Input
	 * - stpSocket : Pointer to Socket structure
	 * - vpDataAddr : Address to Send
	 * - lDataSize : Data size to Send
	 * Output
	 * - lSendSize : Send Size
	 */

	SOCKET_INT64 lSendSize;

	SOCKET_UCHAR8 *ucpAddr;
	ucpAddr = vpDataAddr;

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lDataSize > 65507)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[WARNING] %ld Byte Data cannot send via UDP(Normal)\n", lDataSize);
	}
#endif

	// Send Data
	// Returns the number sent, or -1 for errors
	lSendSize = sendto(stpSocket->iSockId, ucpAddr, (SOCKET_UINT32)lDataSize, 0, &(stpSocket->stSockAddr), stpSocket->uiSockAddrSize);
#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lSendSize == (SOCKET_INT64)-1)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Send UDP(Normal)\n");
	}

	if(lSendSize != lDataSize)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[WARNING] Send UDP(Normal) %ld / %ld Byte only\n", lSendSize, lDataSize);
	}
#endif

#if(DISP_SOCKET_RESULT > 1)
	if(lSendSize != (SOCKET_INT64)-1)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send UDP(Normal) IPv4[to %s:%u] : %ld / %ld Byte\n", \
				inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), \
				lSendSize, lDataSize);
	}
#endif

	return lSendSize;
}

SOCKET_INT64 f_SocketSendUDP_IPv4_Partition(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize)
{
	/*
	 * Send Data via UDP(With Data Partition)
	 * Input
	 * - stpSocket : Pointer to Socket structure
	 * - vpDataAddr : Address to Send
	 * - lFixedDataSize : Fixed Data size to Send
	 * Output
	 * - lSendSize : Send Size
	 */

	SOCKET_INT64 lSendSize;

	SOCKET_INT32 iUDP_PartitionSize_Byte;
	SOCKET_UINT32 uiUDP_PartitionSendGap_us;

	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_INT64 lRemainSize, lTempSize;

	// Mutex Lock
	(SOCKET_VOID) pthread_mutex_lock(&sgMutex_sgiUDP_PartitionSize_Byte);

	// Get Global Value
	iUDP_PartitionSize_Byte = sgiUDP_PartitionSize_Byte;

	// Mutex UnLock
	(SOCKET_VOID) pthread_mutex_unlock(&sgMutex_sgiUDP_PartitionSize_Byte);

	// Mutex Lock
	(SOCKET_VOID) pthread_mutex_lock(&sgMutex_sguiUDP_PartitionSendGap_us);

	// Get Global Value
	uiUDP_PartitionSendGap_us = sguiUDP_PartitionSendGap_us;

	// Mutex UnLock
	(SOCKET_VOID) pthread_mutex_unlock(&sgMutex_sguiUDP_PartitionSendGap_us);

	ucpAddr = vpDataAddr;

	lSendSize = (SOCKET_INT64)0;
	lRemainSize = lFixedDataSize - lSendSize;

	while(lRemainSize > (SOCKET_INT64)0)
	{
		if(iUDP_PartitionSize_Byte <= lRemainSize)
		{
			// Send Data
			// Returns the number sent, or -1 for errors
			lTempSize = sendto(stpSocket->iSockId, &(ucpAddr[lSendSize]), (SOCKET_UINT32)iUDP_PartitionSize_Byte, 0, &(stpSocket->stSockAddr), stpSocket->uiSockAddrSize);
		}
		else
		{
			// Send Data
			// Returns the number sent, or -1 for errors
			lTempSize = sendto(stpSocket->iSockId, &(ucpAddr[lSendSize]), (SOCKET_UINT32)lRemainSize, 0, &(stpSocket->stSockAddr), stpSocket->uiSockAddrSize);
		}

		if(lTempSize != (SOCKET_INT64)-1)
		{
			lSendSize = lSendSize + lTempSize;
			lRemainSize = lFixedDataSize - lSendSize;

			// Partition Send Gap(us)
			(SOCKET_VOID) usleep(uiUDP_PartitionSendGap_us);

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send UDP(Partition) IPv4[to %s:%u] : %ld / %ld Byte(Remained %ld Byte)\n", \
					inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), \
					lSendSize, lFixedDataSize, lRemainSize);
#endif
		}
		else
		{
#if(DISP_SOCKET_ERROR_WARNING == 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Send UDP(Partition)\n");
#endif

			break;
		}
	}

	return lSendSize;
}

SOCKET_INT64 f_SocketRecvUDP_IPv4_Normal(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lMaxSize)
{
	/*
	 * Receive Data via UDP(Without Data Partition)
	 * Input
	 * - stpSocket : Pointer to Socket structure
	 * - vpDataAddr : Address to Receive
	 * - lMaxSize : Max Data size to Receive
	 * Output
	 * - lReceiveSize : Receive Size
	 */

	SOCKET_INT64 lReceiveSize;

	st_Socket stSocket_Trash;
	SOCKET_UCHAR8 *ucpAddr;
	ucpAddr = vpDataAddr;

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lMaxSize > 65507)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[WARNING] %ld Byte Data cannot cover via UDP(Normal)\n", lMaxSize);
	}
#endif

	// Receive Data
	// Returns the number of bytes read or -1 for errors
	// To prevent Address Update, Use stSocket_Trash
	lReceiveSize = recvfrom(stpSocket->iSockId, ucpAddr, (SOCKET_UINT32)lMaxSize, 0, &(stSocket_Trash.stSockAddr), &(stSocket_Trash.uiSockAddrSize));
#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lReceiveSize == (SOCKET_INT64)-1)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Receive UDP(Normal)\n");
	}
#endif

#if(DISP_SOCKET_RESULT > 1)
	if(lReceiveSize != (SOCKET_INT64)-1)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Receive UDP(Normal) IPv4[at %s:%u] : %ld / %ld Byte\n", \
				inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), \
				lReceiveSize, lMaxSize);
	}
#endif

	return lReceiveSize;
}

SOCKET_INT64 f_SocketRecvUDP_IPv4_Partition(const st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize)
{
	/*
	 * Receive Data via UDP(With Data Partition)
	 * Input
	 * - stpSocket : Pointer to Socket structure
	 * - vpDataAddr : Address to Receive
	 * - lFixedDataSize : Fixed Data size to Receive
	 * Output
	 * - lReceiveSize : Receive Size
	 */

	SOCKET_INT64 lReceiveSize;

	SOCKET_INT32 iUDP_PartitionSize_Byte;

	st_Socket stSocket_Trash;
	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_INT64 lRemainSize, lTempSize;

	// Mutex Lock
	(SOCKET_VOID) pthread_mutex_lock(&sgMutex_sgiUDP_PartitionSize_Byte);

	// Get Global Value
	iUDP_PartitionSize_Byte = sgiUDP_PartitionSize_Byte;

	// Mutex UnLock
	(SOCKET_VOID) pthread_mutex_unlock(&sgMutex_sgiUDP_PartitionSize_Byte);

	ucpAddr = vpDataAddr;

	lReceiveSize = (SOCKET_INT64)0;
	lRemainSize = lFixedDataSize - lReceiveSize;

	while(lRemainSize > (SOCKET_INT64)0)
	{
		if(iUDP_PartitionSize_Byte <= lRemainSize)
		{
			// Receive Data
			// Returns the number of bytes read or -1 for errors
			// To prevent Address Update, Use stSocket_Trash
			lTempSize = recvfrom(stpSocket->iSockId, &(ucpAddr[lReceiveSize]), (SOCKET_UINT32)iUDP_PartitionSize_Byte, 0, &(stSocket_Trash.stSockAddr), &(stSocket_Trash.uiSockAddrSize));
		}
		else
		{
			// Receive Data
			// Returns the number of bytes read or -1 for errors
			// To prevent Address Update, Use stSocket_Trash
			lTempSize = recvfrom(stpSocket->iSockId, &(ucpAddr[lReceiveSize]), (SOCKET_UINT32)lRemainSize, 0, &(stSocket_Trash.stSockAddr), &(stSocket_Trash.uiSockAddrSize));
		}

		if(lTempSize != (SOCKET_INT64)-1)
		{
			lReceiveSize = lReceiveSize + lTempSize;
			lRemainSize = lFixedDataSize - lReceiveSize;

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Receive UDP(Partition) IPv4[at %s:%u] : %ld / %ld Byte(Remained %ld Byte)\n", \
					inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), \
					lReceiveSize, lFixedDataSize, lRemainSize);
#endif
		}
		else
		{
#if(DISP_SOCKET_ERROR_WARNING == 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Receive UDP(Partition)\n");
#endif

			break;
		}
	}

	return lReceiveSize;
}

st_Socket f_SocketInitTCP_IPv4Tx_Normal(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us)
{
	/*
	 * Initialize TCP Socket for Tx(Without Sync)
	 * Step1 : Define Socket
	 * Step2 : Set TimeOut(if 0, Blocking on send when Buffer Full)
	 * Step3 : Set Receiver Address(Include Port Number)
	 * Step4 : Connect
	 * Step5 : When Connect Fail, Retry(Optional)
	 * Input
	 * - cpIpAddr : IPv4 Address(Destination IP)
	 *   NULL : Self IP
	 *   Others : User Defined IP
	 * - usPortNum : Port Number
	 * - lTimeOut_s : Socket Send TimeOut(sec)
	 * - lTimeOut_us : Socket Send TimeOut(usec)
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined, enum_Socket_Status_Connected(Connect Success)
	 *   stSocket.iSockType : enum_Socket_Type_TCP_IPv4Tx
	 *   stSocket.iSockId : Fail(-1) or Pass(Others)
	 */

	st_Socket stSocket_TCP;

	SOCKET_INT32 iTemp;

	// Step1 : Define Socket
	/*
	 * Socket
	 * - PF_INET : IPv4, PF_INET6 : IPv6, PF_LOCAL : UNIX Protocol for LOCAL Communication, PF_PACKET : Low Level Socket Interface
	 * - SOCK_STREAM : TCP, SOCK_DGRAM : UDP, SOCK_RAW : User Defined
	 * - IPPROTO_TCP : TCP, IPPROTO_UDP : UDP, 0 : Follow SOCK_STREAM setting
	 * - Return : Success ID, Fail -1
	 */
	stSocket_TCP.iSockId = socket(PF_INET, (SOCKET_INT32)SOCK_STREAM, (SOCKET_INT32)IPPROTO_TCP);
	stSocket_TCP.iSockStatus = enum_Socket_Status_NotDefined;
	stSocket_TCP.iSockType = enum_Socket_Type_TCP_IPv4Tx;
	stSocket_TCP.uiSockAddrSize = sizeof(struct sockaddr_in);
	stSocket_TCP.stTimeOutVal.tv_sec = lTimeOut_s;
	stSocket_TCP.stTimeOutVal.tv_usec = lTimeOut_us;

	// Step2 : Set TimeOut(if 0, Blocking on send when Buffer Full)
	/*
	 * SetSockOpt
	 * - SOL_SOCKET : Level
	 * - SO_SNDTIMEO(sendto TimeOut), SO_RCVTIMEO(recvfrom TimeOut)
	 * - timeval Address
	 * - timeval Length
	 */
	if( (stSocket_TCP.stTimeOutVal.tv_sec > (SOCKET_INT64)0) || (stSocket_TCP.stTimeOutVal.tv_usec > (SOCKET_INT64)0) )
	{
		(SOCKET_VOID) setsockopt(stSocket_TCP.iSockId, SOL_SOCKET, SO_SNDTIMEO, &stSocket_TCP.stTimeOutVal, sizeof(stSocket_TCP.stTimeOutVal));
	}

	// Step3 : Set Receiver Address(Include Port Number)
	/*
	 * Socket Address Setting
	 * - sin_family : AF_INET - IPv4
	 * - sin_port : htons(0000)- Port Number Set(Big Endian Short)
	 * - s_addr : htonl(INADDR_ANY) - Self(Own) IP Address Set(Big Endian Long)
	 *            inet_addr("000.000.000.000") - User Defined IP Address Set
	 */
	(SOCKET_VOID) memset(&stSocket_TCP.stSockAddr, 0, stSocket_TCP.uiSockAddrSize);
	stSocket_TCP.stSockAddr.sin_family = AF_INET;
	stSocket_TCP.stSockAddr.sin_port = htons(usPortNum);
	if(cpIpAddr != NULL)
	{
		stSocket_TCP.stSockAddr.sin_addr.s_addr = inet_addr(cpIpAddr);
	}
	else
	{
		stSocket_TCP.stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	// Step4 : Connect
	// Return : Success 0, Fail -1
	iTemp = connect(stSocket_TCP.iSockId, &stSocket_TCP.stSockAddr, stSocket_TCP.uiSockAddrSize);
	if(iTemp == 0)
	{
		stSocket_TCP.iSockStatus = enum_Socket_Status_Connected;

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Tx Socket : Connect OK\n");
#endif
	}
#if(DISP_SOCKET_ERROR_WARNING == 1)
	else
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Tx Socket : Connect ERROR\n");
	}
#endif

#if(CTRL_SOCKET_REPEAT_CONNECT == 1)
	// Step5 : When Connect Fail, Retry(Optional)
	while(stSocket_TCP.iSockStatus != enum_Socket_Status_Connected)
	{
		// Return : Success 0, Fail -1
		iTemp = connect(stSocket_TCP.iSockId, &stSocket_TCP.stSockAddr, stSocket_TCP.uiSockAddrSize);
		if(iTemp == 0)
		{
			stSocket_TCP.iSockStatus = enum_Socket_Status_Connected;

#if(DISP_SOCKET_RESULT > 0)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Tx Socket : Connect OK\n");
#endif
		}
		else
		{
#if(DISP_SOCKET_ERROR_WARNING == 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Retry Connect...\n");
#endif

			(SOCKET_VOID) usleep((SOCKET_UINT32)500 * (SOCKET_UINT32)1000);	// 500ms
		}
	}
#endif

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP IPv4 Tx Socket[listen %s:%u, send Timeout %ld sec %ld usec] --> ID %d, Status %d, Type %d\n", \
			cpIpAddr, usPortNum, stSocket_TCP.stTimeOutVal.tv_sec, stSocket_TCP.stTimeOutVal.tv_usec, stSocket_TCP.iSockId, stSocket_TCP.iSockStatus, stSocket_TCP.iSockType);
#endif

	return stSocket_TCP;
}

st_Socket f_SocketInitTCP_IPv4Tx_Sync(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us)
{
	/*
	 * Initialize TCP Socket for Tx(With Sync)
	 * Step1 : Define UDP Socket and Set Receiver Address(Include Port Number)
	 * Step2 : Send 'SOCKET_SYNC_PASS_TX_LINE_CHECK' until receive 'SOCKET_SYNC_PASS_RX_LINE_CHECK'
	 * Step3 : Wait 'SOCKET_SYNC_PASS_LISTEN' via UDP Socket
	 * Step4 : Try Connect and Send 'SOCKET_SYNC_PASS_CONNECT' via UDP Socket
	 * Step5 : Wait 'SOCKET_SYNC_PASS_SEND_TO_TX' and send 'SOCKET_SYNC_PASS_SEND_TO_RX' via TCP Socket
	 * Step6 : Close UDP Socket
	 * Input
	 * - cpIpAddr : IPv4 Address(Destination IP)
	 *   NULL : Self IP
	 *   Others : User Defined IP
	 * - usPortNum : Port Number
	 * - lTimeOut_s : Socket Send TimeOut(sec)
	 * - lTimeOut_us : Socket Send TimeOut(usec)
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined, enum_Socket_Status_Connected(Connect Success)
	 *   stSocket.iSockType : enum_Socket_Type_TCP_IPv4Tx
	 *   stSocket.iSockId : Fail(-1) or Pass(Others)
	 */

	st_Socket stSocket_TCP;

	st_Socket stSocket_UDP;
	SOCKET_INT32 iTemp, iTxRxData;
	SOCKET_INT64 lRunStep_s, lRunStep_us;

	// 500ms
	lRunStep_s = 0;
	lRunStep_us = 500 * 1000;

	struct timeval stRunStepVal;
	stRunStepVal.tv_sec = lRunStep_s;
	stRunStepVal.tv_usec = lRunStep_us;

	// Step1 : Define UDP Socket and Set Receiver Address(Include Port Number)
	// UDP Socket TimeOut = stRunStepVal
	stSocket_UDP = f_SocketInitUDP_IPv4Tx(cpIpAddr, usPortNum);
	if( (stRunStepVal.tv_sec > (SOCKET_INT64)0) || (stRunStepVal.tv_usec > (SOCKET_INT64)0) )
	{
		(SOCKET_VOID) setsockopt(stSocket_UDP.iSockId, SOL_SOCKET, SO_RCVTIMEO, &stRunStepVal, sizeof(stRunStepVal));
	}

	// Step2 : Send 'SOCKET_SYNC_PASS_TX_LINE_CHECK' until receive 'SOCKET_SYNC_PASS_RX_LINE_CHECK'
	iTxRxData = SOCKET_SYNC_PASS_TX_LINE_CHECK;
	while(iTxRxData != SOCKET_SYNC_PASS_RX_LINE_CHECK)
	{
		(SOCKET_VOID) f_SocketSendUDP_IPv4_Normal(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));
		(SOCKET_VOID) f_SocketRecvUDP_IPv4_Normal(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Tx Socket : TX Line Check\n");
#endif
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Tx Socket : RX Line Check\n");
#endif

	// Step3 : Wait 'SOCKET_SYNC_PASS_LISTEN' via UDP Socket
	while(iTxRxData != SOCKET_SYNC_PASS_LISTEN)
	{
		(SOCKET_VOID) f_SocketRecvUDP_IPv4_Normal(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Tx Socket : Listen Start\n");
#endif

	// Step4 : Try Connect and Send 'SOCKET_SYNC_PASS_CONNECT' via UDP Socket
	/*
	 * Socket
	 * - PF_INET : IPv4, PF_INET6 : IPv6, PF_LOCAL : UNIX Protocol for LOCAL Communication, PF_PACKET : Low Level Socket Interface
	 * - SOCK_STREAM : TCP, SOCK_DGRAM : UDP, SOCK_RAW : User Defined
	 * - IPPROTO_TCP : TCP, IPPROTO_UDP : UDP, 0 : Follow SOCK_STREAM setting
	 * - Return : Success ID, Fail -1
	 */
	stSocket_TCP.iSockId = socket(PF_INET, (SOCKET_INT32)SOCK_STREAM, (SOCKET_INT32)IPPROTO_TCP);
	stSocket_TCP.iSockStatus = enum_Socket_Status_NotDefined;
	stSocket_TCP.iSockType = enum_Socket_Type_TCP_IPv4Tx;
	stSocket_TCP.uiSockAddrSize = sizeof(struct sockaddr_in);
	stSocket_TCP.stTimeOutVal.tv_sec = lTimeOut_s;
	stSocket_TCP.stTimeOutVal.tv_usec = lTimeOut_us;

	/*
	 * SetSockOpt
	 * - SOL_SOCKET : Level
	 * - SO_SNDTIMEO(sendto TimeOut), SO_RCVTIMEO(recvfrom TimeOut)
	 * - timeval Address
	 * - timeval Length
	 */
	if( (stSocket_TCP.stTimeOutVal.tv_sec > (SOCKET_INT64)0) || (stSocket_TCP.stTimeOutVal.tv_usec > (SOCKET_INT64)0) )
	{
		(SOCKET_VOID) setsockopt(stSocket_TCP.iSockId, SOL_SOCKET, SO_SNDTIMEO, &stSocket_TCP.stTimeOutVal, sizeof(stSocket_TCP.stTimeOutVal));
	}

	/*
	 * Socket Address Setting
	 * - sin_family : AF_INET - IPv4
	 * - sin_port : htons(0000)- Port Number Set(Big Endian Short)
	 * - s_addr : htonl(INADDR_ANY) - Self(Own) IP Address Set(Big Endian Long)
	 *            inet_addr("000.000.000.000") - User Defined IP Address Set
	 */
	(SOCKET_VOID) memset(&stSocket_TCP.stSockAddr, 0, stSocket_TCP.uiSockAddrSize);
	stSocket_TCP.stSockAddr.sin_family = AF_INET;
	stSocket_TCP.stSockAddr.sin_port = htons(usPortNum);
	if(cpIpAddr != NULL)
	{
		stSocket_TCP.stSockAddr.sin_addr.s_addr = inet_addr(cpIpAddr);
	}
	else
	{
		stSocket_TCP.stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	iTxRxData = SOCKET_SYNC_PASS_CONNECT;

	// Return : Success 0, Fail -1
	iTemp = connect(stSocket_TCP.iSockId, &stSocket_TCP.stSockAddr, stSocket_TCP.uiSockAddrSize);
	if(iTemp == 0)
	{
		stSocket_TCP.iSockStatus = enum_Socket_Status_Connected;

		(SOCKET_VOID) f_SocketSendUDP_IPv4_Normal(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Tx Socket : Connect OK\n");
#endif
	}
#if(DISP_SOCKET_ERROR_WARNING == 1)
	else
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Tx Socket : Connect ERROR\n");
	}
#endif

	// Step5 : Wait 'SOCKET_SYNC_PASS_SEND_TO_TX' and send 'SOCKET_SYNC_PASS_SEND_TO_RX' via TCP Socket
	while(iTxRxData != SOCKET_SYNC_PASS_SEND_TO_TX)
	{
		(SOCKET_VOID) f_SocketRecvTCP_IPv4(&stSocket_TCP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Tx Socket : RX Data OK\n");
#endif

	iTxRxData = SOCKET_SYNC_PASS_SEND_TO_RX;
	(SOCKET_VOID) f_SocketSendTCP_IPv4(&stSocket_TCP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Tx Socket : TX Data OK\n");
#endif

	// Step6 : Close UDP Socket
	(SOCKET_VOID) f_SocketClose(stSocket_UDP);

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP IPv4 Tx Socket[listen %s:%u, send Timeout %ld sec %ld usec] --> ID %d, Status %d, Type %d\n", \
			cpIpAddr, usPortNum, stSocket_TCP.stTimeOutVal.tv_sec, stSocket_TCP.stTimeOutVal.tv_usec, stSocket_TCP.iSockId, stSocket_TCP.iSockStatus, stSocket_TCP.iSockType);
#endif

	return stSocket_TCP;
}

st_Socket f_SocketInitTCP_IPv4Rx_Normal(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us)
{
	/*
	 * Initialize TCP Socket for Rx(Without Sync)
	 * Step1 : Define Socket(For Listen)
	 * Step2 : Set Receiver Address(Include Port Number)
	 * Step3 : Bind
	 * Step4 : Listen
	 * Step5 : Accept
	 * Step6 : Copy Listen Address
	 * Step7 : Set TimeOut(if 0, Blocking on recv)
	 * Step8 : Close TCP Temp Socket
	 * Input
	 * - cpIpAddr : IPv4 Address(Destination IP)
	 *   NULL : Self IP
	 *   Others : User Defined IP
	 * - usPortNum : Port Number
	 * - lTimeOut_s : Socket Receive TimeOut(sec)
	 * - lTimeOut_us : Socket Receive TimeOut(usec)
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined, enum_Socket_Status_Connected(Accept Success)
	 *   stSocket.iSockType : enum_Socket_Type_TCP_IPv4Rx
	 *   stSocket.iSockId : Fail(-1) or Pass(Others)
	 */

	st_Socket stSocket_TCP;

	st_Socket stSocket_Temp;
	SOCKET_INT32 iReuseAddrOption;

#if((DISP_SOCKET_RESULT > 0) || (DISP_SOCKET_ERROR_WARNING == 1))
	SOCKET_INT32 iTemp;
#endif

	// Step1 : Define Socket(For Listen)
	/*
	 * Socket
	 * - PF_INET : IPv4, PF_INET6 : IPv6, PF_LOCAL : UNIX Protocol for LOCAL Communication, PF_PACKET : Low Level Socket Interface
	 * - SOCK_STREAM : TCP, SOCK_DGRAM : UDP, SOCK_RAW : User Defined
	 * - IPPROTO_TCP : TCP, IPPROTO_UDP : UDP, 0 : Follow SOCK_STREAM setting
	 * - Return : Success ID, Fail -1
	 */
	stSocket_Temp.iSockId = socket(PF_INET, (SOCKET_INT32)SOCK_STREAM, (SOCKET_INT32)IPPROTO_TCP);
	stSocket_Temp.iSockType = enum_Socket_Type_NotDefined;
	stSocket_Temp.uiSockAddrSize = sizeof(struct sockaddr_in);

	// Step2 : Set Receiver Address(Include Port Number)
	/*
	 * Socket Address Setting
	 * - sin_family : AF_INET - IPv4
	 * - sin_port : htons(0000)- Port Number Set(Big Endian Short)
	 * - s_addr : htonl(INADDR_ANY) - Self(Own) IP Address Set(Big Endian Long)
	 *            inet_addr("000.000.000.000") - User Defined IP Address Set
	 */
	(SOCKET_VOID) memset(&stSocket_Temp.stSockAddr, 0, stSocket_Temp.uiSockAddrSize);
	stSocket_Temp.stSockAddr.sin_family = AF_INET;
	stSocket_Temp.stSockAddr.sin_port = htons(usPortNum);
	if(cpIpAddr != NULL)
	{
		stSocket_Temp.stSockAddr.sin_addr.s_addr = inet_addr(cpIpAddr);
	}
	else
	{
		stSocket_Temp.stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	// Setting to prevent Time-wait Bind Error when try Same Address re-connection
	/*
	 * SetSockOpt
	 * - SOL_SOCKET : Level
	 * - SO_REUSEADDR : Re-use Address Flag before Time-wait(About 90s in Linux)
	 * - Option : 0(Off), 1(On)
	 * - Option Length
	 */
	iReuseAddrOption = 1;
	(SOCKET_VOID) setsockopt(stSocket_Temp.iSockId, SOL_SOCKET, SO_REUSEADDR, &iReuseAddrOption, sizeof(iReuseAddrOption));

	// Step3 : Bind
	/*
	 * bind
	 * - iSockId : SocketID(Return ID of socket())
	 * - &stSockAddr : Socket Address for Server(AF_INET : struct sockaddr_in, AF_UNIX : struct sockaddr, Same size)
	 * - uiSockAddrSize : Socket Address Structure Size
	 * - Return : Success 0, Fail -1
	 */
#if((DISP_SOCKET_RESULT > 0) || (DISP_SOCKET_ERROR_WARNING == 1))
	iTemp = bind(stSocket_Temp.iSockId, &stSocket_Temp.stSockAddr, stSocket_Temp.uiSockAddrSize);

#if(DISP_SOCKET_RESULT > 0)
	if(iTemp == 0)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Rx Socket : Bind OK\n");
	}
#endif
#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(iTemp == -1)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Rx Socket : Bind ERROR\n");
	}
#endif
#else
	(SOCKET_VOID) bind(stSocket_Temp.iSockId, &stSocket_Temp.stSockAddr, stSocket_Temp.uiSockAddrSize);
#endif

	// Step4 : Listen
	/*
	 * listen
	 * - iSockId : SocketID(Return ID of socket())
	 * - SOCKET_TCP_QUEUE_SIZE : Connection Waiting Queue Size, TempSocket will be closed when TCP connected
	 * - Return : Success 0, Fail -1
	 */
#if((DISP_SOCKET_RESULT > 0) || (DISP_SOCKET_ERROR_WARNING == 1))
	iTemp = listen(stSocket_Temp.iSockId, SOCKET_TCP_QUEUE_SIZE);

#if(DISP_SOCKET_RESULT > 0)
	if(iTemp == 0)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Rx Socket : Listen Start\n");
	}
#endif
#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(iTemp == -1)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Rx Socket : Listen ERROR\n");
	}
#endif
#else
	(SOCKET_VOID) listen(stSocket_Temp.iSockId, SOCKET_TCP_QUEUE_SIZE);
#endif

	// Step5 : Accept
	stSocket_TCP.iSockId = accept(stSocket_Temp.iSockId, &stSocket_TCP.stSockAddr, &stSocket_TCP.uiSockAddrSize);

	// Step6 : Copy Listen Address
	stSocket_TCP.iSockStatus = enum_Socket_Status_NotDefined;
	stSocket_TCP.iSockType = enum_Socket_Type_TCP_IPv4Rx;
	stSocket_TCP.uiSockAddrSize = sizeof(struct sockaddr_in);
	stSocket_TCP.stTimeOutVal.tv_sec = lTimeOut_s;
	stSocket_TCP.stTimeOutVal.tv_usec = lTimeOut_us;
	(SOCKET_VOID) memcpy(&stSocket_TCP.stSockAddr, &stSocket_Temp.stSockAddr, (SOCKET_UINT32)sizeof(stSocket_Temp.stSockAddr));

	if(stSocket_TCP.iSockId != -1)
	{
		stSocket_TCP.iSockStatus = enum_Socket_Status_Connected;

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Rx Socket : Accept OK\n");
#endif
	}
#if(DISP_SOCKET_ERROR_WARNING == 1)
	else
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Normal) IPv4 Rx Socket : Accept ERROR\n");
	}
#endif

	// Step7 : Set TimeOut(if 0, Blocking on recv)
	/*
	 * SetSockOpt
	 * - SOL_SOCKET : Level
	 * - SO_SNDTIMEO(sendto TimeOut), SO_RCVTIMEO(recvfrom TimeOut)
	 * - timeval Address
	 * - timeval Length
	 */
	if( (stSocket_TCP.stTimeOutVal.tv_sec > (SOCKET_INT64)0) || (stSocket_TCP.stTimeOutVal.tv_usec > (SOCKET_INT64)0) )
	{
		(SOCKET_VOID) setsockopt(stSocket_TCP.iSockId, SOL_SOCKET, SO_RCVTIMEO, &stSocket_TCP.stTimeOutVal, sizeof(stSocket_TCP.stTimeOutVal));
	}

	// Step8 : Close TCP Temp Socket
	(SOCKET_VOID) f_SocketClose(stSocket_Temp);

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP IPv4 Rx Socket[listen %s:%u, recv Timeout %ld sec %ld usec] --> ID %d, Status %d, Type %d\n", \
			cpIpAddr, usPortNum, stSocket_TCP.stTimeOutVal.tv_sec, stSocket_TCP.stTimeOutVal.tv_usec, stSocket_TCP.iSockId, stSocket_TCP.iSockStatus, stSocket_TCP.iSockType);
#endif

	return stSocket_TCP;
}

st_Socket f_SocketInitTCP_IPv4Rx_Sync(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us, const SOCKET_INT32 iMaxSyncTime_s)
{
	/*
	 * Initialize TCP Socket for Rx(With Sync)
	 * Step1 : Define UDP Socket and Set Receiver Address(Include Port Number)
	 * Step2 : Wait 'SOCKET_SYNC_PASS_TX_LINE_CHECK' during iMaxSyncTime_s(No SyncData = Fail)
	 * Step3 : Send 'SOCKET_SYNC_PASS_RX_LINE_CHECK' to Transmitter Socket Address obtained via recvfrom
	 * Step4 : Define TCP Temp Socket to Receive Connection Request
	 * Step5 : Start listen and send 'SOCKET_SYNC_PASS_LISTEN' via UDP Socket
	 * Step6 : Receive 'SOCKET_SYNC_PASS_CONNECT' via UDP Socket and start accept
	 * Step7 : Send 'SOCKET_SYNC_PASS_SEND_TO_TX' via TCP Socket
	 * Step8 : Receive 'SOCKET_SYNC_PASS_SEND_TO_RX' via TCP Socket
	 * Step9 : Close UDP Socket and TCP Temp Socket
	 * Input
	 * - cpIpAddr : IPv4 Address(Destination IP)
	 *   NULL : Self IP
	 *   Others : User Defined IP
	 * - usPortNum : Port Number
	 * - lTimeOut_s : Socket Receive TimeOut(sec)
	 * - lTimeOut_us : Socket Receive TimeOut(usec)
	 * - iMaxSyncTime_s : Maximum Wait Time for 'UDP Connection Check' via UDP Socket(0 : infinite)
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined, enum_Socket_Status_Disconnected(Sync TimeOut), enum_Socket_Status_Connected(Accept Success)
	 *   stSocket.iSockType : enum_Socket_Type_TCP_IPv4Rx
	 *   stSocket.iSockId : Fail(-1) or Pass(Others)
	 */

	st_Socket stSocket_TCP;

	st_Socket stSocket_UDP, stSocket_Temp;
	SOCKET_INT32 iTemp, iTxRxData;
	SOCKET_INT32 iRunStep_s, iRunTime_s, iRunFlag, iMaxRunTime_s;
	SOCKET_INT32 iReuseAddrOption;

	iRunStep_s = 1;
	iRunFlag = 1;
	iRunTime_s = 0;
	iMaxRunTime_s = (iMaxSyncTime_s == 0) ? INT32_MAX : iMaxSyncTime_s;

	// Step1 : Define UDP Socket and Set Receiver Address(Include Port Number)
	// UDP Socket TimeOut = 1s (iRunStep_s)
	stSocket_UDP = f_SocketInitUDP_IPv4Rx(cpIpAddr, usPortNum, (SOCKET_INT64)iRunStep_s, (SOCKET_INT64)0);

	// Step2 : Wait 'SOCKET_SYNC_PASS_TX_LINE_CHECK' during iMaxSyncTime_s(No SyncData = Fail)
	iTxRxData = 0;
	while(iRunFlag > 0)
	{
		// Since there is no Transmitter Address, Use Pre-definced Socket when Receive
		(SOCKET_VOID) f_SocketRecvUDP_IPv4_Address(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));
		if(iTxRxData == SOCKET_SYNC_PASS_TX_LINE_CHECK)
		{
			iRunFlag = 0;

#if(DISP_SOCKET_RESULT > 0)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : TX Line Check(Elapsed Time : %d [sec])\n", \
					iRunTime_s);
#endif

			// Step3 : Send 'SOCKET_SYNC_PASS_RX_LINE_CHECK' to Transmitter Socket Address obtained via recvfrom
			iTxRxData = SOCKET_SYNC_PASS_RX_LINE_CHECK;
			(SOCKET_VOID) f_SocketSendUDP_IPv4_Normal(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));

#if(DISP_SOCKET_RESULT > 0)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : RX Line Check\n");
#endif
		}
		else
		{
			iRunTime_s = iRunTime_s + iRunStep_s;

#if(DISP_SOCKET_RESULT > 0)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : TX Line Check(Elapsed Time : %d [sec])\n", \
					iRunTime_s);
#endif

			if(iRunTime_s >= iMaxRunTime_s)
			{
				iRunFlag = -1;

				stSocket_TCP.iSockStatus = enum_Socket_Status_Disconnected;
				stSocket_TCP.iSockId = -1;
				stSocket_TCP.uiSockAddrSize = sizeof(struct sockaddr_in);
				(SOCKET_VOID) memcpy(&stSocket_TCP.stSockAddr, &stSocket_UDP.stSockAddr, (SOCKET_UINT32)sizeof(stSocket_UDP.stSockAddr));
			}
		}
	}

	if(iRunFlag == 0)
	{
		// Step4 : Define TCP Temp Socket to Receive Connection Request
		/*
		 * Socket
		 * - PF_INET : IPv4, PF_INET6 : IPv6, PF_LOCAL : UNIX Protocol for LOCAL Communication, PF_PACKET : Low Level Socket Interface
		 * - SOCK_STREAM : TCP, SOCK_DGRAM : UDP, SOCK_RAW : User Defined
		 * - IPPROTO_TCP : TCP, IPPROTO_UDP : UDP, 0 : Follow SOCK_STREAM setting
		 * - Return : Success ID, Fail -1
		 */
		stSocket_Temp.iSockId = socket(PF_INET, (SOCKET_INT32)SOCK_STREAM, (SOCKET_INT32)IPPROTO_TCP);
		stSocket_Temp.iSockType = enum_Socket_Type_NotDefined;
		stSocket_Temp.uiSockAddrSize = sizeof(struct sockaddr_in);

		/*
		 * Socket Address Setting
		 * - sin_family : AF_INET - IPv4
		 * - sin_port : htons(0000)- Port Number Set(Big Endian Short)
		 * - s_addr : htonl(INADDR_ANY) - Self(Own) IP Address Set(Big Endian Long)
		 *            inet_addr("000.000.000.000") - User Defined IP Address Set
		 */
		(SOCKET_VOID) memset(&stSocket_Temp.stSockAddr, 0, stSocket_Temp.uiSockAddrSize);
		stSocket_Temp.stSockAddr.sin_family = AF_INET;
		stSocket_Temp.stSockAddr.sin_port = htons(usPortNum);
		if(cpIpAddr != NULL)
		{
			stSocket_Temp.stSockAddr.sin_addr.s_addr = inet_addr(cpIpAddr);
		}
		else
		{
			stSocket_Temp.stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
		}

		// Setting to prevent Time-wait Bind Error when try Same Address re-connection
		/*
		 * SetSockOpt
		 * - SOL_SOCKET : Level
		 * - SO_REUSEADDR : Re-use Address Flag before Time-wait(About 90s in Linux)
		 * - Option : 0(Off), 1(On)
		 * - Option Length
		 */
		iReuseAddrOption = 1;
		(SOCKET_VOID) setsockopt(stSocket_Temp.iSockId, SOL_SOCKET, SO_REUSEADDR, &iReuseAddrOption, sizeof(iReuseAddrOption));

		/*
		 * bind
		 * - iSockId : SocketID(Return ID of socket())
		 * - &stSockAddr : Socket Address for Server(AF_INET : struct sockaddr_in, AF_UNIX : struct sockaddr, Same size)
		 * - uiSockAddrSize : Socket Address Structure Size
		 * - Return : Success 0, Fail -1
		 */
#if((DISP_SOCKET_RESULT > 0) || (DISP_SOCKET_ERROR_WARNING == 1))
		iTemp = bind(stSocket_Temp.iSockId, &stSocket_Temp.stSockAddr, stSocket_Temp.uiSockAddrSize);

#if(DISP_SOCKET_RESULT > 0)
		if(iTemp == 0)
		{
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : Bind OK\n");
		}
#endif
#if(DISP_SOCKET_ERROR_WARNING == 1)
		if(iTemp == -1)
		{
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : Bind ERROR\n");
		}
#endif
#else
		(SOCKET_VOID) bind(stSocket_Temp.iSockId, &stSocket_Temp.stSockAddr, stSocket_Temp.uiSockAddrSize);
#endif

		// Step5 : Start listen and send 'SOCKET_SYNC_PASS_LISTEN' via UDP Socket
		/*
		 * listen
		 * - iSockId : SocketID(Return ID of socket())
		 * - SOCKET_TCP_QUEUE_SIZE : Connection Waiting Queue Size, TempSocket will be closed when TCP connected
		 * - Return : Success 0, Fail -1
		 */
		iTemp = listen(stSocket_Temp.iSockId, SOCKET_TCP_QUEUE_SIZE);
		if(iTemp == 0)
		{
			iTxRxData = SOCKET_SYNC_PASS_LISTEN;
			(SOCKET_VOID) f_SocketSendUDP_IPv4_Normal(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));

#if(DISP_SOCKET_RESULT > 0)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : Listen Start\n");
#endif
		}
#if(DISP_SOCKET_ERROR_WARNING == 1)
		else
		{
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : Listen ERROR\n");
		}
#endif

		// Step6 : Receive 'SOCKET_SYNC_PASS_CONNECT' via UDP Socket and start accept
		while(iTxRxData != SOCKET_SYNC_PASS_CONNECT)
		{
			(SOCKET_VOID) f_SocketRecvUDP_IPv4_Normal(&stSocket_UDP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));
		}

		stSocket_TCP.iSockId = accept(stSocket_Temp.iSockId, &stSocket_TCP.stSockAddr, &stSocket_TCP.uiSockAddrSize);

		stSocket_TCP.iSockStatus = enum_Socket_Status_NotDefined;
		stSocket_TCP.iSockType = enum_Socket_Type_TCP_IPv4Rx;
		stSocket_TCP.stTimeOutVal.tv_sec = lTimeOut_s;
		stSocket_TCP.stTimeOutVal.tv_usec = lTimeOut_us;
		stSocket_TCP.uiSockAddrSize = sizeof(struct sockaddr_in);
		(SOCKET_VOID) memcpy(&stSocket_TCP.stSockAddr, &stSocket_Temp.stSockAddr, (SOCKET_UINT32)sizeof(stSocket_Temp.stSockAddr));

		if(stSocket_TCP.iSockId != -1)
		{
			stSocket_TCP.iSockStatus = enum_Socket_Status_Connected;

#if(DISP_SOCKET_RESULT > 0)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : Accept OK\n");
#endif
		}
#if(DISP_SOCKET_ERROR_WARNING == 1)
		else
		{
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : Accept ERROR\n");
		}
#endif

		/*
		 * SetSockOpt
		 * - SOL_SOCKET : Level
		 * - SO_SNDTIMEO(sendto TimeOut), SO_RCVTIMEO(recvfrom TimeOut)
		 * - timeval Address
		 * - timeval Length
		 */
		if( (stSocket_TCP.stTimeOutVal.tv_sec > (SOCKET_INT64)0) || (stSocket_TCP.stTimeOutVal.tv_usec > (SOCKET_INT64)0) )
		{
			(SOCKET_VOID) setsockopt(stSocket_TCP.iSockId, SOL_SOCKET, SO_RCVTIMEO, &stSocket_TCP.stTimeOutVal, sizeof(stSocket_TCP.stTimeOutVal));
		}

		// Step7 : Send 'SOCKET_SYNC_PASS_SEND_TO_TX' via TCP Socket
		iTxRxData = SOCKET_SYNC_PASS_SEND_TO_TX;
		(SOCKET_VOID) f_SocketSendTCP_IPv4(&stSocket_TCP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : TX Data OK\n");
#endif

		// Step8 : Receive 'SOCKET_SYNC_PASS_SEND_TO_RX' via TCP Socket
		while(iTxRxData != SOCKET_SYNC_PASS_SEND_TO_RX)
		{
			(SOCKET_VOID) f_SocketRecvTCP_IPv4(&stSocket_TCP, &iTxRxData, (SOCKET_INT64)sizeof(iTxRxData));
		}

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP(Sync) IPv4 Rx Socket : RX Data OK\n");
#endif

		// Step9 : Close UDP Socket and TCP Temp Socket
		(SOCKET_VOID) f_SocketClose(stSocket_UDP);
		(SOCKET_VOID) f_SocketClose(stSocket_Temp);
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial TCP IPv4 Rx Socket[listen %s:%u, recv Timeout %ld sec %ld usec] --> ID %d, Status %d, Type %d\n", \
			cpIpAddr, usPortNum, stSocket_TCP.stTimeOutVal.tv_sec, stSocket_TCP.stTimeOutVal.tv_usec, stSocket_TCP.iSockId, stSocket_TCP.iSockStatus, stSocket_TCP.iSockType);
#endif

	return stSocket_TCP;
}

SOCKET_INT64 f_SocketSendTCP_IPv4(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize)
{
	/*
	 * Send Data via TCP
	 * Use 'send' function with 'MSG_NOSIGNAL' option to prevent SIGPIPE
	 * Consider Error Case(Optional)
	 * - Connect Fail Case & Lost Link Case(include send Fail) --> Close current socket & Initial new socket
	 * Data Transmit will be occurred if and only if enum_Socket_Status_Connected status
	 * Input
	 * - stpSocket : Pointer to Socket structure
	 * - vpDataAddr : Address to Send
	 * - lFixedDataSize : Fixed Data size to Send
	 * Output
	 * - lSendSize : Send Size
	 */

	SOCKET_INT64 lSendSize;

	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_INT64 lRemainSize, lTempSize;

	ucpAddr = vpDataAddr;

	lSendSize = (SOCKET_INT64)0;
	lRemainSize = lFixedDataSize;

#if(CTRL_SOCKET_LINK_RECOVERY == 1)
	// Connect Fail Case & Lost Link Case(include send Fail) --> Close current socket & Initial new socket
	while(stpSocket->iSockStatus != enum_Socket_Status_Connected)
	{
		(SOCKET_VOID) usleep((SOCKET_UINT32)100 * (SOCKET_UINT32)1000);	// 100ms

		// Close current socket to prevent Time-wait
		(SOCKET_VOID) close(stpSocket->iSockId);

		// Initial new socket
		st_Socket stSock_Temp;
		stSock_Temp = f_SocketInitTCP_IPv4Tx_Normal(inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), stpSocket->stTimeOutVal.tv_sec, stpSocket->stTimeOutVal.tv_usec);
		(SOCKET_VOID) memcpy(stpSocket, &stSock_Temp, (SOCKET_UINT32)sizeof(st_Socket));
	}
#endif

	while((lRemainSize > (SOCKET_INT64)0) && (stpSocket->iSockStatus == enum_Socket_Status_Connected))
	{
		// Send Data
		// Returns the number sent or -1
		// Use 'send' function with 'MSG_NOSIGNAL' option to prevent SIGPIPE
		lTempSize = send(stpSocket->iSockId, &(ucpAddr[lSendSize]), (SOCKET_UINT32)lRemainSize, MSG_NOSIGNAL);
		if(lTempSize != (SOCKET_INT64)-1)
		{
			lSendSize = lSendSize + lTempSize;
			lRemainSize = lFixedDataSize - lSendSize;

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send TCP IPv4[to %s:%u] : %ld / %ld Byte(Remained %ld Byte)\n", \
					inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), \
					lSendSize, lFixedDataSize, lRemainSize);
#endif
		}
		else
		{
			stpSocket->iSockStatus = enum_Socket_Status_Disconnected;

#if(DISP_SOCKET_ERROR_WARNING == 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Send TCP\n");
#endif
		}
	}

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(stpSocket->iSockStatus != enum_Socket_Status_Connected)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send TCP IPv4[to %s:%u] : Unconnected or Disconnected\n", \
				inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port));
	}
#endif

	return lSendSize;
}

SOCKET_INT64 f_SocketRecvTCP_IPv4(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize)
{
	/*
	 * Receive Data via TCP
	 * Consider Error Case(Optional)
	 * - Lost Link Case(include receive Fail) --> Close current socket & Initial new socket
	 * Data Transmit will be occurred if and only if enum_Socket_Status_Connected status
	 * Input
	 * - stpSocket : Pointer to Socket structure
	 * - vpDataAddr : Address to Receive
	 * - lFixedDataSize : Fixed Data size to Receive
	 * Output
	 * - lReceiveSize : Receive Size
	 */

	SOCKET_INT64 lReceiveSize;

	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_INT64 lRemainSize, lTempSize;

	ucpAddr = vpDataAddr;

	lReceiveSize = (SOCKET_INT64)0;
	lRemainSize = lFixedDataSize;

#if(CTRL_SOCKET_LINK_RECOVERY == 1)
	// Lost Link Case(include receive Fail) --> Close current socket & Initial new socket
	if(stpSocket->iSockStatus != enum_Socket_Status_Connected)
	{
		// Close current socket
		(SOCKET_VOID) close(stpSocket->iSockId);

		// Initial new socket
		st_Socket stSock_Temp;
		stSock_Temp = f_SocketInitTCP_IPv4Rx_Normal(inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), stpSocket->stTimeOutVal.tv_sec, stpSocket->stTimeOutVal.tv_usec);
		(SOCKET_VOID) memcpy(stpSocket, &stSock_Temp, (SOCKET_UINT32)sizeof(st_Socket));
	}
#endif

	while((lRemainSize > (SOCKET_INT64)0) && (stpSocket->iSockStatus == enum_Socket_Status_Connected))
	{
		// Receive Data
		// Returns the number read or -1 for errors
		// In case of Link lost, return 0
		lTempSize = recv(stpSocket->iSockId, &(ucpAddr[lReceiveSize]), (SOCKET_UINT32)lRemainSize, 0);
		if(lTempSize > (SOCKET_INT64)0)
		{
			lReceiveSize = lReceiveSize + lTempSize;
			lRemainSize = lFixedDataSize - lReceiveSize;

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Receive TCP IPv4[at %s:%u] : %ld / %ld Byte(Remained %ld Byte)\n", \
					inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port), \
					lReceiveSize, lFixedDataSize, lRemainSize);
#endif
		}
		else
		{
			stpSocket->iSockStatus = enum_Socket_Status_Disconnected;

#if(DISP_SOCKET_ERROR_WARNING == 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Receive TCP\n");
#endif
		}
	}

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(stpSocket->iSockStatus != enum_Socket_Status_Connected)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Receive TCP IPv4[at %s:%u] : Unconnected or Disconnected\n", \
				inet_ntoa(stpSocket->stSockAddr.sin_addr), ntohs(stpSocket->stSockAddr.sin_port));
	}
#endif

	return lReceiveSize;
}

#if(__USE_RDMA == 1)
st_Socket f_SocketInitRDMA_IPv4Tx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us)
{
	/*
	 * Initialize RDMA Socket for Tx
	 * Step1 : Init TCP Socket
	 * Step2 : Set RDMA Parameter
	 * Step3 : Init RDMA(Tx)
	 * Step4 : Start(Run) RDMA(Tx)
	 * Input
	 * - cpIpAddr : User Defined IP(Sender or Source IP)
	 * - usPortNum : Port Number
	 * - lTimeOut_s : TCP Socket Receive TimeOut(sec)
	 * - lTimeOut_us : TCP Socket Receive TimeOut(usec)
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined, enum_Socket_Status_Connected(TCP Connect & Pass Start RDMA), enum_Socket_Status_Disconnected(Fail Start RDMA)
	 *   stSocket.iSockType : enum_Socket_Type_RDMA_IPv4Tx
	 *   stSocket.iSockId : TCP Socket ID
	 */

	st_Socket stSocket;

	SOCKET_INT32 iTemp;

	// Step1 : Init TCP Socket
	stSocket = f_SocketInitTCP_IPv4Rx_Normal(cpIpAddr, usPortNum, lTimeOut_s, lTimeOut_us);

	// Step2 : Set RDMA Parameter
	(SOCKET_VOID) memcpy(stSocket.caIpAddr, cpIpAddr, (SOCKET_UINT32)strlen(cpIpAddr) + (SOCKET_UINT32)1);	//	+1 : Include Null 
	stSocket.usPortNum = usPortNum;
	stSocket.stpRDMA_CtrlBlock = malloc(sizeof(struct edrdma_cb));
	stSocket.stpRDMA_CtrlInfo = malloc(sizeof(st_CtrlInfo_RDMA));
	(SOCKET_VOID) memset(stSocket.stpRDMA_CtrlBlock, 0, (SOCKET_UINT32)sizeof(struct edrdma_cb));
	(SOCKET_VOID) memset(stSocket.stpRDMA_CtrlInfo, 0, (SOCKET_UINT32)sizeof(st_CtrlInfo_RDMA));
	stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte = (SOCKET_UINT32)EDRDMA_BUFSIZE;

	// Step3 : Init RDMA
	// Buffer Size Cannot be over INT32_MAX(2147483647)
	// Return : Success 0, Fail -1
	iTemp = init_rdma(stSocket.stpRDMA_CtrlBlock, stSocket.caIpAddr, stSocket.usPortNum, (SOCKET_INT32)stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte, enum_Socket_Mode_RDMA_IPv4Tx);
	if( (iTemp != -1 ) && (stSocket.stpRDMA_CtrlBlock->server == enum_Socket_Mode_RDMA_IPv4Tx) )
	{
		stSocket.iSockType = enum_Socket_Type_RDMA_IPv4Tx;

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial RDMA IPv4 Tx Socket : OK\n");
#endif
	}
#if(DISP_SOCKET_ERROR_WARNING == 1)
	else
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial RDMA IPv4 Tx Socket : ERROR\n");
	}
#endif

	// Step4 : Start(Run) RDMA
	// Return : Success 0, Fail -1
	iTemp = edrdma_run_server(stSocket.stpRDMA_CtrlBlock);
	if(iTemp == 0)
	{
#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Start(Run) RDMA IPv4 Tx Socket : OK\n");
#endif

		(SOCKET_VOID) f_SocketRecvTCP_IPv4(&stSocket, &(stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte), (SOCKET_INT64)sizeof(SOCKET_INT32));
		(SOCKET_VOID) f_SocketSendTCP_IPv4(&stSocket, &(stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte), (SOCKET_INT64)sizeof(SOCKET_INT32));
		stSocket.uiBuffSize_Byte = (stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte < stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte) ? stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte : stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte;

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "RDMA Buffer Size : Tx %u, Rx %u --> %u\n", stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte, stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte, stSocket.uiBuffSize_Byte);
#endif		
	}
	else
	{
		stSocket.iSockStatus = enum_Socket_Status_Disconnected;

#if(DISP_SOCKET_ERROR_WARNING == 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Start(Run) RDMA IPv4 Tx Socket : ERROR\n");
#endif
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial RDMA IPv4 Tx Socket[to %s:%u] --> ID %d, Status %d, Type %d\n", \
			stSocket.caIpAddr, stSocket.usPortNum, stSocket.iSockId, stSocket.iSockStatus, stSocket.iSockType);
#endif

	return stSocket;
}

st_Socket f_SocketInitRDMA_IPv4Rx(const SOCKET_CHAR8 *cpIpAddr, const SOCKET_UINT16 usPortNum, const SOCKET_INT64 lTimeOut_s, const SOCKET_INT64 lTimeOut_us)
{
	/*
	 * Initialize RDMA Socket for Rx
	 * Step1 : Init TCP Socket
	 * Step2 : Set RDMA Parameter
	 * Step3 : Init RDMA(Rx)
	 * Step4 : Start(Run) RDMA(Rx)
	 * Input
	 * - cpIpAddr : User Defined IP(Sender or Source IP)
	 * - usPortNum : Port Number
	 * - lTimeOut_s : TCP Socket Send TimeOut(sec)
	 * - lTimeOut_us : TCP Socket Send TimeOut(usec)
	 * Output
	 * - stSocket : Socket structure
	 *   stSocket.iSockStatus : enum_Socket_Status_NotDefined, enum_Socket_Status_Connected(TCP Connect & Pass Start RDMA), enum_Socket_Status_Disconnected(Fail Start RDMA)
	 *   stSocket.iSockType : enum_Socket_Type_RDMA_IPv4Rx
	 *   stSocket.iSockId : TCP Socket ID
	 */

	st_Socket stSocket;

	SOCKET_INT32 iTemp;

	// Step1 : Init TCP Socket
	stSocket = f_SocketInitTCP_IPv4Tx_Normal(cpIpAddr, usPortNum, lTimeOut_s, lTimeOut_us);

	// Step2 : Set RDMA Parameter
	(SOCKET_VOID) memcpy(stSocket.caIpAddr, cpIpAddr, (SOCKET_UINT32)strlen(cpIpAddr) + (SOCKET_UINT32)1);	//	+1 : Include Null 
	stSocket.usPortNum = usPortNum;
	stSocket.stpRDMA_CtrlBlock = malloc(sizeof(struct edrdma_cb));
	stSocket.stpRDMA_CtrlInfo = malloc(sizeof(st_CtrlInfo_RDMA));
	(SOCKET_VOID) memset(stSocket.stpRDMA_CtrlBlock, 0, (SOCKET_UINT32)sizeof(struct edrdma_cb));
	(SOCKET_VOID) memset(stSocket.stpRDMA_CtrlInfo, 0, (SOCKET_UINT32)sizeof(st_CtrlInfo_RDMA));
	stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte = (SOCKET_UINT32)EDRDMA_BUFSIZE;

	// Step3 : Init RDMA
	// Buffer Size Cannot be over INT32_MAX(2147483647)
	// Return : Success 0, Fail -1
	iTemp = init_rdma(stSocket.stpRDMA_CtrlBlock, stSocket.caIpAddr, stSocket.usPortNum, (SOCKET_INT32)stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte, enum_Socket_Mode_RDMA_IPv4Rx);
	if( (iTemp != -1 ) && (stSocket.stpRDMA_CtrlBlock->server == enum_Socket_Mode_RDMA_IPv4Rx) )
	{
		stSocket.iSockType = enum_Socket_Type_RDMA_IPv4Rx;

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial RDMA IPv4 Rx Socket : OK\n");
#endif
	}
#if(DISP_SOCKET_ERROR_WARNING == 1)
	else
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial RDMA IPv4 Rx Socket : ERROR\n");
	}
#endif

	// Step4 : Start(Run) RDMA
	// Return : Success 0, Fail -1
	iTemp = edrdma_run_client(stSocket.stpRDMA_CtrlBlock);
	if(iTemp == 0)
	{
#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Start(Run) RDMA IPv4 Tx Socket : OK\n");
#endif

		(SOCKET_VOID) f_SocketSendTCP_IPv4(&stSocket, &(stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte), (SOCKET_INT64)sizeof(SOCKET_INT32));
		(SOCKET_VOID) f_SocketRecvTCP_IPv4(&stSocket, &(stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte), (SOCKET_INT64)sizeof(SOCKET_INT32));
		stSocket.uiBuffSize_Byte = (stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte < stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte) ? stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte : stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte;

#if(DISP_SOCKET_RESULT > 0)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "RDMA Buffer Size : Tx %u, Rx %u --> %u\n", stSocket.stpRDMA_CtrlInfo->uiTxBuffSize_Byte, stSocket.stpRDMA_CtrlInfo->uiRxBuffSize_Byte, stSocket.uiBuffSize_Byte);
#endif				
	}
	else
	{
		stSocket.iSockStatus = enum_Socket_Status_Disconnected;

#if(DISP_SOCKET_ERROR_WARNING == 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Start(Run) RDMA IPv4 Tx Socket : ERROR\n");
#endif
	}

#if(DISP_SOCKET_RESULT > 0)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Initial RDMA IPv4 Rx Socket[to %s:%u] --> ID %d, Status %d, Type %d\n", \
			stSocket.caIpAddr, stSocket.usPortNum, stSocket.iSockId, stSocket.iSockStatus, stSocket.iSockType);
#endif

	return stSocket;
}

SOCKET_INT64 f_SocketSendRDMA_IPv4_OffsetResetEveryTime(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lDataSize)
{
	SOCKET_INT64 lSendSize;

	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_INT32 iTemp, iCtrlMsg;

	ucpAddr = vpDataAddr;

	iCtrlMsg = 0;
	lSendSize = (SOCKET_INT64)0;

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lDataSize > (SOCKET_INT64)stpSocket->uiBuffSize_Byte)
	{
(SOCKET_VOID) dprintf(STDOUT_FILENO, "[WARNING] %ld Byte Data cannot send via RDMA(Offset Reset Every Time) Buffer(%u Byte)\n", lDataSize, stpSocket->uiBuffSize_Byte);
	}
#endif

	// CtrlInfo Setting
	stpSocket->stpRDMA_CtrlInfo->uiDataOffset = (SOCKET_UINT32)0;
	stpSocket->stpRDMA_CtrlInfo->uiDataLength = (lDataSize > (SOCKET_INT64)stpSocket->uiBuffSize_Byte) ? stpSocket->uiBuffSize_Byte : (SOCKET_UINT32)lDataSize;

	// Memory Copy to Send
	(SOCKET_VOID) memcpy(&(stpSocket->stpRDMA_CtrlBlock->rdma_buf[stpSocket->stpRDMA_CtrlInfo->uiDataOffset]), ucpAddr, stpSocket->stpRDMA_CtrlInfo->uiDataLength);

#if(DISP_SOCKET_RESULT > 1)
	(SOCKET_VOID) dprintf(STDOUT_FILENO, "Memory Copy to Offset %u(Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
#endif

	// Send Data
	// Return : Success 0, Fail -1
	iTemp = edrdma_send_data(stpSocket->stpRDMA_CtrlBlock, stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);	
	if(iTemp == 0)
	{
		// Send CtrlInfo
		(SOCKET_VOID) f_SocketSendTCP_IPv4(stpSocket, stpSocket->stpRDMA_CtrlInfo, (SOCKET_INT64)sizeof(st_CtrlInfo_RDMA));

#if(DISP_SOCKET_RESULT > 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send CtrlInfo(Offset %u, Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Wait RecvDoneMsg from Rx\n");
#endif

		// Wait RecvDoneMsg from Rx
		(SOCKET_VOID) f_SocketRecvTCP_IPv4(stpSocket, &iCtrlMsg, (SOCKET_INT64)sizeof(SOCKET_INT32));
		if(iCtrlMsg == SOCKET_RDMA_RECV_DONE)
		{
			// Update DataOffset
			stpSocket->stpRDMA_CtrlInfo->uiDataOffset = stpSocket->stpRDMA_CtrlInfo->uiDataOffset + stpSocket->stpRDMA_CtrlInfo->uiDataLength;

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Update OffsetInfo : %u\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset);
#endif

			// Set SendSize
			lSendSize = lSendSize + (SOCKET_INT64)stpSocket->stpRDMA_CtrlInfo->uiDataLength;
		}
	}
#if(DISP_SOCKET_ERROR_WARNING == 1)
	else
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Send RDMA(Offset Reset Every Time)\n");
	}
#endif

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(lSendSize != lDataSize)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[WARNING] Send RDMA(Offset Reset Every Time) %ld / %ld Byte only\n", lSendSize, lDataSize);
	}
#endif

#if(DISP_SOCKET_RESULT > 0)
	if(lSendSize != (SOCKET_INT64)0)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send RDMA(Offset Reset Every Time) IPv4[to %s:%u] : %ld / %ld Byte\n", \
				stpSocket->caIpAddr, stpSocket->usPortNum, lSendSize, lDataSize);
	}
#endif
	
	return lSendSize;
}

SOCKET_INT64 f_SocketSendRDMA_IPv4_OffsetResetWhenFull(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize)
{
	SOCKET_INT64 lSendSize;

	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_UINT32 uiEmptySize;
	SOCKET_INT32 iTemp, iCtrlMsg;
	SOCKET_INT64 lRemainSize;

	ucpAddr = vpDataAddr;

	iCtrlMsg = 0;
	lSendSize = (SOCKET_INT64)0;
	lRemainSize = lFixedDataSize;

	while((lRemainSize > (SOCKET_INT64)0) && (stpSocket->iSockStatus == enum_Socket_Status_Connected))
	{
		// Empty Size Check
		uiEmptySize = stpSocket->uiBuffSize_Byte - stpSocket->stpRDMA_CtrlInfo->uiDataOffset;

		if(uiEmptySize == 0)
		{
#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Wait RecvDoneMsg from Rx\n");			
#endif

			// Wait RecvDoneMsg from Rx
			(SOCKET_VOID) f_SocketRecvTCP_IPv4(stpSocket, &iCtrlMsg, (SOCKET_INT64)sizeof(SOCKET_INT32));
			if(iCtrlMsg == SOCKET_RDMA_RECV_DONE)
			{
				// Update DataOffset
				stpSocket->stpRDMA_CtrlInfo->uiDataOffset = (SOCKET_UINT32)0;

#if(DISP_SOCKET_RESULT > 1)
				(SOCKET_VOID) dprintf(STDOUT_FILENO, "Reset OffsetInfo : %u\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset);
#endif
			}
		}
		else
		{
			// CtrlInfo Setting
			stpSocket->stpRDMA_CtrlInfo->uiDataLength = (lRemainSize > (SOCKET_INT64)uiEmptySize) ? uiEmptySize : (SOCKET_UINT32)lRemainSize;

			// Memory Copy to Send
			(SOCKET_VOID) memcpy(&(stpSocket->stpRDMA_CtrlBlock->rdma_buf[stpSocket->stpRDMA_CtrlInfo->uiDataOffset]), &(ucpAddr[lSendSize]), stpSocket->stpRDMA_CtrlInfo->uiDataLength);

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Memory Copy to Offset %u(Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
#endif

			// Send Data
			// Return : Success 0, Fail -1
			iTemp = edrdma_send_data(stpSocket->stpRDMA_CtrlBlock, stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);	
			if(iTemp == 0)
			{
				// Send CtrlInfo
				(SOCKET_VOID) f_SocketSendTCP_IPv4(stpSocket, stpSocket->stpRDMA_CtrlInfo, (SOCKET_INT64)sizeof(st_CtrlInfo_RDMA));

#if(DISP_SOCKET_RESULT > 1)
				(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send CtrlInfo(Offset %u, Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
#endif

				// Update DataOffset
				stpSocket->stpRDMA_CtrlInfo->uiDataOffset = stpSocket->stpRDMA_CtrlInfo->uiDataOffset + stpSocket->stpRDMA_CtrlInfo->uiDataLength;

#if(DISP_SOCKET_RESULT > 1)
				(SOCKET_VOID) dprintf(STDOUT_FILENO, "Update OffsetInfo : %u\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset);
#endif

				// Set SendSize
				lSendSize = lSendSize + (SOCKET_INT64)stpSocket->stpRDMA_CtrlInfo->uiDataLength;
			}
			else
			{
				stpSocket->iSockStatus = enum_Socket_Status_Disconnected;

#if(DISP_SOCKET_ERROR_WARNING == 1)
				(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Send RDMA(Offset Reset When Full)\n");
#endif
			}
		}

		lRemainSize = lFixedDataSize - lSendSize;

#if(DISP_SOCKET_RESULT > 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send RDMA(Offset Reset When Full) IPv4[to %s:%u] : %ld / %ld Byte(Remained %ld Byte)\n", \
				stpSocket->caIpAddr, stpSocket->usPortNum, \
				lSendSize, lFixedDataSize, lRemainSize);
#endif		
	}

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(stpSocket->iSockStatus != enum_Socket_Status_Connected)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send RDMA(Offset Reset When Full) IPv4[to %s:%u] : Unconnected or Disconnected\n", \
				stpSocket->caIpAddr, stpSocket->usPortNum);
	}
#endif		

	return lSendSize;
}

SOCKET_INT64 f_SocketRecvRDMA_IPv4_OffsetResetEveryTime(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr)
{
	SOCKET_INT64 lRecvSize;

	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_INT32 iTemp, iCtrlMsg;
	st_CtrlInfo_RDMA stDumpCtrlInfo;

	ucpAddr = vpDataAddr;

	iCtrlMsg = 0;
	lRecvSize = (SOCKET_INT64)0;

	// Recv Data
	// Return : Success 0, Fail -1
	iTemp = edrdma_recv_data(stpSocket->stpRDMA_CtrlBlock, &(stDumpCtrlInfo.uiDataOffset), &(stDumpCtrlInfo.uiDataLength));
	if(iTemp == 0)
	{
		// Recv CtrlInfo
		(SOCKET_VOID) f_SocketRecvTCP_IPv4(stpSocket, stpSocket->stpRDMA_CtrlInfo, (SOCKET_INT64)sizeof(st_CtrlInfo_RDMA));

#if(DISP_SOCKET_RESULT > 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Recv CtrlInfo(Offset %u, Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
#endif

		// Memory Copy from Recv
		(SOCKET_VOID) memcpy(ucpAddr, &(stpSocket->stpRDMA_CtrlBlock->rdma_buf[stpSocket->stpRDMA_CtrlInfo->uiDataOffset]), stpSocket->stpRDMA_CtrlInfo->uiDataLength);

#if(DISP_SOCKET_RESULT > 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Memory Copy from Offset %u(Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
#endif

		// Send RecvDoneMsg to Tx
		iCtrlMsg = SOCKET_RDMA_RECV_DONE;
		(SOCKET_VOID) f_SocketSendTCP_IPv4(stpSocket, &iCtrlMsg, (SOCKET_INT64)sizeof(SOCKET_INT32));

#if(DISP_SOCKET_RESULT > 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send RecvDoneMsg to Rx\n");
#endif

		// Update DataOffset
		stpSocket->stpRDMA_CtrlInfo->uiDataOffset = stpSocket->stpRDMA_CtrlInfo->uiDataOffset + stpSocket->stpRDMA_CtrlInfo->uiDataLength;

#if(DISP_SOCKET_RESULT > 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Update OffsetInfo : %u\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset);
#endif

		// Set RecvSize		
		lRecvSize = lRecvSize + (SOCKET_INT64)stpSocket->stpRDMA_CtrlInfo->uiDataLength;
	}
#if(DISP_SOCKET_ERROR_WARNING == 1)	
	else
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Recv RDMA(Offset Reset Every Time)\n");
	}
#endif

#if(DISP_SOCKET_RESULT > 0)
	if(lRecvSize != (SOCKET_INT64)0)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Recv RDMA(Offset Reset Every Time) IPv4[from %s:%u] : %ld Byte\n", \
				stpSocket->caIpAddr, stpSocket->usPortNum, lRecvSize);
	}
#endif

	return lRecvSize;	
}

SOCKET_INT64 f_SocketRecvRDMA_IPv4_OffsetResetWhenFull(st_Socket *stpSocket, const SOCKET_VOID *vpDataAddr, const SOCKET_INT64 lFixedDataSize)
{
	SOCKET_INT64 lRecvSize;

	SOCKET_UCHAR8 *ucpAddr;
	SOCKET_UINT32 uiEmptySize;
	SOCKET_INT32 iTemp, iCtrlMsg;
	SOCKET_INT64 lRemainSize;
	st_CtrlInfo_RDMA stDumpCtrlInfo;

	ucpAddr = vpDataAddr;

	iCtrlMsg = 0;
	lRecvSize = (SOCKET_INT64)0;
	lRemainSize = lFixedDataSize;	

	while((lRemainSize > (SOCKET_INT64)0) && (stpSocket->iSockStatus == enum_Socket_Status_Connected))
	{
		// Recv Data
		// Return : Success 0, Fail -1
		iTemp = edrdma_recv_data(stpSocket->stpRDMA_CtrlBlock, &(stDumpCtrlInfo.uiDataOffset), &(stDumpCtrlInfo.uiDataLength));
		if(iTemp == 0)
		{
			// Recv CtrlInfo
			(SOCKET_VOID) f_SocketRecvTCP_IPv4(stpSocket, stpSocket->stpRDMA_CtrlInfo, (SOCKET_INT64)sizeof(st_CtrlInfo_RDMA));

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Recv CtrlInfo(Offset %u, Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
#endif			

			// Memory Copy from Recv
			(SOCKET_VOID) memcpy(&(ucpAddr[lRecvSize]), &(stpSocket->stpRDMA_CtrlBlock->rdma_buf[stpSocket->stpRDMA_CtrlInfo->uiDataOffset]), stpSocket->stpRDMA_CtrlInfo->uiDataLength);

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Memory Copy from Offset %u(Length %u) : OK\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset, stpSocket->stpRDMA_CtrlInfo->uiDataLength);
#endif

			// Update DataOffset
			stpSocket->stpRDMA_CtrlInfo->uiDataOffset = stpSocket->stpRDMA_CtrlInfo->uiDataOffset + stpSocket->stpRDMA_CtrlInfo->uiDataLength;

#if(DISP_SOCKET_RESULT > 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "Update OffsetInfo : %u\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset);
#endif

			// Empty Size Check
			uiEmptySize = stpSocket->uiBuffSize_Byte - stpSocket->stpRDMA_CtrlInfo->uiDataOffset;

			if(uiEmptySize == (SOCKET_UINT32)0)
			{
				// Send RecvDoneMsg to Tx
				iCtrlMsg = SOCKET_RDMA_RECV_DONE;
				(SOCKET_VOID) f_SocketSendTCP_IPv4(stpSocket, &iCtrlMsg, (SOCKET_INT64)sizeof(SOCKET_INT32));

#if(DISP_SOCKET_RESULT > 1)
				(SOCKET_VOID) dprintf(STDOUT_FILENO, "Send RecvDoneMsg to Tx\n");
#endif

				// Update DataOffset
				stpSocket->stpRDMA_CtrlInfo->uiDataOffset = (SOCKET_UINT32)0;

#if(DISP_SOCKET_RESULT > 1)
				(SOCKET_VOID) dprintf(STDOUT_FILENO, "Reset OffsetInfo : %u\n", stpSocket->stpRDMA_CtrlInfo->uiDataOffset);
#endif
			}

			// Set RecvSize
			lRecvSize = lRecvSize + (SOCKET_INT64)stpSocket->stpRDMA_CtrlInfo->uiDataLength;
		}
		else
		{
			stpSocket->iSockStatus = enum_Socket_Status_Disconnected;

#if(DISP_SOCKET_ERROR_WARNING == 1)
			(SOCKET_VOID) dprintf(STDOUT_FILENO, "[ERROR] Recv RDMA(Offset Reset When Full)\n");
#endif
		}

		lRemainSize = lFixedDataSize - lRecvSize;

#if(DISP_SOCKET_RESULT > 1)
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Recv RDMA(Offset Reset When Full) IPv4[to %s:%u] : %ld / %ld Byte(Remained %ld Byte)\n", \
				stpSocket->caIpAddr, stpSocket->usPortNum, \
				lRecvSize, lFixedDataSize, lRemainSize);
#endif			
	}

#if(DISP_SOCKET_ERROR_WARNING == 1)
	if(stpSocket->iSockStatus != enum_Socket_Status_Connected)
	{
		(SOCKET_VOID) dprintf(STDOUT_FILENO, "Recv RDMA(Offset Reset When Full) IPv4[from %s:%u] : Unconnected or Disconnected\n", \
				stpSocket->caIpAddr, stpSocket->usPortNum);
	}
#endif	

	return lRecvSize;
}
#endif

SOCKET_INT32 f_SocketClose(st_Socket stSocket)
{
	/*
	 * Close Socket
	 * Input
	 * - stSocket : Socket Structure included Socket ID
	 * Output
	 * - 0 : Fail
	 * - 1 : Pass
	 */

	SOCKET_INT32 iResult;

	SOCKET_INT32 iTemp;

	// Due to Code Coverage, Set Result value as FAIL and then update as PASS if no problem during the work
	iResult = SOCKET_FAIL;
	
	// Common Case
	// Return : Success 0, Fail -1
	iTemp = close(stSocket.iSockId);
	if(iTemp == 0)
	{
		iResult = SOCKET_PASS;
	}

#if(__USE_RDMA == 1)
	if(stSocket.iSockType == enum_Socket_Type_RDMA_IPv4Tx)
	{
		// RDMA(Tx) Case
		(SOCKET_VOID) close_edrdma_srv(stSocket.stpRDMA_CtrlBlock);
		(SOCKET_VOID) destroy_edrdma(stSocket.stpRDMA_CtrlBlock);
		(SOCKET_VOID) free(stSocket.stpRDMA_CtrlBlock);
	}
	
	if(stSocket.iSockType == enum_Socket_Type_RDMA_IPv4Rx)
	{
		// RDMA(Rx) Case
		// Return : Success 0, Fail -1
		(SOCKET_VOID) close_edrdma_cli(stSocket.stpRDMA_CtrlBlock);
		(SOCKET_VOID) destroy_edrdma(stSocket.stpRDMA_CtrlBlock);
		(SOCKET_VOID) free(stSocket.stpRDMA_CtrlBlock);
	}
#endif

	return iResult;
}
