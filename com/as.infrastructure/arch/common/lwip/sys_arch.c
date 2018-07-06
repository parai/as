/**
 * AS - the open source Automotive Software on https://github.com/parai
 *
 * Copyright (C) 2016  AS <parai@foxmail.com>
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
#include "lwip/opt.h"
#include "lwip/arch.h"
#include "lwip/stats.h"
#include "lwip/debug.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/tcpip.h"
#include "netif/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/inet.h"
#include "lwip_handler.h"
#include "ethernetif.h"
#include "lwip/ip_addr.h"

#include "Os.h"
#include "mbox.h"
#include "asdebug.h"
#include <unistd.h>
/* ============================ [ MACROS    ] ====================================================== */
#define AS_LOG_LWIP 1

#ifndef USE_LWIP_POSIX_ARCH
/* This is the number of threads that can be started with sys_thread_new()
 * Cannot be modified at the moment. No need to support slip/ppp */
#define SYS_THREAD_MAX 1
#define SYS_SEM_MAX 22
#endif /* LWIP_POSIX_ARCH */
/* ============================ [ TYPES     ] ====================================================== */
#ifndef USE_LWIP_POSIX_ARCH
struct semlist_t
{
	sys_sem_t val;
	boolean used;
	u32_t event;
	TaskType task[SYS_SEM_MAX];
	u8_t taskIndex;
};
#endif /* LWIP_POSIX_ARCH */
/* ============================ [ DECLARES  ] ====================================================== */
#ifndef USE_LWIP_POSIX_ARCH
err_t ethernetif_input(struct netif *netif, struct pbuf *p);
err_t ethernetif_init(struct netif *netif);
#endif /* USE_LWIP_POSIX_ARCH */
extern void Eth_Isr(void);
/* ============================ [ DATAS     ] ====================================================== */
static struct netif netif;
static boolean tcpip_initialized = FALSE;
static lwip_thread_fn tcpip_thread = NULL;
#ifndef USE_LWIP_POSIX_ARCH
static u16_t nextthread = 0;
static struct semlist_t semlist[SYS_SEM_MAX];
#endif /* USE_LWIP_POSIX_ARCH */
/* ============================ [ LOCALS    ] ====================================================== */
#ifndef USE_LWIP_POSIX_ARCH
static void sys_sleep(TickType tick)
{
	TaskType tskid = -1;
	GetTaskID(&tskid);
	if(tskid != TASK_ID_tcpip_task)
	{
		ASLOG(ERROR,"LWIP Sleep API only for tcpip task\n");
		asAssert(0);
		return;
	}
	SetRelAlarm(ALARM_ID_Alarm_Lwip, tick, 0);
	WaitEvent(EVENT_MASK_SLEEP_TCPIP);
	ClearEvent(EVENT_MASK_SLEEP_TCPIP);
}
#endif /* USE_LWIP_POSIX_ARCH */
/* ============================ [ FUNCTIONS ] ====================================================== */
struct netif* sys_get_netif(void)
{
	return &netif;
}
void __weak Eth_Isr(void)
{

}
#ifndef USE_LWIP_POSIX_ARCH
/*
  This optional function does a "fast" critical region protection and returns
  the previous protection level. This function is only called during very short
  critical regions. An embedded system which supports ISR-based drivers might
  want to implement this function by disabling interrupts. Task-based systems
  might want to implement this by using a mutex or disabling tasking. This
  function should support recursive calls from the same task or interrupt. In
  other words, sys_arch_protect() could be called while already protected. In
  that case the return value indicates that it is already protected.

  sys_arch_protect() is only required if your port is supporting an operating
  system.
*/
sys_prot_t sys_arch_protect(void)
{
	return GetResource(RES_SCHEDULER);
}

/*
  This optional function does a "fast" set of critical region protection to the
  value specified by pval. See the documentation for sys_arch_protect() for
  more information. This function is only required if your port is supporting
  an operating system.
*/
void sys_arch_unprotect(sys_prot_t pval)
{
	ReleaseResource(RES_SCHEDULER);
}


/*-----------------------------------------------------------------------------------*/
//  Creates an empty mailbox.
err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
	err_t res = ERR_OK;
	if(size==0){
		size=20;
	}
	*mbox = Arc_MBoxCreate(size);

	if(mbox == NULL){
		res = ERR_MEM;
	}
	return res;
}


// TBD memory leak with invalid mbox?
int sys_mbox_valid(sys_mbox_t *mbox)
{
	return (*mbox == NULL) ? 0 : 1;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
	*mbox = NULL;
}


int sys_sem_valid(sys_sem_t *sem)
{
	return (semlist[*sem].used == FALSE) ? 0 : 1;
}


void sys_sem_set_invalid(sys_sem_t *sem)
{
	semlist[*sem].used = FALSE;
}


/*-----------------------------------------------------------------------------------*/
/*
  Deallocates a mailbox. If there are messages still present in the
  mailbox when the mailbox is deallocated, it is an indication of a
  programming error in lwIP and the developer should be notified.
*/
void sys_mbox_free(sys_mbox_t *mbox)
{
	Arc_MBoxDestroy(*mbox);
}

/*-----------------------------------------------------------------------------------*/
/* void sys_mbox_post(sys_mbox_t mbox, void *msg)

  Posts the "msg" to the mailbox. This function have to block until
  the "msg" is really posted.
  */
void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
	while(Arc_MBoxPost( *mbox, &msg ) != E_OK)
	{
		sys_sleep(1);
	}
	Schedule();
}


/* err_t sys_mbox_trypost(sys_mbox_t mbox, void *msg)

  Try to post the "msg" to the mailbox. Returns ERR_MEM if this one
  is full, else, ERR_OK if the "msg" is posted.
  */
err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
	if(Arc_MBoxPost( *mbox, &msg ) != E_OK)
	{
		return ERR_MEM;
	}
	else
	{
		return ERR_OK;
	}
}

/* This is similar to sys_arch_mbox_fetch, however if a message is not
  present in the mailbox, it immediately returns with the code
  SYS_MBOX_EMPTY. On success 0 is returned.

  To allow for efficient implementations, this can be defined as a
  function-like macro in sys_arch.h instead of a normal function. For
  example, a naive implementation could be:
    #define sys_arch_mbox_tryfetch(mbox,msg) \
      sys_arch_mbox_fetch(mbox,msg,1)
  although this would introduce unnecessary delays. */
u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
	void *dummyptr;

	if( msg == NULL )
	{
		msg = &dummyptr;
	}

	if( Arc_MBoxFetch( *mbox, msg) != E_OK )
	{
		return SYS_MBOX_EMPTY;
	}

	return 0;
}

/*-----------------------------------------------------------------------------------*/
/*
  Blocks the thread until a message arrives in the mailbox, but does
  not block the thread longer than "timeout" milliseconds (similar to
  the sys_arch_sem_wait() function). The "msg" argument is a result
  parameter that is set by the function (i.e., by doing "*msg =
  ptr"). The "msg" parameter maybe NULL to indicate that the message
  should be dropped.

  The return values are the same as for the sys_arch_sem_wait() function:
  Number of milliseconds spent waiting or SYS_ARCH_TIMEOUT if there was a
  timeout.

  Note that a function with a similar name, sys_mbox_fetch(), is
  implemented by lwIP.
*/
u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
	void *dummyptr;
	TickType StartTime, EndTime, Elapsed;

	StartTime = GetOsTick();

	if( msg == NULL )
	{
		msg = &dummyptr;
	}

	if(	timeout != 0 )
	{
		while( Arc_MBoxFetch( *mbox, msg) != E_OK )
		{
			sys_sleep(1);
			EndTime = GetOsTick();
			Elapsed = EndTime - StartTime;
			if(Elapsed > timeout)
			{
				*msg = NULL;
				return SYS_ARCH_TIMEOUT;
			}
		}
		EndTime = GetOsTick();
		Elapsed = EndTime - StartTime;
		if( Elapsed == 0 )
		{
			Elapsed = 1;
		}
		return (Elapsed); // return time blocked TBD test
	}
	else // block forever for a message.
	{
		while( E_OK != Arc_MBoxFetch( *mbox, msg) )
		{
			sys_sleep(1);
		}
		EndTime = GetOsTick();
		Elapsed = EndTime - StartTime;
		if( Elapsed == 0 )
		{
			Elapsed = 1;
		}
		return ( Elapsed ); // return time blocked TBD test
	}
}

uint8 GetSem(sys_sem_t sem)
{
	uint8 res = E_OK;
	sys_prot_t val = sys_arch_protect();

	if(semlist[sem].val > 0){
		res = E_NOT_OK;
	}else{
		semlist[sem].val = 1;
	}

	sys_arch_unprotect(val);
	return res;
}

/*-----------------------------------------------------------------------------------*/
//  Creates and returns a new semaphore. The "count" argument specifies
//  the initial state of the semaphore. TBD finish and test
err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
	uint8 i;

	sys_prot_t val = sys_arch_protect();

	// use semaphore pool
	for(i = 0; i < SYS_SEM_MAX; i++)
	{
		if(semlist[i].used == FALSE)
		{
			semlist[i].used = TRUE;
			if(count==0)
			{
				if(GetSem( i) != E_OK)
				{
                    sys_arch_unprotect(val);
                    ErrorHook(E_OS_LIMIT);
					return ERR_MEM;
				}
			}
			sys_arch_unprotect(val);
			*sem = i;
			return ERR_OK;
		}
    }
    ErrorHook(E_OS_LIMIT);
	sys_arch_unprotect(val);
	return ERR_MEM;
}

/*-----------------------------------------------------------------------------------*/
/*
  Blocks the thread while waiting for the semaphore to be
  signaled. If the "timeout" argument is non-zero, the thread should
  only be blocked for the specified time (measured in
  milliseconds).

  If the timeout argument is non-zero, the return value is the number of
  milliseconds spent waiting for the semaphore to be signaled. If the
  semaphore wasn't signaled within the specified time, the return value is
  SYS_ARCH_TIMEOUT. If the thread didn't have to wait for the semaphore
  (i.e., it was already signaled), the function may return zero.

  Notice that lwIP implements a function with a similar name,
  sys_sem_wait(), that uses the sys_arch_sem_wait() function.
*/
u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
	TickType StartTime, EndTime, Elapsed;

	StartTime = GetOsTick();

	if(	timeout != 0)
	{
		while( GetSem( *sem) != E_OK )
		{
			sys_sleep(1);
			EndTime = GetOsTick();
			Elapsed = EndTime - StartTime;
			if(Elapsed > timeout)
			{
				return SYS_ARCH_TIMEOUT;
			}
		}
		EndTime = GetOsTick();
		Elapsed = EndTime - StartTime;
		if( Elapsed == 0 )
		{
			Elapsed = 1;
		}
		return (Elapsed); // return time blocked TBD test
	}
	else // must block without a timeout
	{
		while( GetSem( *sem) != E_OK )
		{
			TaskType TaskID;
        	sys_prot_t val = sys_arch_protect();
            GetTaskID(&TaskID);
			semlist[*sem].task[semlist[*sem].taskIndex++] = TaskID;
			sys_arch_unprotect(val);
			WaitEvent(semlist[*sem].event);
			ClearEvent(semlist[*sem].event);
		}
		EndTime = GetOsTick();
		Elapsed = EndTime - StartTime;
		if( Elapsed == 0 )
		{
			Elapsed = 1;
		}

		return ( Elapsed ); // return time blocked

	}
}

/*-----------------------------------------------------------------------------------*/
// Signals a semaphore
void
sys_sem_signal(sys_sem_t *sem)
{
	sys_prot_t val = sys_arch_protect();


	if(semlist[*sem].val > 0){
		semlist[*sem].val = 0;

		for(int i=0;i<semlist[*sem].taskIndex;i++){
			SetEvent(semlist[*sem].task[i] , semlist[*sem].event);
		}
        semlist[*sem].taskIndex = 0;
	}

	sys_arch_unprotect(val);
}

/*-----------------------------------------------------------------------------------*/
// Deallocates a semaphore
void
sys_sem_free(sys_sem_t *sem)
{
	sys_prot_t val = sys_arch_protect();

	if(semlist[*sem].used == TRUE)
	{
		sys_sem_signal(sem); // Just to be sure its released
		semlist[*sem].used = FALSE;
		semlist[*sem].taskIndex = 0;
		sys_arch_unprotect(val);
		return;
    }
    ErrorHook(E_OS_VALUE);
	sys_arch_unprotect(val);
	return;

}

/*-----------------------------------------------------------------------------------*/
// Initialize sys arch
void sys_init(void)
{

}
// Preinitialize sys arch
void pre_sys_init(void)
{
	uint8 i;

	//initialize semaphore pool
	for(i = 0; i < SYS_SEM_MAX; i++)
	{
		semlist[i].used = FALSE;
		semlist[i].val = 0;
		semlist[i].taskIndex = 0;

		/* event Mask by bits */
		semlist[i].event = 1<<i;
	}

	// keep track of how many threads have been created
	nextthread = 0;
}
#else
void pre_sys_init(void){}
#endif /* USE_LWIP_POSIX_ARCH */

#ifdef USE_LWIP
TASK(TaskLwip)
{
	OS_TASK_BEGIN();
#ifndef USE_LWIP_POSIX_ARCH
#ifdef USE_LWIP
	for(;;) {
		WaitEvent(EVENT_MASK_START_TCPIP);
		ClearEvent(EVENT_MASK_START_TCPIP);
		if(tcpip_thread != NULL)
		{
			ASLOG(LWIP,"TaskLwip is running\n");
			tcpip_thread(NULL); // Will never return
		}
	}
#endif
#endif /* USE_LWIP_POSIX_ARCH */
	OsTerminateTask(TaskLwip);

	OS_TASK_END();
}

ALARM(Alarm_Lwip)
{
#ifdef USE_LWIP
#ifdef __WINDOWS__
	SetEvent2(TASK_ID_TaskLwip,EVENT_MASK_SLEEP_TCPIP);
#else
	SetEvent(TASK_ID_TaskLwip,EVENT_MASK_SLEEP_TCPIP);
#endif
#endif
}
#endif
#ifndef USE_LWIP_POSIX_ARCH
sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
		void *arg, int stacksize, int prio)
{
	if( NULL == tcpip_thread )
	{
		tcpip_thread = thread;
	}
	else
	{
		// We only support creating tcpip thread at the moment
        ErrorHook(E_OS_LIMIT);
		return 0;
	}

	SetEvent(TASK_ID_tcpip_task,EVENT_MASK_START_TCPIP); /* Start tcpip stack */

	return TASK_ID_tcpip_task;
}
#endif /* USE_LWIP_POSIX_ARCH */

static void
tcpip_init_done(void *arg)
{
	tcpip_initialized = TRUE;
}
void __weak Ethernet_Configuration(void) {}
void __weak Set_MAC_Address(uint8_t *macaddress) {}
void __weak EnableEthDMAIrq(void) {}
#ifndef __WINDOWS__
struct netif * __weak LwIP_Init(void)
#else
struct netif * LwIP_Init(void)
#endif
{
#ifdef USE_LWIP
	uint8_t macaddress[6] = ETH_MAC_ADDR;
	ip_addr_t ipaddr;
	ip_addr_t netmask;
	ip_addr_t gw;

	/* Configure ethernet */
	Ethernet_Configuration();

#if NO_SYS
#if (MEM_LIBC_MALLOC==0)
	mem_init();
#endif
#if (MEMP_MEM_MALLOC==0)
	memp_init();
#endif
#else
	pre_sys_init();
	tcpip_init(tcpip_init_done, NULL);
	uint32 lockcnt = 0;
	while(tcpip_initialized == FALSE){
		lockcnt++;
#ifdef USE_LWIP_POSIX_ARCH
		usleep(1000);
#endif
	};
#endif

#if LWIP_DHCP
	ipaddr.addr = 0;
	netmask.addr = 0;
	gw.addr = 0;
#else
	GET_BOOT_IPADDR;
	GET_BOOT_NETMASK;
	GET_BOOT_GW;
#endif

	Set_MAC_Address(macaddress);

	/* Add network interface to the netif_list */
#ifdef USE_PCAPIF
	extern err_t pcapif_init(struct netif *netif);
	netif_add(&netif, &ipaddr, &netmask, &gw, NULL, &pcapif_init, &tcpip_input);
#else
	extern err_t tapif_init(struct netif *netif);
	netif_add(&netif, &ipaddr, &netmask, &gw, NULL, &tapif_init, &tcpip_input);
#endif
	/*  Registers the default network interface.*/
	netif_set_default(&netif);

	#if LWIP_DHCP
	/* start dhcp search */
	dhcp_start(&netif);
	#else
	netif_set_addr(&netif, &ipaddr , &netmask, &gw);
	#endif

	/* netif is configured */
	netif_set_up(&netif);

	EnableEthDMAIrq();
	extern void netbios_init(void);
	netbios_init();
#ifdef USE_LWIP_POSIX_ARCH
	extern void http_server_netconn_init(void);
	http_server_netconn_init();
#endif
	return &netif;
#endif /* USE_LWIP */
}
#ifdef USE_LWIP
KSM(LwipIdle,Init)
{
#ifdef USE_LWIP_POSIX_ARCH
	printf("!!!LWIP run on pthreads!!!\n");
#else
	printf("!!!LWIP run on OSEK OS!!!\n");
#endif
	KGS(LwipIdle,Running);
}

KSM(LwipIdle,Start)
{

}

KSM(LwipIdle,Stop)
{

}

KSM(LwipIdle,Running)
{
#ifdef USE_LWIP
	Eth_Isr();
#endif
}
#endif
