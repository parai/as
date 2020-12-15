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
#include "lascanlib.h"
#include <sys/queue.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include "asdebug.h"
/* ============================ [ MACROS    ] ====================================================== */
#define CAN_BUS_NUM   4
#define CAN_BUS_PDU_NUM   16
#define CAN_BUS_Q_PDU_NUM   1024

#define AS_LOG_LUA 0
#define AS_LOG_CAN 0
/* ============================ [ TYPES     ] ====================================================== */
typedef struct {
    /* the CAN ID, 29 or 11-bit */
    uint32_t 	id;
    uint8_t     bus;
    /* Length, max 8 bytes */
    uint8_t		length;
    /* data ptr */
    uint8_t 	sdu[64];
} Can_PduType;
struct Can_Pdu_s {
	Can_PduType msg;
	STAILQ_ENTRY(Can_Pdu_s) entry;	/* entry for Can_PduQueue_s, sort by CANID queue*/
	STAILQ_ENTRY(Can_Pdu_s) entry2; /* entry for Can_Bus_s, sort by CANID queue*/
};
struct Can_PduQueue_s {
	uint32_t id;	/* can_id of this list */
	uint32_t size;
	uint32_t warning;
	STAILQ_HEAD(,Can_Pdu_s) head;
	STAILQ_ENTRY(Can_PduQueue_s) entry;
};
struct Can_Bus_s {
	uint32_t busid;
	Can_DeviceType  device;
	STAILQ_HEAD(,Can_PduQueue_s) head;	/* sort message by CANID queue */
	STAILQ_HEAD(,Can_Pdu_s) head2;	/* for all the message received by this bus */
	uint32_t                size2;

	STAILQ_HEAD(,Can_Pdu_s) headQ;	/* for all the message RX or TX by this bus with single Queue */
	uint32_t                sizeQ;
	uint32_t                warningQ;
	STAILQ_ENTRY(Can_Bus_s) entry;
};

struct Can_BusList_s {
	boolean initialized;
	pthread_mutex_t q_lock;
	STAILQ_HEAD(,Can_Bus_s) head;
};
/* ============================ [ DECLARES  ] ====================================================== */
static void logCan(boolean isRx,uint32_t busid,uint32_t canid,uint32_t dlc,uint8_t* data);
/* ============================ [ DATAS     ] ====================================================== */
static struct Can_BusList_s canbusH =
{
	.initialized=FALSE,
	.q_lock=PTHREAD_MUTEX_INITIALIZER
};
static const Can_DeviceOpsType* canOps [] =
{
	#ifdef USE_RPMSG
	&can_rpmsg_ops,
	#endif
	#ifdef USE_LUA_SERIAL_CAN
	&can_serial_ops,
	#endif
	#ifdef __WINDOWS__
	#ifdef USE_LUA_VXL_CAN
	&can_vxl_ops,
	#endif
	#ifdef USE_LUA_PEAK_CAN
	&can_peak_ops,
	#endif
	#ifdef USE_LUA_ZLG_CAN
	&can_zlg_ops,
	#endif
	#else
	#ifdef USE_LUA_SOCKET_CAN
	&can_socketwin_ops,
	#endif
	#endif
	#ifdef USE_LUA_SOCKET_CAN
	&can_socket_ops,
	#endif
	NULL
};
static FILE* canLog = NULL;
/* ============================ [ LOCALS    ] ====================================================== */
static void freeQ(struct Can_PduQueue_s* l)
{
	struct Can_Pdu_s* pdu;
	while(FALSE == STAILQ_EMPTY(&l->head))
	{
		pdu = STAILQ_FIRST(&l->head);
		STAILQ_REMOVE_HEAD(&l->head,entry);

		free(pdu);
	}
}
static void freeB(struct Can_Bus_s* b)
{
	struct Can_PduQueue_s* l;
	while(FALSE == STAILQ_EMPTY(&b->head))
	{
		l = STAILQ_FIRST(&b->head);
		STAILQ_REMOVE_HEAD(&b->head,entry);
		freeQ(l);
		free(l);
	}

}
static void freeH(struct Can_BusList_s*h)
{
	struct Can_Bus_s* b;

	pthread_mutex_lock(&h->q_lock);
	while(FALSE == STAILQ_EMPTY(&h->head))
	{
		b = STAILQ_FIRST(&h->head);
		STAILQ_REMOVE_HEAD(&h->head,entry);
		freeB(b);
		free(b);
	}
	pthread_mutex_unlock(&h->q_lock);
}
static struct Can_Bus_s* getBus(uint32_t busid)
{
	struct Can_Bus_s *handle,*h;
	handle = NULL;
	if(canbusH.initialized)
	{
		(void)pthread_mutex_lock(&canbusH.q_lock);
		STAILQ_FOREACH(h,&canbusH.head,entry)
		{
			if(h->busid == busid)
			{
				handle = h;
				break;
			}
		}
		(void)pthread_mutex_unlock(&canbusH.q_lock);
	}
	return handle;
}


static void saveQ(struct Can_Bus_s* b, uint32_t canid, uint8_t dlc, uint8_t * data)
{
	struct Can_Pdu_s* pdu;
	if(b->sizeQ > CAN_BUS_Q_PDU_NUM)
	{
		if(FALSE == b->warningQ)
		{
			b->warningQ = TRUE;
			ASWARNING(("LUA CAN BUSQ[id=%X] List is full with size %d\n", b->busid, b->sizeQ));
		}
		return;
	}
	pdu = malloc(sizeof(struct Can_Pdu_s));
	if(NULL != pdu)
	{
		pdu->msg.bus = b->busid;
		pdu->msg.id = canid;
		pdu->msg.length = dlc;
		memcpy(&(pdu->msg.sdu),data,dlc);
		(void)pthread_mutex_lock(&canbusH.q_lock);
		STAILQ_INSERT_TAIL(&b->headQ, pdu, entry);
		b->sizeQ ++;
		(void)pthread_mutex_unlock(&canbusH.q_lock);
		/* b->warningQ = FALSE; */
	}
}

static struct Can_Pdu_s* getQ(struct Can_Bus_s* b)
{
	struct Can_Pdu_s* pdu = NULL;
	(void)pthread_mutex_lock(&canbusH.q_lock);
	if((FALSE == STAILQ_EMPTY(&b->headQ)))
	{
		pdu = STAILQ_FIRST(&b->headQ);
		/* when remove, should remove from the both queue */
		STAILQ_REMOVE_HEAD(&b->headQ,entry);
		b->sizeQ --;

	}
	(void)pthread_mutex_unlock(&canbusH.q_lock);
	return pdu;
}

static struct Can_Pdu_s* getPdu(struct Can_Bus_s* b,uint32_t canid)
{
	struct Can_PduQueue_s* L=NULL;
	struct Can_Pdu_s* pdu = NULL;
	struct Can_PduQueue_s* l;

	if((uint32_t)-2 == canid)
	{
		return getQ(b);
	}

	(void)pthread_mutex_lock(&canbusH.q_lock);
	if((uint32_t)-1 == canid)
	{	/* id is -1, means get the first of queue from b->head2 */
		if(FALSE == STAILQ_EMPTY(&b->head2))
		{
			pdu = STAILQ_FIRST(&b->head2);
			/* get the first message canid, and then search CANID queue L */
			canid = pdu->msg.id;
		}
		else
		{
			/* no message all is empty */
		}
	}
	/* search queue specified by canid */
	STAILQ_FOREACH(l,&b->head,entry)
	{

		if(l->id == canid)
		{
			L = l;
			break;
		}
	}
	if(L && (FALSE == STAILQ_EMPTY(&L->head)))
	{
		pdu = STAILQ_FIRST(&L->head);
		/* when remove, should remove from the both queue */
		STAILQ_REMOVE_HEAD(&L->head,entry);
		STAILQ_REMOVE_HEAD(&b->head2,entry2);
		b->size2 --;
		L->size --;
	}
	(void)pthread_mutex_unlock(&canbusH.q_lock);
	return pdu;
}

static void saveB(struct Can_Bus_s* b,struct Can_Pdu_s* pdu)
{
	struct Can_PduQueue_s* L;
	struct Can_PduQueue_s* l;
	L = NULL;
	(void)pthread_mutex_lock(&canbusH.q_lock);
	STAILQ_FOREACH(l,&b->head,entry)
	{
		if(l->id == pdu->msg.id)
		{
			L = l;
			break;
		}
	}

	if(NULL == L)
	{
		L = malloc(sizeof(struct Can_PduQueue_s));
		if(L)
		{
			L->id = pdu->msg.id;
			L->size = 0;
			L->warning = FALSE;
			STAILQ_INIT(&L->head);
			STAILQ_INSERT_TAIL(&b->head,L,entry);
		}
		else
		{
			ASWARNING(("LUA CAN Bus List malloc failed\n"));
		}
	}

	if(L)
	{
		/* limit by CANID queue is better than the whole bus one */
		if(L->size < CAN_BUS_PDU_NUM)
		{
			STAILQ_INSERT_TAIL(&L->head,pdu,entry);
			STAILQ_INSERT_TAIL(&b->head2,pdu,entry2);
			b->size2 ++;
			L->size ++;
			L->warning = FALSE;
		}
		else
		{
			if(L->warning == FALSE)
			{
				ASWARNING(("LUA CAN Q[id=%X] List is full with size %d\n",L->id,L->size));
				L->warning = TRUE;
			}
			free(pdu);
		}
	}

	(void)pthread_mutex_unlock(&canbusH.q_lock);
}

static void rx_notification(uint32_t busid,uint32_t canid,uint32_t dlc,uint8_t* data)
{
	if((busid < CAN_BUS_NUM) && ((uint32_t)-1 != canid))
	{	/* canid -1 reserved for can_read get the first received CAN message on bus */
		struct Can_Bus_s* b = getBus(busid);
		if(NULL != b)
		{
			struct Can_Pdu_s* pdu = malloc(sizeof(struct Can_Pdu_s));
			if(pdu)
			{
				pdu->msg.bus = busid;
				pdu->msg.id = canid;
				pdu->msg.length = dlc;
				memcpy(&(pdu->msg.sdu),data,dlc);

				saveB(b,pdu);
				saveQ(b,canid,dlc,data);
				logCan(TRUE,busid,canid,dlc,data);
			}
			else
			{
				ASWARNING(("LUA CAN RX malloc failed\n"));
			}
		}
		else
		{
			/* not on-line */
			ASLOG(LUA,("lua is not on-line now!\n"));
		}
	}
	else
	{
		ASWARNING(("LUA CAN RX bus <%d> out of range, busid < %d is support only\n",busid,CAN_BUS_NUM));
	}

    ASLOG(LUA,("LUA RX CAN ID=0x%08X LEN=%d DATA=[%02X %02X %02X %02X %02X %02X %02X %02X]\n",
		  canid,dlc,data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]));
}
static const Can_DeviceOpsType* search_ops(const char* name)
{
	const Can_DeviceOpsType *ops,**o;
	o = canOps;
	ops = NULL;
	while(*o != NULL)
	{
		if(0 == strcmp((*o)->name,name))
		{
			ops = *o;
			break;
		}
		o++;
	}

	return ops;
}
static void logCan(boolean isRx,uint32_t busid,uint32_t canid,uint32_t dlc,uint8_t* data)
{
	static struct timeval m0 = { -1 , -1 };

	if( (-1 == m0.tv_sec) && (-1 == m0.tv_usec) )
	{
		gettimeofday(&m0,NULL);
	}
	if(NULL != canLog)
	{
		uint32_t i;
		struct timeval m1;
		gettimeofday(&m1,NULL);

		float rtim = m1.tv_sec-m0.tv_sec;

		if(m1.tv_usec > m0.tv_usec)
		{
			rtim += (float)(m1.tv_usec-m0.tv_usec)/1000000.0;
		}
		else
		{
			rtim = rtim - 1 + (float)(1000000.0+m1.tv_usec-m0.tv_usec)/1000000.0;
		}
		/* gettimeofday(&m0,NULL); */ /* use absolute time */
		fprintf(canLog,"busid=%d %s canid=%04X dlc=%d data=[",busid,isRx?"rx":"tx",canid,dlc);
		if(dlc < 8)
		{
			dlc = 8;
		}
		for(i=0; i<dlc; i++)
		{
			fprintf(canLog,"%02X,",data[i]);
		}

		fprintf(canLog,"] [");

		for(i=0; i<dlc; i++)
		{
			if(isprint(data[i]))
			{
				fprintf(canLog,"%c",data[i]);
			}
			else
			{
				fprintf(canLog,".");
			}
		}
		fprintf(canLog,"] @ %f s\n", rtim);
	}
}
/* ============================ [ FUNCTIONS ] ====================================================== */
#if !defined(__AS_PY_CAN__) && !defined(__AS_CAN_BUS__)
int luai_can_open  (lua_State *L)
{
	int n = lua_gettop(L);  /* number of arguments */
	if(4==n)
	{
		uint32_t busid,port,baudrate;
		const char* device_name;
		const Can_DeviceOpsType* ops;
		size_t ls;
		int is_num;

		busid = lua_tounsignedx(L, 1,&is_num);
		if((!is_num) || (busid >= CAN_BUS_NUM))
		{
			 return luaL_error(L,"incorrect argument busid to function 'can_open'");
		}

		device_name = lua_tolstring(L, 2, &ls);
		if((0 == ls) || (ls > CAN_DEVICE_NAME_SIZE))
		{
			 return luaL_error(L,"incorrect argument device name to function 'can_open'");
		}

		port = lua_tounsignedx(L, 3, &is_num);
		if(!is_num)
		{
			 return luaL_error(L,"incorrect argument port to function 'can_open'");
		}


		baudrate = lua_tounsignedx(L, 4, &is_num);
		if(!is_num)
		{
			 return luaL_error(L,"incorrect argument baudrate to function 'can_open'");
		}
		struct Can_Bus_s* b = getBus(busid);
		if(NULL != b)
		{
			return luaL_error(L,"can bus(%d) is already on-line 'can_open'",busid);
		}
		else
		{
			ops = search_ops(device_name);
			if(NULL != ops)
			{
				b = malloc(sizeof(struct Can_Bus_s));
				b->busid = busid;
				b->device.ops = ops;
				b->device.busid = busid;
				b->device.port = port;
				b->warningQ = FALSE;

				boolean rv = ops->probe(busid,port,baudrate,rx_notification);

				if(rv)
				{
					STAILQ_INIT(&b->head);
					STAILQ_INIT(&b->head2);
					STAILQ_INIT(&b->headQ);
					b->size2 = 0;
					b->sizeQ = 0;
					pthread_mutex_lock(&canbusH.q_lock);
					STAILQ_INSERT_TAIL(&canbusH.head,b,entry);
					pthread_mutex_unlock(&canbusH.q_lock);
					lua_pushboolean(L, TRUE);        /* result OK */
				}
				else
				{
					free(b);
					return luaL_error(L, "can_open device <%s> failed!",device_name);
				}
			}
			else
			{
				return luaL_error(L, "can_open device <%s> is not known by lua!",device_name);
			}
		}


		return 1;
	}
	else
	{
		return luaL_error(L, "can_open (bus_id,\"device name\",port,baudrate) API should has 3 arguments");
	}
}
int luai_can_write (lua_State *L)
{
	int n = lua_gettop(L);  /* number of arguments */
	if(3==n)
	{
		uint32 busid,canid,dlc;
		uint8 data[64]; /*64 for CANFD support */
		int is_num;

		busid = lua_tounsignedx(L, 1,&is_num);
		if((!is_num) || (busid >= CAN_BUS_NUM))
		{
			 return luaL_error(L,"incorrect argument busid to function 'can_write'");
		}

		canid = lua_tounsignedx(L, 2,&is_num);
		if(!is_num)
		{
			 return luaL_error(L,"incorrect argument canid to function 'can_write'");
		}

		dlc = luaL_len ( L , 3 ) ;
		if(dlc > 64)
		{
			return luaL_error(L,"len(data array{})>64 to function 'can_write'");
		}
		else
		{
			size_t i = 0;
			/* Push another reference to the table on top of the stack (so we know
			 * where it is, and this function can work for negative, positive and
			 * pseudo indices
			 */
			lua_pushvalue(L, 3);
			/* stack now contains: -1 => table */
			lua_pushnil(L);
			/* stack now contains: -1 => nil; -2 => table */
			while (lua_next(L, -2))
			{
				/* stack now contains: -1 => value; -2 => key; -3 => table
				 * copy the key so that lua_tostring does not modify the original */
				lua_pushvalue(L, -2);
				/* stack now contains: -1 => key; -2 => value; -3 => key; -4 => table
				 * printf("%s => %s\n", lua_tostring(L, -1), lua_tostring(L, -2)); */
				asAssert(i<sizeof(data));
				data[i] = lua_tounsignedx(L, -2,&is_num);
				if(!is_num)
				{
					return luaL_error(L,"invalid data[%d] to function 'can_write'",i);
				}
				else
				{
					i ++;
				}
				/* pop value + copy of key, leaving original key */
				lua_pop(L, 2);
				/* stack now contains: -1 => key; -2 => table */
			}
			/* stack now contains: -1 => table (when lua_next returns 0 it pops the key
			 * but does not push anything.)
			 * Pop table */
			lua_pop(L, 1);
			/* Stack is now the same as it was on entry to this function */
		}

		struct Can_Bus_s * b = getBus(busid);
		if(NULL == b)
		{
			return luaL_error(L,"can bus(%d) is not on-line 'can_write'",busid);
		}
		else
		{
			if(b->device.ops->write)
			{
				boolean rv = b->device.ops->write(b->device.busid,b->device.port,canid,dlc,data);
				ASLOG(CAN,("can_write(bus=%d,canid=%X,dlc=%d,data=[%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X]\n",
										busid,canid,dlc,data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]));
				saveQ(b,canid,dlc,data);
				logCan(FALSE,busid,canid,dlc,data);
				if(rv)
				{
					lua_pushboolean(L, TRUE);        /* result OK */
				}
				else
				{
					return luaL_error(L, "can_write bus(%d) failed!",busid);
				}
			}
			else
			{
				return luaL_error(L,"can bus(%d) is read-only 'can_write'",busid);
			}
		}

		return 1;
	}
	else
	{
		return luaL_error(L, "can_write (bus_id,can_id,{xx,xx,xx,xx,xx,xx,xx,xx}) API should has 3 arguments");
	}
}

int luai_can_read  (lua_State *L)
{
	int n = lua_gettop(L);  /* number of arguments */
	if(2==n)
	{
		uint32_t busid;
		uint32_t canid;
		struct Can_Pdu_s* pdu;
		int is_num;

		busid = lua_tounsignedx(L, 1,&is_num);
		if(!is_num)
		{
			 return luaL_error(L,"incorrect argument busid to function 'can_read'");
		}

		canid = lua_tounsignedx(L, 2,&is_num);
		if(!is_num)
		{
			 return luaL_error(L,"incorrect argument canid to function 'can_read'");
		}
		struct Can_Bus_s* b = getBus(busid);
		if(NULL == b)
		{
			 return luaL_error(L,"bus(%d) is not on-line 'can_read'",busid);
		}
		/* if canid is -1, return any of the message received */
		pdu = getPdu(b,canid);
		if(NULL == pdu)
		{
			lua_pushboolean(L, FALSE);
			lua_pushnil(L);
			lua_pushnil(L);
		}
		else
		{
			int table_index,i;

			ASLOG(CAN,("can_read(bus=%d,canid=%X,dlc=%d,data=[%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X]\n",
									busid,pdu->msg.id,pdu->msg.length,
									pdu->msg.sdu[0],pdu->msg.sdu[1],pdu->msg.sdu[2],pdu->msg.sdu[3],
									pdu->msg.sdu[4],pdu->msg.sdu[5],pdu->msg.sdu[6],pdu->msg.sdu[7]));
			lua_pushboolean(L, TRUE);
			lua_pushinteger(L,pdu->msg.id);
			lua_newtable(L);
			table_index = lua_gettop(L);
			for(i=0; i<pdu->msg.length;i++)
			{
				lua_pushinteger(L, pdu->msg.sdu[i]);
				lua_seti(L, table_index, i+1);
			}
			free(pdu);
		}
		return 3;
	}
	else
	{
		return luaL_error(L, "can_read (bus_id, can_id) API should has 2 arguments");
	}
}

int luai_can_close  (lua_State *L)
{
	int n = lua_gettop(L);  /* number of arguments */
	if(1==n)
	{
		uint32_t busid;
		int is_num;

		busid = lua_tounsignedx(L, 1,&is_num);
		if(!is_num)
		{
			 return luaL_error(L,"incorrect argument busid to function 'can_close'");
		}

		struct Can_Bus_s* b = getBus(busid);
		if(NULL == b)
		{
			 return luaL_error(L,"bus(%d) is not on-line 'can_close'",busid);
		}

		(void)pthread_mutex_lock(&canbusH.q_lock);
		b->device.ops->close(b->device.busid,b->device.port);
		STAILQ_REMOVE(&canbusH.head,b,Can_Bus_s,entry);
		free(b);
		(void)pthread_mutex_unlock(&canbusH.q_lock);

		return 0;
	}
	else
	{
		return luaL_error(L, "can_close (bus_id) API should has 1 arguments");
	}
}
int luai_can_log  (lua_State *L)
{
	int n = lua_gettop(L);  /* number of arguments */
	if(0==n)
	{
		if(canLog != NULL)
		{
			fclose(canLog);
			canLog = NULL;
		}

		return 0;
	}
	else if(1==n)
	{
		const char* file;
		size_t ls;

		file = lua_tolstring(L, 1, &ls);
		if(0 == ls)
		{
			 return luaL_error(L,"incorrect argument file name to function 'can_log'");
		}

		if(canLog != NULL)
		{
			fclose(canLog);
			canLog = NULL;
			ASWARNING(("can_log re-log without previous one closed\n"));
		}

		time_t t = time(0);
		struct tm* lt = localtime(&t);

		canLog = fopen(file,"w+");
		if(NULL != canLog)
		{
			ASLOG(STDOUT,("can trace log to file < %s >\n\n",file));
			fprintf(canLog,"lascan log %04d-%02d-%02d %02d:%02d:%02d\n\n",lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,
					lt->tm_hour,lt->tm_min,lt->tm_sec);
		}
		else
		{
			return luaL_error(L,"'can_log' open file <%s> failed\n",file);
		}

		return 0;
	}
	else
	{
		return luaL_error(L, "can_open ( [\"file name\"]) API should has no parameter for close log file, else open!");
	}
}
#endif /* __AS_PY_CAN__ */
void luai_canlib_open(void)
{
	if(canbusH.initialized)
	{
		freeH(&canbusH);
	}
	canbusH.initialized = TRUE;
	STAILQ_INIT(&canbusH.head);

	canLog=NULL;
}
void luai_canlib_close(void)
{
	if(canbusH.initialized)
	{
		struct Can_Bus_s* b;
		STAILQ_FOREACH(b,&canbusH.head,entry)
		{
			b->device.ops->close(b->device.busid,b->device.port);
		}
		freeH(&canbusH);
		canbusH.initialized = FALSE;
	}

	if(NULL != canLog)
	{
		fclose(canLog);
	}
}
#if defined(__AS_PY_CAN__) || defined(__AS_CAN_BUS__)
int can_open(unsigned long busid,const char* device_name,unsigned long port, unsigned long baudrate)
{
	int rv;
	const Can_DeviceOpsType* ops;
	ops = search_ops(device_name);
	struct Can_Bus_s* b = getBus(busid);
	rv = FALSE;
	if(NULL != b)
	{
		printf("ERROR :: can bus(%d) is already on-line 'can_open'\n",(int)busid);
	}
	else
	{
		if(NULL != ops)
		{
			b = malloc(sizeof(struct Can_Bus_s));
			b->busid = busid;
			b->device.ops = ops;
			b->device.busid = busid;
			b->device.port = port;

			rv = ops->probe(busid,port,baudrate,rx_notification);

			if(rv)
			{
				STAILQ_INIT(&b->head);
				STAILQ_INIT(&b->head2);
				STAILQ_INIT(&b->headQ);
				b->size2 = 0;
				b->sizeQ = 0;
				pthread_mutex_lock(&canbusH.q_lock);
				STAILQ_INSERT_TAIL(&canbusH.head,b,entry);
				pthread_mutex_unlock(&canbusH.q_lock);
				/* result OK */
			}
			else
			{
				free(b);
				printf("ERROR :: can_open device <%s> failed!\n",device_name);
			}
		}
		else
		{
			printf("ERROR :: can_open device <%s> is not known by lua!\n",device_name);
		}
	}

	fflush(stdout);
	return rv;
}

int can_write(unsigned long busid,unsigned long canid,unsigned long dlc,unsigned char* data)
{
	int rv;
	struct Can_Bus_s * b = getBus(busid);
	rv = FALSE;
	if(NULL == b)
	{
		printf("ERROR :: can bus(%d) is not on-line 'can_write'\n",(int)busid);
	}
	else if(dlc > 64)
	{
		printf("ERROR :: can bus(%d) 'can_write' with invalid dlc(%d>8)\n",(int)busid,(int)dlc);
	}
	else
	{
		if(b->device.ops->write)
		{
			/*printf("can_write(bus=%d,canid=%X,dlc=%d,data=[%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X]\n",
			  busid,canid,dlc,data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]);*/
			rv = b->device.ops->write(b->device.busid,b->device.port,canid,dlc,data);
			saveQ(b,canid,dlc,data);
			if(rv)
			{
				/* result OK */
			}
			else
			{
				printf("ERROR :: can_write bus(%d) failed!\n",(int)busid);
			}
		}
		else
		{
			printf("ERROR :: can bus(%d) is read-only 'can_write'\n",(int)busid);
		}
	}
	fflush(stdout);
	return rv;
}
#if defined(__AS_CAN_BUS__)
int can_read(unsigned long busid,unsigned long canid,unsigned long* p_canid,unsigned long *dlc,unsigned char* data)
#else
int can_read(unsigned long busid,unsigned long canid,unsigned long* p_canid,unsigned long *dlc,unsigned char** data)
#endif
{
	int rv = FALSE;
	struct Can_Pdu_s* pdu;
	struct Can_Bus_s* b = getBus(busid);

	*dlc = 0;
#if defined(__AS_CAN_BUS__)
#else
	*data = NULL;
#endif

	if(NULL == b)
	{
		printf("ERROR :: bus(%d) is not on-line 'can_read'\n",(int)busid);
	}
	else
	{
		pdu = getPdu(b,canid);
		if(NULL == pdu)
		{
			/* no data */
		}
		else
		{
			*p_canid = pdu->msg.id;
			*dlc = pdu->msg.length;
			#if defined(__AS_CAN_BUS__)
			asAssert(data);
			memcpy(data,pdu->msg.sdu,*dlc);
			#else
			*data = malloc(*dlc);
			asAssert(*data);
			memcpy(*data,pdu->msg.sdu,*dlc);
			#endif
			free(pdu);
			rv = TRUE;
		}
	}

	fflush(stdout);
	return rv;
}

int can_close(unsigned long busid)
{
	int rv;
	struct Can_Bus_s * b = getBus(busid);
	rv = FALSE;
	if(NULL == b)
	{
		printf("ERROR :: can bus(%d) is not on-line 'can_close'\n",(int)busid);
	}
	else
	{
		(void)pthread_mutex_lock(&canbusH.q_lock);
		b->device.ops->close(b->device.busid,b->device.port);
		STAILQ_REMOVE(&canbusH.head,b,Can_Bus_s,entry);
		free(b);
		(void)pthread_mutex_unlock(&canbusH.q_lock);
		rv = TRUE;
	}

	return rv;
}
int can_reset(unsigned long busid)
{
	int rv;
	struct Can_Bus_s * b = getBus(busid);
	rv = FALSE;
	if(NULL == b)
	{
		printf("ERROR :: can bus(%d) is not on-line 'can_reset'\n",(int)busid);
	}
	else
	{
		if(NULL != b->device.ops->reset)
		{
			rv = b->device.ops->reset(b->device.busid,b->device.port);
		}
		else
		{
			rv = TRUE;
		}
	}

	return rv;
}
#endif /* __AS_PY_CAN__ */
