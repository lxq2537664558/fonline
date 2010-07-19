#include "StdAfx.h"
#include "Server.h"

void* zlib_alloc(void *opaque, unsigned int items, unsigned int size){return calloc(items, size);}
void zlib_free(void *opaque, void *address){free(address);}

#ifndef SERVER_LITE
#define MAX_CLIENTS_IN_GAME		(3000)
#else
#define MAX_CLIENTS_IN_GAME		(5)
#endif

#ifdef FOSERVER_DUMP
HANDLE hDump;
#endif

FOServer* FOServer::Self=NULL;
int FOServer::UpdateIndex=-1;
int FOServer::UpdateLastIndex=-1;
DWORD  FOServer::UpdateLastTick=0;
ClVec FOServer::LogClients;
HANDLE FOServer::IOCompletionPort=NULL;
HANDLE* FOServer::IOThreadHandles=NULL;
DWORD FOServer::WorkThreadCount=0;
SOCKET FOServer::ListenSock=INVALID_SOCKET;
ClVec FOServer::ConnectedClients;
CRITICAL_SECTION FOServer::CSConnectedClients;
FOServer::Statistics_ FOServer::Statistics;
FOServer::ClientSaveDataVec FOServer::ClientsSaveData;
size_t FOServer::ClientsSaveDataCount=0;
PByteVec FOServer::WorldSaveData;
size_t FOServer::WorldSaveDataBufCount=0;
size_t FOServer::WorldSaveDataBufFreeSize=0;
FILE* FOServer::DumpFile=NULL;
HANDLE FOServer::DumpBeginEvent=NULL;
HANDLE FOServer::DumpEndEvent=NULL;
DWORD FOServer::SaveWorldIndex=0;
DWORD FOServer::SaveWorldTime=0;
DWORD FOServer::SaveWorldNextTick=0;
DwordVec FOServer::SaveWorldDeleteIndexes;
HANDLE FOServer::DumpThreadHandle=NULL;

FOServer::FOServer()
{
	Active=0;
	ShowUIDError=false;
	CycleBeginTick=0;
	ZeroMemory((void*)&Statistics,sizeof(Statistics));
	ZeroMemory(&ServerFunctions,sizeof(ServerFunctions));
	ZeroMemory(RadioChannels,sizeof(RadioChannels));
	ServerWindow=NULL;
	LastClientId=0;
	Self=this;
	MEMORY_PROCESS(MEMORY_STATIC,sizeof(FOServer));
}

FOServer::~FOServer()
{
#ifdef FOSERVER_DUMP
	CloseHandle(hDump);
#endif
	Self=NULL;
}

void FOServer::Finish()
{
	if(!Active) return;

	// World dumper
	if(WorldSaveManager)
	{
		WaitForSingleObject(DumpEndEvent,INFINITE);
		MEMORY_PROCESS(MEMORY_SAVE_DATA,-(int)ClientsSaveData.size()*sizeof(ClientSaveData));
		ClientsSaveData.clear();
		ClientsSaveDataCount=0;
		MEMORY_PROCESS(MEMORY_SAVE_DATA,-(int)WorldSaveData.size()*WORLD_SAVE_DATA_BUFFER_SIZE);
		for(size_t i=0;i<WorldSaveData.size();i++) delete[] WorldSaveData[i];
		WorldSaveData.clear();
		WorldSaveDataBufCount=0;
		WorldSaveDataBufFreeSize=0;
		CloseHandle(DumpBeginEvent);
		DumpBeginEvent=NULL;
		CloseHandle(DumpEndEvent);
		DumpEndEvent=NULL;
		//WaitForSingleObject(DumpThreadHandle,INFINITE);
		CloseHandle(DumpThreadHandle);
		DumpThreadHandle=NULL;
	}
	if(DumpFile) fclose(DumpFile);
	DumpFile=NULL;
	SaveWorldIndex=0;
	SaveWorldTime=0;
	SaveWorldNextTick=0;

	// End script
	if(Script::PrepareContext(ServerFunctions.Finish,CALL_FUNC_STR,"Game")) Script::RunPrepared();

	// Logging clients
	LogFinish(LOG_FUNC);
	for(ClVecIt it=LogClients.begin(),end=LogClients.end();it!=end;++it) (*it)->Release();
	LogClients.clear();

	// Net
	EnterCriticalSection(&CSConnectedClients);
	for(ClVecIt it=ConnectedClients.begin(),end=ConnectedClients.end();it!=end;++it)
	{
		Client* cl=*it;
		cl->Shutdown();
	}
	LeaveCriticalSection(&CSConnectedClients);
	closesocket(ListenSock);
	ListenSock=INVALID_SOCKET;
	for(DWORD i=0;i<WorkThreadCount;i++) PostQueuedCompletionStatus(IOCompletionPort,0,1,NULL);
	WaitForMultipleObjects(WorkThreadCount+1,IOThreadHandles,TRUE,INFINITE);
	for(DWORD i=0;i<WorkThreadCount;i++) CloseHandle(IOThreadHandles[i]);
	SAFEDELA(IOThreadHandles);
	WorkThreadCount=0;
	CloseHandle(IOCompletionPort);
	IOCompletionPort=NULL;
	DeleteCriticalSection(&CSConnectedClients);

	// Managers
	AIMngr.Finish();
	MapMngr.Finish();
	CrMngr.Finish();
	ItemMngr.Finish();
	DlgMngr.Finish();
	VarMngr.Finish();
	FinishScriptSystem();
	FinishLangPacks();
	RadioClearChannels();
	FileManager::EndOfWork();
	Active=false;
	WriteLog("Server stopped.\n");
	WriteLog("Statistics:\n");
	WriteLog("Traffic:\n"
		"Bytes Send:%u\n"
		"Bytes Recv:%u\n",
		Statistics.BytesSend,
		Statistics.BytesRecv);
	WriteLog("Cycles count:%u\n"
		"Approx cycle period:%u\n"
		"Min cycle period:%u\n"
		"Max cycle period:%u\n"
		"Count of lags (>100ms):%u\n",
		Statistics.LoopCycles,
		Statistics.LoopTime/Statistics.LoopCycles,
		Statistics.LoopMin,
		Statistics.LoopMax,
		Statistics.LagsCount);
}

string FOServer::GetIngamePlayersStatistics()
{
	static string result;
	const char* net_states_str[]={"None","Disconnect","Connect","Game","LoginOk","InitNet","Remove"};
	const char* cond_states_str[]={"None","Life","Knockout","Dead"};
	char str[512];
	char str_loc[256];
	char str_map[256];

	EnterCriticalSection(&CSConnectedClients);
	DWORD conn_count=ConnectedClients.size();
	LeaveCriticalSection(&CSConnectedClients);

	ClVec players;
	CrMngr.GetCopyPlayers(players);

	sprintf(str,"Players in game: %u\nConnections: %u\n",players.size(),conn_count);
	result=str;
	result+="Name                 Id        Ip              NetState   Cond     X     Y     Location (Id, Pid)             Map (Id, Pid)                  Level\n";
	for(size_t i=0,j=players.size();i<j;i++)
	{
		Client* cl=players[i];
		const char* name=cl->GetName();

		Map* map=MapMngr.GetMap(cl->GetMap());
		sprintf(str_loc,"%s (%u, %u)",map?map->MapLocation->Proto->Name.c_str():"",map?map->MapLocation->GetId():0,map?map->MapLocation->GetPid():0);
		sprintf(str_map,"%s (%u, %u)",map?map->Proto->GetName():"",map?map->GetId():0,map?map->GetPid():0);
		sprintf(str,"%-20s %-9u %-15s %-10s %-8s %-5u %-5u %-30s %-30s %-4d\n",
			cl->GetName(),cl->GetId(),cl->GetIpStr(),net_states_str[InterlockedCompareExchange(&cl->NetState,0,0)],cond_states_str[cl->Data.Cond],
			map?cl->GetHexX():cl->Data.WorldX,map?cl->GetHexY():cl->Data.WorldY,map?str_loc:"Global map",map?str_map:"",cl->GetParam(ST_LEVEL));
		result+=str;
	}
	return result;
}

void FOServer::DisconnectClient(Client* cl)
{
	if(cl->IsDisconnected) return;
	cl->IsDisconnected=true;
	if(InterlockedCompareExchange(&cl->WSAOut->Operation,0,0)==WSAOP_FREE)
	{
		cl->Bout.Lock();
		bool is_empty=cl->Bout.IsEmpty();
		cl->Bout.Unlock();
		if(is_empty) cl->Shutdown();
	}

	DWORD id=cl->GetId();
	Client* cl_=(id?CrMngr.GetPlayer(id):NULL);
	if(cl_ && cl_==cl)
	{
		cl->Disconnect(); // Refresh disconnectTick
		EraseSaveClient(cl->GetId());
		cl->AddRef();
		SaveClients.push_back(cl);

		SETFLAG(cl->Flags,FCRIT_DISCONNECT);
		if(cl->GetMap())
		{
			cl->SendA_Action(ACTION_DISCONNECT,0,NULL);
		}
		else if(cl->GroupMove)
		{
			for(CrVecIt it=cl->GroupMove->CritMove.begin(),end=cl->GroupMove->CritMove.end();it!=end;++it)
				(*it)->Send_CritterParam(cl,OTHER_FLAGS,cl->Flags);
		}
	}
}

void FOServer::RemoveClient(Client* cl)
{
	DWORD id=cl->GetId();
	Client* cl_=(id?CrMngr.GetPlayer(id):NULL);
	if(cl_ && cl_==cl)
	{
		cl->EventFinish(cl->Data.ClientToDelete);
		if(Script::PrepareContext(ServerFunctions.CritterFinish,CALL_FUNC_STR,cl->GetInfo()))
		{
			Script::SetArgObject(cl);
			Script::SetArgBool(cl->Data.ClientToDelete);
			Script::RunPrepared();
		}

		if(cl->GetMap())
		{
			Map* map=MapMngr.GetMap(cl->GetMap());
			if(map) MapMngr.EraseCrFromMap(cl,map,cl->GetHexX(),cl->GetHexY());
		}
		else if(cl->GroupMove)
		{
			GlobalMapGroup* group=cl->GroupMove;
			group->EraseCrit(cl);
			if(cl==group->Rule)
			{
				for(CrVecIt it=group->CritMove.begin(),end=group->CritMove.end();it!=end;++it)
				{
					Critter* cr=*it;
					MapMngr.GM_GroupStartMove(cr,true);
				}
			}
			else
			{
				for(CrVecIt it=group->CritMove.begin(),end=group->CritMove.end();it!=end;++it)
				{
					Critter* cr=*it;
					cr->Send_RemoveCritter(cl);
				}

				Item* car=cl->GetItemCar();
				if(car && car->GetId()==group->CarId)
				{
					group->CarId=0;
					MapMngr.GM_GroupSetMove(group,group->MoveX,group->MoveY,0); // Stop others
				}
			}
			cl->GroupMove=NULL;
		}

		// Deffered saving
		EraseSaveClient(cl->GetId());
		if(!cl->Data.ClientToDelete)
		{
			cl->AddRef();
			SaveClients.push_back(cl);
		}

		CrMngr.EraseCritter(cl);
		cl->Disconnect();
		cl->IsNotValid=true;
		// Full delete
		if(cl->Data.ClientToDelete)
		{
			ClientData* data=GetClientData(id);
			if(data) data->Clear();
			cl->FullClear();
		}
		cl->Release();
	}
	else
	{
		cl->Disconnect();
		cl->IsNotValid=true;
	}
}

void FOServer::EraseSaveClient(DWORD crid)
{
	for(ClVecIt it=SaveClients.begin();it!=SaveClients.end();)
	{
		Client* cl=*it;
		if(cl->GetId()==crid)
		{
			cl->Release();
			it=SaveClients.erase(it);
		}
		else ++it;
	}
}

void FOServer::RunGameLoop()
{
	if(!Active) return;

	ClVec remove_cl;
	remove_cl.reserve(MAX_CLIENTS_IN_GAME);
	ProcessClients.reserve(MAX_CLIENTS_IN_GAME);
	DWORD ticks=Timer::FastTick();

	WriteLog("***   Starting game loop   ***\n");

	Statistics.LoopTime=0;
	Statistics.LoopCycles=0;
	Statistics.LoopMin=100;
	Statistics.LoopMax=0;
	Statistics.LagsCount=0;

	while(!FOQuit)
	{
		// Pre loop
		CycleBeginTick=Timer::FastTick();

		static DWORD last_tick=Timer::FastTick();
		static WORD call_cnt=0;
		if(CycleBeginTick-last_tick>=1000)
		{
			Statistics.FPS=call_cnt;
			call_cnt=0;
			last_tick=CycleBeginTick;
		}
		else call_cnt++;

		// Get connected clients
#ifndef FONLINE_SINGLE
		EnterCriticalSection(&CSConnectedClients);
		Statistics.CurOnline=ConnectedClients.size();
		ProcessClients.clear();
		for(ClVecIt it=ConnectedClients.begin();it!=ConnectedClients.end();)
		{
			Client* cl=*it;
			if(InterlockedCompareExchange(&cl->WSAIn->Operation,0,0)==WSAOP_FREE && InterlockedCompareExchange(&cl->WSAOut->Operation,0,0)==WSAOP_FREE)
			{
				cl->Shutdown();
				InterlockedExchange(&cl->WSAIn->Operation,WSAOP_END);
				InterlockedExchange(&cl->WSAOut->Operation,WSAOP_END);
				cl->Release();
				it=ConnectedClients.erase(it);
			}
			else
			{
				cl->AddRef();
				ProcessClients.push_back(cl);
				++it;
			}
		}
		LeaveCriticalSection(&CSConnectedClients);
#else
		// Get Chosen
#endif

		// Connected clients process
		for(ClVecIt it=ProcessClients.begin(),end=ProcessClients.end();it!=end;++it)
		{
			ClientPtr& cl=*it;
			Process(cl);
		}

		// Critters process
		CrMap critters=CrMngr.GetCritters(); // TODO:
		remove_cl.clear();
		for(CrMapIt it=critters.begin(),end=critters.end();it!=end;++it)
		{
			Critter* cr=(*it).second;
			ProcessCritter(cr);

			if(cr->IsPlayer())
			{
				Client* cl=(Client*)cr;
				if(InterlockedCompareExchange(&cl->NetState,0,0)==STATE_REMOVE) remove_cl.push_back(cl);
			}
		}

		// Disconnect, remove
		for(ClVecIt it=remove_cl.begin(),end=remove_cl.end();it!=end;++it)
		{
			Client* cl=*it;
			RemoveClient(cl);
			ClVecIt it_=std::find(ProcessClients.begin(),ProcessClients.end(),cl);
			if(it_!=ProcessClients.end())
			{
				ProcessClients.erase(it_);
				cl->Release();
			}
		}

		for(ClVecIt it=ProcessClients.begin(),end=ProcessClients.end();it!=end;++it)
		{
			Client* cl=*it;
			if(InterlockedCompareExchange(&cl->NetState,0,0)==STATE_DISCONNECT) DisconnectClient(cl);
			cl->Release();
		}

		// Time events
		ProcessTimeEvents();

		// Process maps
		MapMngr.ProcessMaps();

		// Script game loop
		static DWORD game_loop_tick=1;
		if(game_loop_tick && Timer::FastTick()>=game_loop_tick)
		{
			DWORD wait=3600000; // 1hour
			if(Script::PrepareContext(ServerFunctions.Loop,CALL_FUNC_STR,"Game") && Script::RunPrepared()) wait=Script::GetReturnedDword();
			if(!wait) game_loop_tick=0; // Disable
			else game_loop_tick=Timer::FastTick()+wait;
		}

		// Garbage
		ItemMngr.ItemGarbager(CycleBeginTick); // Items garbage
		CrMngr.CritterGarbager(CycleBeginTick); // Critters garbage
		MapMngr.LocationGarbager(CycleBeginTick); // Locations and maps garbage
		Script::CollectGarbage(false); // AngelScript garbage
		VarsGarbarger(CycleBeginTick);

		// Other
		ProcessGameTime();
		ProcessBans();

		// Saver
		if(CycleBeginTick>=SaveWorldNextTick)
		{
			SaveWorld();
			SaveWorldNextTick=Timer::FastTick()+SaveWorldTime;
		}

		// Post loop
		DWORD loop_tick=Timer::FastTick()-CycleBeginTick;
		Statistics.LoopTime+=loop_tick;
		Statistics.LoopCycles++;
		if(loop_tick>Statistics.LoopMax) Statistics.LoopMax=loop_tick;
		if(loop_tick<Statistics.LoopMin) Statistics.LoopMin=loop_tick;
		if(loop_tick>100) Statistics.LagsCount++;
		Statistics.CycleTime=loop_tick;
		Statistics.Uptime=(Timer::FastTick()-Statistics.ServerStartTick)/1000;
		SetEvent(UpdateEvent);
		if(ServerGameSleep>=0) Sleep(ServerGameSleep);

#ifdef SERVER_LITE
		if(Timer::FastTick()-Statistics.ServerStartTick>=Random(60*60000,61*60000)) break;
#endif
	}

	WriteLog("***   Finishing game loop   ***\n");

	// Finish clients
	CrMap& critters=CrMngr.GetCritters();
	for(CrMapIt it=critters.begin(),end=critters.end();it!=end;++it)
	{
		Critter* cr=(*it).second;
		if(cr->IsPlayer())
		{
			Client* cl=(Client*)cr;
			cl->EventFinish(cl->Data.ClientToDelete);
			if(Script::PrepareContext(ServerFunctions.CritterFinish,CALL_FUNC_STR,cl->GetInfo()))
			{
				Script::SetArgObject(cl);
				Script::SetArgBool(cl->Data.ClientToDelete);
				Script::RunPrepared();
			}
			if(cl->Data.ClientToDelete) cl->FullClear();
			else SaveClient(cl,true);
		}
	}

	SaveWorld();
}

unsigned int __stdcall FOServer::Net_Listen(HANDLE iocp)
{
	while(true)
	{
		// Blocked
		SOCKADDR_IN from;
		int addrsize=sizeof(from);
		SOCKET sock=WSAAccept(ListenSock,(sockaddr*)&from,&addrsize,NULL,NULL);
		if(sock==INVALID_SOCKET)
		{
			int error=WSAGetLastError();
			if(error==WSAEINTR || error==WSAENOTSOCK) break; // End of work

			WriteLog(__FUNCTION__" - Listen error<%s>. Continue listening.\n",GetLastSocketError());
			continue;
		}

		EnterCriticalSection(&CSConnectedClients);
		DWORD count=ConnectedClients.size();
		LeaveCriticalSection(&CSConnectedClients);
		if(count>=MAX_CLIENTS_IN_GAME)
		{
			closesocket(sock);
			continue;
		}

#ifdef SERVER_LITE
		if(from.sin_addr.s_addr!=0x0100007F)
		{
			closesocket(sock);
			continue;
		}
#endif

		if(GameOpt.DisableTcpNagle)
		{
			int optval=1;
			if(setsockopt(sock,IPPROTO_TCP,TCP_NODELAY,(char*)&optval,sizeof(optval)))
				WriteLog(__FUNCTION__" - Can't set TCP_NODELAY (disable Nagle) to socket, error<%s>.\n",GetLastSocketError());
		}

		Client* cl=new Client();
		// ZLib
		cl->Zstrm.zalloc=zlib_alloc;
		cl->Zstrm.zfree=zlib_free;
		cl->Zstrm.opaque=NULL;
		if(deflateInit(&cl->Zstrm,Z_DEFAULT_COMPRESSION)!=Z_OK)
		{
			WriteLog(__FUNCTION__" - Client Zlib deflateInit fail.\n");
			closesocket(sock);
			delete cl;
			continue;
		}
		cl->ZstrmInit=true;
		// CompletionPort
		if(!CreateIoCompletionPort((HANDLE)sock,IOCompletionPort,0,0))
		{
			WriteLog(__FUNCTION__" - CreateIoCompletionPort fail, error<%u>.\n",GetLastError());
			delete cl;
			continue;
		}
		// Socket
		cl->Sock=sock;
		cl->From=from;
		// First receive queue
		if(WSARecv(cl->Sock,&cl->WSAIn->Buffer,1,NULL,&cl->WSAIn->Flags,&cl->WSAIn->OV,NULL)==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
		{
			WriteLog(__FUNCTION__" - First recv fail, error<%s>.\n",GetLastSocketError());
			closesocket(sock);
			delete cl;
			continue;
		}
		// Add to connected collection
		EnterCriticalSection(&CSConnectedClients);
		InterlockedExchange(&cl->NetState,STATE_CONN);
		ConnectedClients.push_back(cl);
		LeaveCriticalSection(&CSConnectedClients);
	}
	return 0;
}

unsigned int __stdcall FOServer::Net_Work(HANDLE iocp)
{
	while(true)
	{
		DWORD bytes;
		DWORD key;
		WSAOVERLAPPED_EX* io;
		DWORD error=ERROR_SUCCESS;
		if(!GetQueuedCompletionStatus(iocp,&bytes,(PULONG_PTR)&key,(LPOVERLAPPED*)&io,INFINITE)) error=GetLastError();
		if(key==1) break; // End of work

		if(error!=ERROR_SUCCESS && error!=ERROR_NETNAME_DELETED && error!=ERROR_CONNECTION_ABORTED && error!=ERROR_OPERATION_ABORTED && error!=ERROR_SEM_TIMEOUT)
		{
			WriteLog(__FUNCTION__" - GetQueuedCompletionStatus fail, error<%u>. Work thread closed!\n",error);
			break;
		}

		EnterCriticalSection(&io->CS);
		Client* cl=(Client*)io->Client;
		cl->AddRef();

		if(error==ERROR_SUCCESS && bytes)
		{
			switch(InterlockedCompareExchange(&io->Operation,0,0))
			{
			case WSAOP_SEND:
				Statistics.BytesSend+=bytes;
				Net_Output(io);
				break;
			case WSAOP_RECV:
				Statistics.BytesRecv+=bytes;
				io->Bytes=bytes;
				Net_Input(io);
				break;
			default:
				WriteLog(__FUNCTION__" - Unknown operation<%d>.\n",io->Operation);
				break;
			}
		}
		else
		{
			InterlockedExchange(&io->Operation,WSAOP_FREE);
		}

		LeaveCriticalSection(&io->CS);
		cl->Release();
	}
	return 0;
}

void FOServer::Net_Input(WSAOVERLAPPED_EX* io)
{
	Client* cl=(Client*)io->Client;

	cl->Bin.Lock();
	if(cl->Bin.GetCurPos()+io->Bytes>=GameOpt.FloodSize)
	{
		WriteLog(__FUNCTION__" - Flood.\n");
		cl->Disconnect();
		cl->Bin.Reset();
		cl->Bin.Unlock();
		InterlockedExchange(&io->Operation,WSAOP_FREE);
		return;
	}
	cl->Bin.Push(io->Buffer.buf,io->Bytes);
	cl->Bin.Unlock();

	io->Flags=0;
	if(WSARecv(cl->Sock,&io->Buffer,1,NULL,&io->Flags,&io->OV,NULL)==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
	{
		WriteLog(__FUNCTION__" - Recv fail, error<%s>.\n",GetLastSocketError());
		cl->Disconnect();
		InterlockedExchange(&io->Operation,WSAOP_FREE);
	}
}

void FOServer::Net_Output(WSAOVERLAPPED_EX* io)
{
	Client* cl=(Client*)io->Client;

	cl->Bout.Lock();
	if(cl->Bout.IsEmpty()) // Nothing to send
	{
		cl->Bout.Unlock();
		InterlockedExchange(&io->Operation,WSAOP_FREE);
		if(cl->IsDisconnected) cl->Shutdown();
		return;
	}

	// Compress
	if(!GameOpt.DisableZlibCompression && !cl->DisableZlib)
	{
		DWORD to_compr=cl->Bout.GetEndPos();
		if(to_compr>WSA_BUF_SIZE) to_compr=WSA_BUF_SIZE;

		cl->Zstrm.next_in=(UCHAR*)cl->Bout.GetCurData();
		cl->Zstrm.avail_in=to_compr;
		cl->Zstrm.next_out=(UCHAR*)io->Buffer.buf;
		cl->Zstrm.avail_out=WSA_BUF_SIZE;

 		if(deflate(&cl->Zstrm,Z_SYNC_FLUSH)!=Z_OK)
 		{
			WriteLog(__FUNCTION__" - Deflate fail.\n");
			cl->Disconnect();
			cl->Bout.Reset();
			cl->Bout.Unlock();
			InterlockedExchange(&io->Operation,WSAOP_FREE);
			return;
		}

		DWORD compr=cl->Zstrm.next_out-(UCHAR*)io->Buffer.buf;
		DWORD real=cl->Zstrm.next_in-(UCHAR*)cl->Bout.GetCurData();
		io->Buffer.len=compr;
		cl->Bout.Pop(real);
		Statistics.DataReal+=real;
		Statistics.DataCompressed+=compr;
	}
	// Without compressing
	else
	{
		DWORD len=cl->Bout.GetEndPos();
		if(len>WSA_BUF_SIZE) len=WSA_BUF_SIZE;
		memcpy(io->Buffer.buf,cl->Bout.GetCurData(),len);
		io->Buffer.len=len;
		cl->Bout.Pop(len);
		Statistics.DataReal+=len;
		Statistics.DataCompressed+=len;
	}
	if(cl->Bout.IsEmpty()) cl->Bout.Reset();
	cl->Bout.Unlock();

	// Send
	InterlockedExchange(&io->Operation,WSAOP_SEND);
	if(WSASend(cl->Sock,&io->Buffer,1,NULL,0,(LPOVERLAPPED)io,NULL)==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
	{
		WriteLog(__FUNCTION__" - Send fail, error<%s>.\n",GetLastSocketError());
		cl->Disconnect();
		InterlockedExchange(&io->Operation,WSAOP_FREE);
	}
}

void FOServer::Process(ClientPtr& cl)
{
#define TIME_CHECKER //if(Timer::FastTick()>=tick+50) WriteLog(__FUNCTION__" - !!!Lag<%u> message<%u>.\n",Timer::FastTick()-tick,(msg>>8)&0xFF)

#ifdef SERVER_LITE
	if(cl->From.sin_addr.s_addr!=0x0100007F)
	{
		cl->Disconnect();
		return;
	}
#endif

	if(cl->IsOffline())
	{
		cl->Bin.LockReset();
		return;
	}

	MSGTYPE msg=0;
	if(InterlockedCompareExchange(&cl->NetState,0,0)==STATE_CONN) 
	{
		BIN_BEGIN(cl);
		if(cl->Bin.IsEmpty()) cl->Bin.Reset();
		cl->Bin.Refresh();
		if(cl->Bin.NeedProcess())
		{
			cl->Bin >> msg;

			DWORD tick=Timer::FastTick();
			switch(msg) 
			{
			case 0xFFFFFFFF:
				{
			 		DWORD answer[4]={CrMngr.PlayersInGame(),Statistics.Uptime,0,0};
			 		BOUT_BEGIN(cl);
					cl->Bout.Push((char*)answer,sizeof(answer));
					cl->DisableZlib=true;
					BOUT_END(cl);
			 		cl->Disconnect();
			 	}
				BIN_END(cl);
				break;
			case NETMSG_PING:
				Process_Ping(cl);TIME_CHECKER;
				BIN_END(cl);
				break;
			case NETMSG_LOGIN:
				Process_LogIn(cl);TIME_CHECKER;
				BIN_END(cl);
				break;
			case NETMSG_CREATE_CLIENT:
				Process_CreateClient(cl);TIME_CHECKER;
				BIN_END(cl);
				break;
			default:
				//WriteLog(__FUNCTION__" - Invalid msg<%u> from client<%s> on STATE_CONN. Skip.\n",msg,cl->GetInfo());
				cl->Bin.SkipMsg(msg);TIME_CHECKER;
				BIN_END(cl);
				break;
			}
		}
		else
		{
			BIN_END(cl);
		}

		if(cl->NetState==STATE_CONN && cl->ConnectTime && Timer::FastTick()-cl->ConnectTime>PING_CLIENT_LIFE_TIME) // Kick bot
		{
			WriteLog(__FUNCTION__" - Connection timeout, client kicked, maybe bot.\n");
			cl->Disconnect();
		}
	}
	else if(InterlockedCompareExchange(&cl->NetState,0,0)==STATE_LOGINOK)
	{
#define CHECK_BUSY if(cl->IsBusy()) {cl->Bin.MoveReadPos(-int(sizeof(msg))); BIN_END(cl); return;}

		BIN_BEGIN(cl);
		if(cl->Bin.IsEmpty()) cl->Bin.Reset();
		cl->Bin.Refresh();
		if(cl->Bin.NeedProcess())
		{
			cl->Bin >> msg;

			DWORD tick=Timer::FastTick();
			switch(msg) 
			{
			case NETMSG_PING:
				Process_Ping(cl);TIME_CHECKER;
				BIN_END(cl);
				break;
			case NETMSG_SEND_GIVE_MAP:
				CHECK_BUSY;
				Process_GiveMap(cl);TIME_CHECKER;
				BIN_END(cl);
				break;
			case NETMSG_SEND_LOAD_MAP_OK:
				CHECK_BUSY;
				Process_ParseToGame(cl);TIME_CHECKER;
				BIN_END(cl);
				break;
			default:
				//if(msg<NETMSG_SEND_MOVE && msg>NETMSG_CRITTER_XY) WriteLog(__FUNCTION__" - Invalid msg<%u> from client<%s> on STATE_LOGINOK. Skip.\n",msg,cl->GetInfo());
				cl->Bin.SkipMsg(msg);TIME_CHECKER;
				BIN_END(cl);
				break;
			}
		}
		else
		{
			BIN_END(cl);
		}
	}
	else if(InterlockedCompareExchange(&cl->NetState,0,0)==STATE_GAME)
	{
#define MESSAGES_PER_CYCLE         (5)
#define CHECK_BUSY_AND_LIFE if(!cl->IsLife()) break; if(cl->IsBusy()) {cl->Bin.MoveReadPos(-int(sizeof(msg))); BIN_END(cl); return;}
#define CHECK_NO_GLOBAL if(!cl->GetMap()) break;
#define CHECK_IS_GLOBAL if(cl->GetMap() || !cl->GroupMove) break;
#define CHECK_AP_MSG BYTE ap; cl->Bin >> ap; if(!cl->IsTurnBased()){if(ap>cl->GetMaxAp()) break; if((int)ap>cl->GetAp()){cl->Bin.MoveReadPos(-int(sizeof(msg)+sizeof(ap))); BIN_END(cl); return;}}
#define CHECK_AP(ap) if(!cl->IsTurnBased()){if((int)(ap)>cl->GetMaxAp()) break; if((int)(ap)>cl->GetAp()){cl->Bin.MoveReadPos(-int(sizeof(msg))); BIN_END(cl); return;}}
#define CHECK_REAL_AP(ap) if(!cl->IsTurnBased()){if((int)(ap)>cl->GetMaxAp()*AP_DIVIDER) break; if((int)(ap)>cl->GetRealAp()){cl->Bin.MoveReadPos(-int(sizeof(msg))); BIN_END(cl); return;}}

		for(int i=0;i<MESSAGES_PER_CYCLE;i++)
		{
			BIN_BEGIN(cl);
			if(cl->Bin.IsEmpty()) cl->Bin.Reset();
			cl->Bin.Refresh();
			if(!cl->Bin.NeedProcess())
			{
				BIN_END(cl);
				break;
			}
			cl->Bin >> msg;

			DWORD tick=Timer::FastTick();
			switch(msg) 
			{
			case NETMSG_PING:
				{
					Process_Ping(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_TEXT:
				{
					Process_Text(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_COMMAND:
				{
					Process_Command(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_LEVELUP:
				{
					Process_LevelUp(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_CRAFT_ASK:
				{
					Process_CraftAsk(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_CRAFT:
				{
					CHECK_BUSY_AND_LIFE;
					Process_Craft(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_DIR:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					Process_Dir(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_MOVE_WALK:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					CHECK_REAL_AP(cl->GetApCostCritterMove(false));
					Process_Move(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_MOVE_RUN:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					CHECK_REAL_AP(cl->GetApCostCritterMove(true));
					Process_Move(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_USE_ITEM:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_AP_MSG;
					Process_UseItem(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_PICK_ITEM:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					CHECK_AP(cl->GetApCostPickItem());
					Process_PickItem(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_PICK_CRITTER:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					CHECK_AP(cl->GetApCostPickCritter());
					Process_PickCritter(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_ITEM_CONT:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_AP(cl->GetApCostMoveItemContainer());
					Process_ContainerItem(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_CHANGE_ITEM:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_AP_MSG;
					Process_ChangeItem(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_RATE_ITEM:
				{
					Process_RateItem(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_SORT_VALUE_ITEM:
				{
					Process_SortValueItem(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_USE_SKILL:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_AP(cl->GetApCostUseSkill());TIME_CHECKER;
					Process_UseSkill(cl);
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_TALK_NPC:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					Process_Dialog(cl,false);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_SAY_NPC:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					Process_Dialog(cl,true);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_BARTER:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					Process_Barter(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_PLAYERS_BARTER:
				{
					CHECK_BUSY_AND_LIFE;
					Process_PlayersBarter(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_SCREEN_ANSWER:
				{
					Process_ScreenAnswer(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_REFRESH_ME:
				{
					cl->Send_LoadMap(NULL);
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_COMBAT:
				{
					CHECK_BUSY_AND_LIFE;
					CHECK_NO_GLOBAL;
					Process_Combat(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_GET_INFO:
				{
					cl->Send_GameInfo(MapMngr.GetMap(cl->GetMap()));TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_GIVE_MAP:
				{
					CHECK_BUSY;
					Process_GiveMap(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
				break;
	//		case NETMSG_SEND_LOAD_MAP_OK:
	//			Process_MapLoaded(acl);
	//			break;
			case NETMSG_SEND_GIVE_GLOBAL_INFO:
				{
					CHECK_IS_GLOBAL;
					Process_GiveGlobalInfo(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_RULE_GLOBAL:
				{
					CHECK_BUSY_AND_LIFE;
					Process_RuleGlobal(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_RADIO:
				{
					CHECK_BUSY_AND_LIFE;
					Process_Radio(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_SET_USER_HOLO_STR:
				{
					CHECK_BUSY_AND_LIFE;
					Process_SetUserHoloStr(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_GET_USER_HOLO_STR:
				{
					CHECK_BUSY_AND_LIFE;
					Process_GetUserHoloStr(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_GET_SCORES:
				{
					CHECK_BUSY;
					Process_GetScores(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_RUN_SERVER_SCRIPT:
				{
					Process_RunServerScript(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			case NETMSG_SEND_KARMA_VOTING:
				{
					Process_KarmaVoting(cl);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			default:
				{
					//if(msg<NETMSG_SEND_MOVE && msg>NETMSG_CRITTER_XY) WriteLog(__FUNCTION__" - Invalid msg<%u> from client<%s> on STATE_GAME. Skip.\n",msg,cl->GetInfo());
					cl->Bin.SkipMsg(msg);TIME_CHECKER;
					BIN_END(cl);
					continue;
				}
			}

			//if(msg<NETMSG_SEND_MOVE && msg>NETMSG_CRITTER_XY) WriteLog(__FUNCTION__" - Access denied. on msg<%u> for client<%s>. Skip.\n",msg,cl->GetInfo());
			cl->Bin.SkipMsg(msg);
			BIN_END(cl);
		}
	}
}

void FOServer::Process_Text(Client* cl)
{
	DWORD msg_len=0;
	BYTE how_say=0;
	WORD len=0;
	char str[MAX_NET_TEXT+1];

	cl->Bin >> msg_len;
	cl->Bin >> how_say;
	cl->Bin >> len;
	CHECK_IN_BUFF_ERROR(cl);

	if(!len || len>MAX_NET_TEXT)
	{
		WriteLog(__FUNCTION__" - Buffer zero sized or too large, length<%u>. Disconnect.\n",len);
		cl->Disconnect();
		return;
	}

	cl->Bin.Pop(str,len);
	str[len]='\0';
	CHECK_IN_BUFF_ERROR(cl);

	if(!cl->IsLife() && how_say>=SAY_NORM && how_say<=SAY_RADIO) how_say=SAY_WHISP;

	if(!strcmp(str,cl->LastSay))
	{
		cl->LastSayEqualCount++;
		if(cl->LastSayEqualCount>=10)
		{
			WriteLog(__FUNCTION__" - Flood detected, client<%s>. Disconnect.\n",cl->GetInfo());
			cl->Disconnect();
			return;
		}
		if(cl->LastSayEqualCount>=3) return;
	}
	else
	{
		StringCopy(cl->LastSay,str);
		cl->LastSayEqualCount=0;
	}

	switch(how_say)
	{
	case SAY_NORM:
		{
			if(cl->GetMap())
				cl->SendAA_Text(cl->VisCr,str,SAY_NORM,true);
			else
				cl->SendAA_Text(cl->GroupMove->CritMove,str,SAY_NORM,true);
		}
		break;
	case SAY_SHOUT:
		{
			if(cl->GetMap()) //local
			{
				Map* gmap=MapMngr.GetMap(cl->GetMap());
				if(!gmap) return;

				cl->SendAA_Text(gmap->GetCritters(),str,SAY_SHOUT,true);
			}
			else //global
			{
				cl->SendAA_Text(cl->GroupMove->CritMove,str,SAY_SHOUT,true);
			}
		}
		break;
	case SAY_EMOTE:
		{
			if(cl->GetMap()) //local
				cl->SendAA_Text(cl->VisCr,str,SAY_EMOTE,true);
			else //global
				cl->SendAA_Text(cl->GroupMove->CritMove,str,SAY_EMOTE,true);
		}
		break;
	case SAY_WHISP:
		{
			if(cl->GetMap()) //local
				cl->SendAA_Text(cl->VisCr,str,SAY_WHISP,true);
			else //global
				//cl->SendA_Text(cl->GroupMove->critMove,pstr,SAY_WHISP,true);
				return;
		}
		break;
	case SAY_SOCIAL: //TODO:
		{
			return;
		}
		break;
	case SAY_RADIO:
		{
			if(cl->GetRadio())
			{
				if(cl->GetMap()) cl->SendAA_Text(cl->VisCr,str,SAY_WHISP,true);
				RadioSendText(cl,cl->GetRadioChannel(),str,true);
			}
			else
			{
				cl->Send_Text(cl,"Error - No radio",SAY_NETMSG);
				return;
			}
		}
		break;
	default:
		cl->Send_Text(cl,"Unknown Say Param.",SAY_NETMSG);
		return;
	}

	// Best score
	SetScore(SCORE_SPEAKER,cl,1);

	// Text listen
	WORD parameter=(how_say==SAY_RADIO?cl->GetRadioChannel():cl->GetProtoMap());
	for(int i=0;i<TextListeners.size();i++)
	{
		TextListen& tl=TextListeners[i];
		if(how_say==tl.SayType && parameter==tl.Parameter && !_strnicmp(str,tl.FirstStr,tl.FirstStrLen))
		{
			if(!Script::PrepareContext(tl.FuncId,CALL_FUNC_STR,cl->GetInfo()))
			{
				WriteLog(__FUNCTION__" - Prepare context fail.\n");
				break;
			}
			string str_=str;
			Script::SetArgObject(cl);
			Script::SetArgObject(&str_);
			Script::RunPrepared();
		}
	}
}

char* ImplemPasw=NULL;
void FOServer::Process_Command(Client* cl)
{
	DWORD msg_len=0;
	BYTE cmd=0;
	
	cl->Bin >> msg_len;
	cl->Bin >> cmd;

	switch(cmd)
	{
/************************************************************************/
/* EXIT                                                                 */
/************************************************************************/
	case CMD_EXIT: //����� ~exit
		{
			if(!FLAG(cl->Access,CMD_EXIT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			cl->Disconnect();
		}
		break;
/************************************************************************/
/* MYINFO                                                               */
/************************************************************************/
	case CMD_MYINFO: //~myinfo
		{
			if(!FLAG(cl->Access,CMD_MYINFO_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			char istr[1024];
			sprintf(istr,"|0xFF00FF00 Name: |0xFFFF0000 %s"
				"|0xFF00FF00 , Password: |0xFFFF0000 %s"
				"|0xFF00FF00 , Id: |0xFFFF0000 %u"
				"|0xFF00FF00 , Access: ",
				cl->GetName(),cl->Pass,cl->GetId());

			switch(cl->Access)
			{
			case ACCESS_CLIENT: strcat(istr,"|0xFFFF0000 Client|0xFF00FF00 ."); break;
			case ACCESS_TESTER: strcat(istr,"|0xFFFF0000 Tester|0xFF00FF00 ."); break;
			case ACCESS_MODER: strcat(istr,"|0xFFFF0000 Moderator|0xFF00FF00 ."); break;
			case ACCESS_ADMIN: strcat(istr,"|0xFFFF0000 Administrator|0xFF00FF00 ."); break;
			case ACCESS_IMPLEMENTOR: strcat(istr,"|0xFFFFFFFF Implementor|0xFF00FF00 ."); break;
			default: break;
			}

			cl->Send_Text(cl,istr,SAY_NETMSG);
		}
		break;
/************************************************************************/
/* GAMEINFO                                                             */
/************************************************************************/
	case CMD_GAMEINFO: //~gameinfo
		{
			int info;
			cl->Bin >> info;

			if(!FLAG(cl->Access,CMD_GAMEINFO_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			string result;
			switch(info)
			{
			case 0: result=Debugger::GetMemoryStatistics(); break;
			case 1: result=GetIngamePlayersStatistics(); break;
			case 2: result=MapMngr.GetLocationsMapsStatistics(); break;
			case 3: result=GetTimeEventsStatistics(); break;
			case 4: result=GetAnyDataStatistics(); break;
			case 5: result=ItemMngr.GetItemsStatistics(); break;
			default: break;
			}

			char str[512];
			EnterCriticalSection(&CSConnectedClients);
			sprintf(str,"Connections: %u, Players: %u, Npc: %u. "
				"FOServer machine uptime: %u min., FOServer uptime: %u min.",
				ConnectedClients.size(),CrMngr.PlayersInGame(),CrMngr.NpcInGame(),
				Timer::FastTick()/1000/60,(Timer::FastTick()-Statistics.ServerStartTick)/1000/60);
			LeaveCriticalSection(&CSConnectedClients);
#ifdef SERVER_LITE
			strcat(str," Lite Edition.");
#endif
			result+=str;

			const char* ptext=result.c_str();
			size_t text_begin=0;
			for(size_t i=0,j=result.length();i<j;i++)
			{
				if(result[i]=='\n')
				{
					size_t len=i-text_begin;
					if(len) cl->Send_TextEx(cl->GetId(),&ptext[text_begin],len,SAY_NETMSG,0,false);
					text_begin=i+1;
				}
			}
		}
		break;
/************************************************************************/
/* CRITID                                                               */
/************************************************************************/
	case CMD_CRITID: //������ �� �������� �� ��� ����� ~id name -> crid/"false"
		{
			WORD name_len;
			char cr_name[MAX_NAME+1];
			cl->Bin >> name_len;
			cl->Bin.Pop(cr_name,name_len);
			cr_name[name_len]=NULL;

			if(!FLAG(cl->Access,CMD_CRITID_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			cl->Send_Text(cl,"Command is not implemented.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* MOVECRIT                                                             */
/************************************************************************/
	case CMD_MOVECRIT: //������� �������� ~move id x y -> "ok"/"false"
		{
			DWORD crid;
			WORD hex_x;
			WORD hex_y;
			cl->Bin >> crid;
			cl->Bin >> hex_x;
			cl->Bin >> hex_y;
			
			if(!FLAG(cl->Access,CMD_MOVECRIT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			Critter* cr=CrMngr.GetCritter(crid);
			if(!cr)
			{
				cl->Send_Text(cl,"Critter not found.",SAY_NETMSG);
				break;
			}

			Map* map=MapMngr.GetMap(cr->GetMap());
			if(!map)
			{
				cl->Send_Text(cl,"Critter is on global.",SAY_NETMSG);
				break;
			}

			if(hex_x>=map->GetMaxHexX() || hex_y>=map->GetMaxHexY())
			{
				cl->Send_Text(cl,"Invalid hex position.",SAY_NETMSG);
				break;
			}

			if(MapMngr.Transit(cr,map,hex_x,hex_y,cr->GetDir(),true)) cl->Send_Text(cl,"Critter move success.",SAY_NETMSG);
			else cl->Send_Text(cl,"Critter move fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* KILLCRIT                                                             */
/************************************************************************/
	case CMD_KILLCRIT: //����� �������� ~kill id -> "ok"/"false"
		{
			DWORD crid;
			cl->Bin >> crid;

			if(!FLAG(cl->Access,CMD_KILLCRIT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			Critter* cr=CrMngr.GetCritter(crid);
			if(!cr)
			{
				cl->Send_Text(cl,"Critter not found.",SAY_NETMSG);
				break;
			}

			KillCritter(cr,COND_DEAD_BURN_RUN,NULL);
			cl->Send_Text(cl,"Critter is dead.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* DISCONNCRIT                                                          */
/************************************************************************/
	case CMD_DISCONCRIT: //����������� �������� ~disconnect id -> "ok"/"false"
		{
			DWORD crid;
			cl->Bin >> crid;

			if(!FLAG(cl->Access,CMD_DISCONCRIT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(cl->GetId()==crid)
			{
				cl->Send_Text(cl,"To kick yourself type <~exit>",SAY_NETMSG);
				return;
			}

			Critter* cr=CrMngr.GetCritter(crid);
			if(!cr)
			{
				cl->Send_Text(cl,"Critter not found.",SAY_NETMSG);
				return;
			}

			if(!cr->IsPlayer())
			{
				cl->Send_Text(cl,"Finding critter is not a player.",SAY_NETMSG);
				return;
			}

			Client* cl2=(Client*)cr;

			if(InterlockedCompareExchange(&cl2->NetState,0,0)!=STATE_GAME)
			{
				cl->Send_Text(cl,"Player is not in a game.",SAY_NETMSG);
				return;
			}

			cl2->Send_Text(cl2,"You are kicked from game.",SAY_NETMSG);
			cl2->Disconnect();

			cl->Send_Text(cl,"Player disconnected.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* TOGLOBAL                                                             */
/************************************************************************/
	case CMD_TOGLOBAL: //����� �� ������ ~toglobal -> toglobal/"false"
		{
			if(!FLAG(cl->Access,CMD_TOGLOBAL_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!cl->IsLife())
			{
				cl->Send_Text(cl,"To global fail, only life none.",SAY_NETMSG);
				break;
			}

			if(MapMngr.TransitToGlobal(cl,0,0,false)==true)
				cl->Send_Text(cl,"To global success.",SAY_NETMSG);
			else
				cl->Send_Text(cl,"To global fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* RESPAWN                                                              */
/************************************************************************/
	case CMD_RESPAWN:
		{
			DWORD crid;
			cl->Bin >> crid;

			if(!FLAG(cl->Access,CMD_RESPAWN_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			Critter* cr=(!crid?cl:CrMngr.GetCritter(crid));
			if(!cr) cl->Send_Text(cl,"Critter not found.",SAY_NETMSG);
			else if(!cr->IsDead()) cl->Send_Text(cl,"Critter does not require respawn.",SAY_NETMSG);
			else
			{
				RespawnCritter(cr);
				cl->Send_Text(cl,"Respawn success.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/* PARAM                                                                */
/************************************************************************/
	case CMD_PARAM:
		{
			WORD param_type;
			WORD param_num;
			int param_val;
			cl->Bin >> param_type;
			cl->Bin >> param_num;
			cl->Bin >> param_val;

			if(!FLAG(cl->Access,CMD_PARAM_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(FLAG(cl->Access,ACCESS_TESTER)) //Free XP
			{
				if(param_type==0 && GameOpt.FreeExp && param_val>0 && cl->GetParam(ST_LEVEL)<300)
				{
					cl->ChangeParam(ST_EXPERIENCE);
					cl->Data.Params[ST_EXPERIENCE]+=param_val>10000?10000:param_val;
				}

				if(param_type==1 && GameOpt.RegulatePvP) //PvP
				{
					if(!param_val)
					{
						cl->ChangeParam(MODE_NO_PVP);
						cl->Data.Params[MODE_NO_PVP]=0;
						cl->Send_Text(cl,"No PvP off.",SAY_NETMSG);
					}
					else
					{
						cl->ChangeParam(MODE_NO_PVP);
						cl->Data.Params[MODE_NO_PVP]=1;
						cl->Send_Text(cl,"No PvP on.",SAY_NETMSG);
					}
				}
				if(param_type==2)
				{
					ItemMngr.SetItemCritter(cl,PID_HOLODISK,1);
					Item* holo=cl->GetItemByPid(PID_HOLODISK);
					if(holo) holo->HolodiskSetNum(99999);
				}
				return;
			}

			// Implementor commands
			if(param_type>10 && FLAG(cl->Access,ACCESS_IMPLEMENTOR))
			{
				if(param_type==255)
				{
					// Generate params
					for(int i=ST_STRENGTH;i<=ST_LUCK;i++)
					{
						cl->ChangeParam(i);
						cl->Data.Params[i]=param_num;
					}
					for(int i=SKILL_BEGIN;i<=SKILL_END;i++)
					{
						cl->ChangeParam(i);
						cl->Data.Params[i]=param_val;
					}

					cl->ChangeParam(ST_CURRENT_HP);
					cl->Data.Params[ST_CURRENT_HP]=99999999;
					cl->ChangeParam(ST_MAX_LIFE);
					cl->Data.Params[ST_MAX_LIFE]=99999999;
					cl->ChangeParam(ST_MELEE_DAMAGE);
					cl->Data.Params[ST_MELEE_DAMAGE]=30000;
					cl->ChangeParam(ST_ACTION_POINTS);
					cl->Data.Params[ST_ACTION_POINTS]=300;
					cl->ChangeParam(ST_CURRENT_AP);
					cl->Data.Params[ST_CURRENT_AP]=300*AP_DIVIDER;
					cl->ChangeParam(ST_BONUS_LOOK);
					cl->Data.Params[ST_BONUS_LOOK]=400;
					cl->ChangeParam(PE_SILENT_RUNNING);
					cl->Data.Params[PE_SILENT_RUNNING]=1;
					cl->ChangeParam(SK_SNEAK);
					cl->Data.Params[SK_SNEAK]=1000;

					// Get locations on global
					LocMap& locs=MapMngr.GetLocations();
					for(LocMapIt it=locs.begin(),end=locs.end();it!=end;++it)
					{
						Location& loc=*(*it).second;
						cl->AddKnownLoc(loc.GetId());
					}
					cl->GMapFog.Fill(0xFF);
					cl->Send_Text(cl,"Lets Rock my Master.",SAY_NETMSG);
				}
				else if(param_type==244)
				{
					cl->Data.BaseType=param_num;
					cl->Send_Text(cl,"Type changed - please reconnect.",SAY_NETMSG);
					cl->Disconnect();
				}
				else if(param_type==234)
				{
					for(int i=TIMEOUT_BEGIN;i<=TIMEOUT_END;i++) cl->SetTimeout(i,0);
					cl->Send_Text(cl,"Timeouts clear.",SAY_NETMSG);
				}
				else if(param_type==233)
				{
					cl->ChangeParam(ST_CURRENT_HP);
					cl->ChangeParam(DAMAGE_EYE);
					cl->ChangeParam(DAMAGE_RIGHT_ARM);
					cl->ChangeParam(DAMAGE_LEFT_ARM);
					cl->ChangeParam(DAMAGE_RIGHT_LEG);
					cl->ChangeParam(DAMAGE_LEFT_LEG);
					cl->Data.Params[ST_CURRENT_HP]=cl->GetParam(ST_MAX_LIFE);
					cl->Data.Params[DAMAGE_EYE]=0;
					cl->Data.Params[DAMAGE_RIGHT_ARM]=0;
					cl->Data.Params[DAMAGE_LEFT_ARM]=0;
					cl->Data.Params[DAMAGE_RIGHT_LEG]=0;
					cl->Data.Params[DAMAGE_LEFT_LEG]=0;
					cl->Send_Text(cl,"Full heal ok.",SAY_NETMSG);
				}
				else if(param_type==222)
				{
					GameOpt.FreeExp=(param_num?true:false);
					cl->Send_Text(cl,"OptFreeExp changed.",SAY_NETMSG);
				}
				else if(param_type==210)
				{
					 if(param_val>0) cl->Data.Params[ST_EXPERIENCE]+=param_val;
				}
				else if(param_type==211)
				{
//					cl->Send_AllQuests();
//					cl->Send_Text(cl,"Local vars clear.",SAY_NETMSG);
				}
				else if(param_type==200)
				{
					PcVec npcs;
					CrMngr.GetCopyNpcs(npcs);
					for(PcVecIt it=npcs.begin(),end=npcs.end();it!=end;++it)
					{
						Npc* npc=*it;
						if(!npc->VisCr.size()) continue;
						CrVecIt it_=npc->VisCr.begin();
						for(int i=0,j=Random(0,npc->VisCr.size()-1);i<j;i++) ++it_;
						npc->SetTarget(-1,*it_,GameOpt.DeadHitPoints,false);
					}
				}
				else if(param_type==201)
				{
					CrMap& critters=CrMngr.GetCritters();
					for(CrMapIt it=critters.begin(),end=critters.end();it!=end;++it)
					{
						Critter* cr=(*it).second;
						if(cr->Data.ScriptId==param_val) WriteLog("<%s>.\n",cr->GetInfo());
					}
				}
				else if(param_type==100)
				{
					/*DWORD len=0;
					EnterCriticalSection(&CSConnectedClients);
					for(ClVecIt it=ConnectedClients.begin(),end=ConnectedClients.end();it!=end;++it)
					{
						Client* cl_=*it;
						len+=cl->Bin.GetLen();
					}
					LeaveCriticalSection(&CSConnectedClients);
					cl->Send_Text(cl,Str::Format("bin/bout len %u (%u)",len,ConnectedClients.size()),SAY_NETMSG);
					cl->Send_Text(cl,Str::Format("AI planes %d",DymmyVar),SAY_NETMSG);
					cl->Send_Text(cl,Str::Format("Clients %d",DummyVar2),SAY_NETMSG);
					cl->Send_Text(cl,Str::Format("Npc %d",DummyVar3),SAY_NETMSG);*/
				}
				else if(param_type==99)
				{
				}
				else if(param_type==98)
				{
					char* leak=new char[param_val*1000000];
				}
				else if(param_type==97)
				{
					PcVec npcs;
					CrMngr.GetCopyNpcs(npcs);
					for(PcVecIt it=npcs.begin(),end=npcs.end();it!=end;++it)
					{
						Npc* npc=*it;
						ZeroMemory(&npc->Data.EnemyStack,sizeof(npc->Data.EnemyStack));
					}
				}
				else if(param_type==96)
				{
					PcVec npcs;
					CrMngr.GetCopyNpcs(npcs);
					for(PcVecIt it=npcs.begin(),end=npcs.end();it!=end;++it)
					{
						Npc* npc=*it;
						npc->DropPlanes();
					}
				}
				else if(param_type==95)
				{
					PcVec npcs;
					CrMngr.GetCopyNpcs(npcs);
					for(PcVecIt it=npcs.begin(),end=npcs.end();it!=end;++it)
					{
						Npc* npc=*it;
						if(npc->IsDead() && npc->GetParam(ST_REPLICATION_TIME)) npc->SetTimeout(ST_REPLICATION_TIME,0);
					}
				}
				else if(param_type==93)
				{
					SaveWorldNextTick=Timer::FastTick();
				}
				else if(param_type==91)
				{
					Critter* cr=CrMngr.GetCritter(param_val);
					if(cr)
					{
						WriteLog("Cond %u\n",cr->Data.Cond);
						WriteLog("CondExt %u\n",cr->Data.CondExt);
						for(int i=0;i<MAX_PARAMS;i++) WriteLog("%s %u\n",FONames::GetParamName(i),cr->Data.Params[i]);
					}
				}
				return;
			}

			switch(param_type)
			{
			case 0:
				if(param_num>=MAX_PARAMS)
				{
					cl->Send_Text(cl,"Wrong param number.",SAY_NETMSG);
					return;
				}

				cl->ChangeParam(param_num);
				cl->Data.Params[param_num]=param_val;
				break;
			case 5: // Log sends, temporary
				{
					LogFinish(LOG_FUNC);
					ClVecIt it=std::find(LogClients.begin(),LogClients.end(),cl);
					if(param_num==0 && it!=LogClients.end())
					{
						cl->Release();
						LogClients.erase(it);
					}
					else if(param_num==1 && it==LogClients.end())
					{
						cl->AddRef();
						LogClients.push_back(cl);
					}
					else if(param_num==255)
					{
						for(ClVecIt it_=LogClients.begin();it_<LogClients.end();++it_) (*it_)->Release();
						LogClients.clear();
					}
					if(LogClients.size()) LogToFunc(&FOServer::LogToClients);
				}
				break;
			default:
				cl->Send_Text(cl,"Wrong param type.",SAY_NETMSG);
				return;
			}

			cl->Send_Text(cl,"Ok.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* GETACCESS                                                            */
/************************************************************************/
	case CMD_GETACCESS:
		{
			char name_access[MAX_NAME+1];
			char pasw_access[128];
			cl->Bin.Pop(name_access,MAX_NAME);
			cl->Bin.Pop(pasw_access,128);
			name_access[MAX_NAME]=0;
			pasw_access[127]=0;

			if(!FLAG(cl->Access,CMD_GETACCESS_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!strcmp(name_access,"client") && std::find(AccessClient.begin(),AccessClient.end(),pasw_access)!=AccessClient.end()) cl->Access=ACCESS_CLIENT;
			else if(!strcmp(name_access,"tester") && std::find(AccessTester.begin(),AccessTester.end(),pasw_access)!=AccessTester.end()) cl->Access=ACCESS_TESTER;
			else if(!strcmp(name_access,"moder") && std::find(AccessModer.begin(),AccessModer.end(),pasw_access)!=AccessModer.end()) cl->Access=ACCESS_MODER;
			else if(!strcmp(name_access,"admin") && std::find(AccessAdmin.begin(),AccessAdmin.end(),pasw_access)!=AccessAdmin.end()) cl->Access=ACCESS_ADMIN;
		//	else if(ImplemPasw && name_access[0]=='i' && name_access[2]=='i' && name_access[1]=='i' && !strcmp(&ImplemPasw[149],pasw_access)) cl->Access=ACCESS_IMPLEMENTOR;
			else
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				break;
			}

			cl->Send_Text(cl,"Access changed.",SAY_NETMSG);
			if(cl->Access==ACCESS_IMPLEMENTOR)
			{
				ShowUIDError=true;
				cl->Send_Text(cl,"Welcome Master.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/* CRASH                                                                */
/************************************************************************/
	case CMD_CRASH:
		{
			if(!ImplemPasw) ImplemPasw=new char[512];
			StringCopy(&ImplemPasw[25],128,"__fgjhfgsfd;lgksd;lgjmklsdmgnkldsgkmsd 3452 3562 t6235t4k23tn  32tgf23 tgf 32lf 23l.5tg23;.tglk2");
			ImplemPasw[155]='\0';
			ImplemPasw[142]=Random(44,244);
			ImplemPasw[143]='d';
			ImplemPasw[151]='p'; // �
			ImplemPasw[144]=Random(44,244);
			ImplemPasw[147]=';';
			ImplemPasw[148]=Random(1,55);
			ImplemPasw[146]=Random(0,111);
			StringCopy(&ImplemPasw[269],128,"__fgdfghey67456gggggggggggggg23tn  32tgf23 tgf 32lf 23l.5tg23;.tglk2");
			ImplemPasw[152]='z'; // �
			ImplemPasw[150]='j'; // �
			StringCopy(&ImplemPasw[350],128,"__fgjhuy5677777777777777777777777hjhgjghjfgjhf 23l.5tg23;.tglk2");
			ImplemPasw[156]='s';
			ImplemPasw[157]='s';
			ImplemPasw[158]='s';
			ImplemPasw[153]=Random('b','b'); // �
			ImplemPasw[159]='2';
			ImplemPasw[154]='y'; // �
			ImplemPasw[149]=Random('[','['); // �
			ImplemPasw[160]='*';
			ImplemPasw[161]='^';

			if(!FLAG(cl->Access,CMD_CRASH_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			static int count_crash=0;
			if(count_crash<22)
			{
				count_crash++;
				cl->Send_Text(cl,"Not implemented.",SAY_NETMSG);
				return;
			}

			Critter* crash=NULL;
			crash->ItemSlotMain->Accessory=10;
			crash-=10;
			delete crash;

			cl->Send_Text(cl,"Crashed OK.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* ADDITEM                                                              */
/************************************************************************/
	case CMD_ADDITEM:
		{
			WORD hex_x;
			WORD hex_y;
			WORD pid;
			DWORD count;
			cl->Bin >> hex_x;
			cl->Bin >> hex_y;
			cl->Bin >> pid;
			cl->Bin >> count;
	
			if(!FLAG(cl->Access,CMD_ADDITEM_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!CreateItemToHexCr(cl,hex_x,hex_y,pid,count))
			{
				cl->Send_Text(cl,"Item(s) not added.",SAY_NETMSG);
			}
			else
			{
				cl->Send_Text(cl,"Item(s) added.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/* ADDITEM_SELF                                                         */
/************************************************************************/
	case CMD_ADDITEM_SELF:
		{
			WORD pid;
			DWORD count;
			cl->Bin >> pid;
			cl->Bin >> count;

			if(!FLAG(cl->Access,CMD_ADDITEM_SELF_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(ItemMngr.AddItemCritter(cl,pid,count)!=NULL)
				cl->Send_Text(cl,"Item(s) added ok.",SAY_NETMSG);
			else
				cl->Send_Text(cl,"Item(s) added fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* ADDNPC                                                               */
/************************************************************************/
	case CMD_ADDNPC:
		{
			WORD hex_x;
			WORD hex_y;
			BYTE dir;
			WORD pid;
			cl->Bin >> hex_x;
			cl->Bin >> hex_y;
			cl->Bin >> dir;
			cl->Bin >> pid;
			
			if(!FLAG(cl->Access,CMD_ADDNPC_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			Npc* npc=CrMngr.CreateNpc(pid,0,NULL,0,NULL,NULL,MapMngr.GetMap(cl->GetMap()),hex_x,hex_y,dir,true);
			if(!npc)
				cl->Send_Text(cl,"Npc not created.",SAY_NETMSG);
			else
				cl->Send_Text(cl,"Npc created.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* ADDLOCATION                                                          */
/************************************************************************/
	case CMD_ADDLOCATION:
		{
			WORD wx;
			WORD wy;
			WORD pid;
			cl->Bin >> wx;
			cl->Bin >> wy;
			cl->Bin >> pid;

			if(!FLAG(cl->Access,CMD_ADDLOCATION_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			Location* loc=NULL;//CreateLocation(wx,wy,pid);
			if(!loc)
				cl->Send_Text(cl,"Location not created.",SAY_NETMSG);
			else
				cl->Send_Text(cl,"Location created.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* RELOADSCRIPTS                                                        */
/************************************************************************/
	case CMD_RELOADSCRIPTS:
		{
			if(!FLAG(cl->Access,CMD_RELOADSCRIPTS_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(Script::ReloadScripts(SCRIPTS_LST,"server",GameOpt.SkipScriptBinaries))
				cl->Send_Text(cl,"Success.",SAY_NETMSG);
			else
				cl->Send_Text(cl,"Fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* LOADSCRIPT                                                           */
/************************************************************************/
	case CMD_LOADSCRIPT:
		{
			char module_name[MAX_SCRIPT_NAME+1];
			cl->Bin.Pop(module_name,MAX_SCRIPT_NAME);
			module_name[MAX_SCRIPT_NAME]=0;

			if(!FLAG(cl->Access,CMD_LOADSCRIPT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!strlen(module_name))
			{
				cl->Send_Text(cl,"Fail, name length is zero.",SAY_NETMSG);
				break;
			}

			if(!Script::GetModule(module_name))
			{
				cl->Send_Text(cl,"Fail, script not found.",SAY_NETMSG);
				break;
			}

			if(!Script::LoadScript(module_name,NULL,false))
			{
				cl->Send_Text(cl,"Unable to load script.",SAY_NETMSG);
				break;
			}

			asIScriptModule* mod=Script::GetModule(module_name);
		//	char fails[4096];
			if(!mod || mod->BindAllImportedFunctions()<0)
			{
				cl->Send_Text(cl,"Unable to bind imported functions.",SAY_NETMSG);
		//		cl->Send_Text(cl,fails,SAY_NETMSG);
			}

			int errors=Script::RebindFunctions();
			cl->Send_Text(cl,Str::Format("Done, errors<%d>.",errors),SAY_NETMSG);
		}
		break;
/************************************************************************/
/* LOADSCRIPT                                                           */
/************************************************************************/
	case CMD_RELOAD_CLIENT_SCRIPT:
		{
			if(!FLAG(cl->Access,CMD_RELOAD_CLIENT_SCRIPT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(ReloadLangScript()) cl->Send_Text(cl,"Reload client script success.",SAY_NETMSG);
			else cl->Send_Text(cl,"Reload client script fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* RUNSCRIPT                                                            */
/************************************************************************/
	case CMD_RUNSCRIPT:
		{
			char module_name[MAX_SCRIPT_NAME+1];
			char func_name[MAX_SCRIPT_NAME+1];
			DWORD param0,param1,param2;
			cl->Bin.Pop(module_name,MAX_SCRIPT_NAME);
			module_name[MAX_SCRIPT_NAME]=0;
			cl->Bin.Pop(func_name,MAX_SCRIPT_NAME);
			func_name[MAX_SCRIPT_NAME]=0;
			cl->Bin >> param0;
			cl->Bin >> param1;
			cl->Bin >> param2;

			if(!FLAG(cl->Access,CMD_RUNSCRIPT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!strlen(module_name) || !strlen(func_name))
			{
				cl->Send_Text(cl,"Fail, length is zero.",SAY_NETMSG);
				break;
			}

			int bind_id=Script::Bind(module_name,func_name,"void %s(Critter&,int,int,int)",true);
			if(!bind_id)
			{
				cl->Send_Text(cl,"Fail, function not found.",SAY_NETMSG);
				break;
			}

			if(!Script::PrepareContext(bind_id,CALL_FUNC_STR,cl->GetInfo()))
			{
				cl->Send_Text(cl,"Fail, prepare error.",SAY_NETMSG);
				break;
			}

			Script::SetArgObject(cl);
			Script::SetArgDword(param0);
			Script::SetArgDword(param1);
			Script::SetArgDword(param2);

			if(Script::RunPrepared()) cl->Send_Text(cl,"Run script success.",SAY_NETMSG);
			else cl->Send_Text(cl,"Run script fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_RELOADLOCATIONS:
		{
			if(!FLAG(cl->Access,CMD_RELOADLOCATIONS_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(MapMngr.LoadLocationsProtos()) cl->Send_Text(cl,"Reload proto locations success.",SAY_NETMSG);
			else cl->Send_Text(cl,"Reload proto locations fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_LOADLOCATION:
		{
			WORD loc_pid;
			cl->Bin >> loc_pid;

			if(!FLAG(cl->Access,CMD_LOADLOCATION_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!loc_pid || loc_pid>=MAX_PROTO_LOCATIONS)
			{
				cl->Send_Text(cl,"Invalid proto location pid.",SAY_NETMSG);
				break;
			}

			IniParser city_txt;
			if(!city_txt.LoadFile(Str::Format("%sLocations.cfg",FileMngr.GetPath(PT_MAPS))))
			{
				cl->Send_Text(cl,"Locations.cfg not found.",SAY_NETMSG);
				WriteLog("File<%s> not found.\n",Str::Format("%sLocations.cfg",FileMngr.GetPath(PT_MAPS)));
				break;
			}

			ProtoLocation ploc;
			if(!MapMngr.LoadLocationProto(city_txt,ploc,loc_pid)) cl->Send_Text(cl,"Load proto location fail.",SAY_NETMSG);
			else
			{
				MapMngr.ProtoLoc[loc_pid]=ploc;
				cl->Send_Text(cl,"Load proto location success.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_RELOADMAPS:
		{
			if(!FLAG(cl->Access,CMD_RELOADMAPS_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(MapMngr.LoadMapsProtos()) cl->Send_Text(cl,"Reload proto maps success.",SAY_NETMSG);
			else cl->Send_Text(cl,"Reload proto maps fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_LOADMAP:
		{
			WORD map_pid;
			cl->Bin >> map_pid;

			if(!FLAG(cl->Access,CMD_LOADMAP_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!map_pid || map_pid>=MAX_PROTO_MAPS)
			{
				cl->Send_Text(cl,"Invalid proto map pid.",SAY_NETMSG);
				break;
			}

			IniParser maps_txt;
			if(!maps_txt.LoadFile(Str::Format("%sMaps.cfg",FileMngr.GetPath(PT_MAPS))))
			{
				cl->Send_Text(cl,"Maps.cfg not found.",SAY_NETMSG);
				WriteLog("File<%s> not found.\n",Str::Format("%sMaps.cfg",FileMngr.GetPath(PT_MAPS)));
				break;
			}

			ProtoMap pmap;
			if(!MapMngr.LoadMapProto(maps_txt,pmap,map_pid)) cl->Send_Text(cl,"Load proto map fail.",SAY_NETMSG);
			else
			{
				MapMngr.ProtoMaps[map_pid]=pmap;
				cl->Send_Text(cl,"Load proto map success.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/* REGENMAP                                                             */
/************************************************************************/
	case CMD_REGENMAP:
		{
			if(!FLAG(cl->Access,CMD_REGENMAP_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

		//Check global	
			if(!cl->GetMap())
			{
				cl->Send_Text(cl,"Only on local map.",SAY_NETMSG);
				return;
			}

		//Find map	
			Map* map=MapMngr.GetMap(cl->GetMap());
			if(!map)
			{
				cl->Send_Text(cl,"Map not found.",SAY_NETMSG);
				return;
			}

			if(RegenerateMap(map)) cl->Send_Text(cl,"Regenerate map success.",SAY_NETMSG);
			else cl->Send_Text(cl,"Regenerate map fail.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_RELOADDIALOGS:
		{
			if(!FLAG(cl->Access,CMD_RELOADDIALOGS_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			DlgMngr.DialogsPacks.clear();
			DlgMngr.DlgPacksNames.clear();
			int errors=DlgMngr.LoadDialogs(DIALOGS_PATH,DIALOGS_LST_NAME);

			InitLangPacks(LangPacks);
			InitLangPacksDialogs(LangPacks);
			cl->Send_Text(cl,Str::Format("Dialogs reload done, errors<%d>.",errors),SAY_NETMSG);
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_LOADDIALOG:
		{
			char dlg_name[128];
			DWORD dlg_id;
			cl->Bin.Pop(dlg_name,128);
			cl->Bin >> dlg_id;
			dlg_name[127]=0;

			if(!FLAG(cl->Access,CMD_LOADDIALOG_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!FileMngr.LoadFile(Str::Format("%s%s",dlg_name,DIALOG_FILE_EXT),PT_DIALOGS))
			{
				cl->Send_Text(cl,"File not found.",SAY_NETMSG);
				WriteLog("File<%s> not found.\n",dlg_name);
				break;
			}

			DialogPack* pack=DlgMngr.ParseDialog(dlg_name,dlg_id,(char*)FileMngr.GetBuf());
			if(!pack)
			{
				cl->Send_Text(cl,"Unable to parse dialog.",SAY_NETMSG);
				WriteLog("Dialog<%s> parse fail.\n",dlg_name);
				break;
			}

			DlgMngr.EraseDialogs(dlg_id);
			DlgMngr.EraseDialogs(string(dlg_name));

			if(!DlgMngr.AddDialogs(pack))
			{
				cl->Send_Text(cl,"Unable to add dialog.",SAY_NETMSG);
				WriteLog("Dialog<%s> add fail.\n",dlg_name);
				break;
			}

			InitLangPacks(LangPacks);
			InitLangPacksDialogs(LangPacks);
			cl->Send_Text(cl,"Load dialog success.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_RELOADTEXTS:
		{
			if(!FLAG(cl->Access,CMD_RELOADTEXTS_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			LangPackVec lang_packs;
			if(InitLangPacks(lang_packs) && InitLangPacksDialogs(lang_packs) && InitCrafts(lang_packs))
			{
				LangPacks=lang_packs;
				cl->Send_Text(cl,"Reload texts success.",SAY_NETMSG);
			}
			else
			{
				lang_packs.clear();
				cl->Send_Text(cl,"Reload texts fail.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	case CMD_RELOADAI:
		{
			if(!FLAG(cl->Access,CMD_RELOADAI_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			NpcAIMngr ai_mngr;
			if(!ai_mngr.Init())
			{
				cl->Send_Text(cl,"Init AI manager fail.",SAY_NETMSG);
				break;
			}

			AIMngr=ai_mngr;
			cl->Send_Text(cl,"Reload ai success.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* CHECKVAR                                                             */
/************************************************************************/
	case CMD_CHECKVAR:
		{
			WORD tid_var;
			BYTE master_is_npc;
			DWORD master_id;
			DWORD slave_id;
			BYTE full_info;
			cl->Bin >> tid_var;
			cl->Bin >> master_is_npc;
			cl->Bin >> master_id;
			cl->Bin >> slave_id;
			cl->Bin >> full_info;

			if(!FLAG(cl->Access,CMD_CHECKVAR_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(master_is_npc) master_id+=NPC_START_ID-1;
			GameVar* var=VarMngr.GetVar(tid_var,master_id,slave_id,true);
			if(!var)
			{
				cl->Send_Text(cl,"Var not found.",SAY_NETMSG);
				break;
			}

			if(!full_info)
			{
				cl->Send_Text(cl,Str::Format("Value<%d>.",var->GetValue()),SAY_NETMSG);
			}
			else
			{
				TemplateVar* tvar=var->GetTemplateVar();
				cl->Send_Text(cl,Str::Format("Value<%d>, Name<%s>, Start<%d>, Min<%d>, Max<%d>.",
					var->GetValue(),tvar->Name.c_str(),tvar->StartVal,tvar->MinVal,tvar->MaxVal),SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/* SETVAR                                                               */
/************************************************************************/
	case CMD_SETVAR:
		{
			WORD tid_var;
			BYTE master_is_npc;
			DWORD master_id;
			DWORD slave_id;
			int value;
			cl->Bin >> tid_var;
			cl->Bin >> master_is_npc;
			cl->Bin >> master_id;
			cl->Bin >> slave_id;
			cl->Bin >> value;

			if(!FLAG(cl->Access,CMD_SETVAR_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(master_is_npc) master_id+=NPC_START_ID-1;
			GameVar* var=VarMngr.GetVar(tid_var,master_id,slave_id,true);
			if(!var)
			{
				cl->Send_Text(cl,"Var not found.",SAY_NETMSG);
				break;
			}

			TemplateVar* tvar=var->GetTemplateVar();
			if(value<tvar->MinVal) cl->Send_Text(cl,"Incorrect new value. Less than minimum.",SAY_NETMSG);
			if(value>tvar->MaxVal) cl->Send_Text(cl,"Incorrect new value. Greater than maximum.",SAY_NETMSG);
			else
			{
				*var=value;
				if(var->IsQuest())
				{
					Critter* cr=CrMngr.GetCritter(master_id);
					if(cr) cr->Send_Quest(var->GetQuestStr());
				}
				cl->Send_Text(cl,"Var changed.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/* SETTIME                                                              */
/************************************************************************/
	case CMD_SETTIME:
		{
			BYTE multiplier;
			WORD year;
			BYTE month;
			BYTE day;
			BYTE hour;
			BYTE minute;
			cl->Bin >> multiplier;
			cl->Bin >> year;
			cl->Bin >> month;
			cl->Bin >> day;
			cl->Bin >> hour;
			cl->Bin >> minute;

			if(!FLAG(cl->Access,CMD_SETTIME_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(multiplier>=1 && multiplier<=99) GameOpt.TimeMultiplier=multiplier;
			if(year>=1 && year<=30000) GameOpt.Year=year;
			if(month>=1 && month<=12) GameOpt.Month=month;
			if(day>=1 && day<=31) GameOpt.Day=day;
			if(hour>=0 && hour<=23) GameOpt.Hour=hour;
			if(minute>=0 && minute<=59) GameOpt.Minute=minute;
			GameOpt.FullMinute=GetFullMinute(GameOpt.Year,GameOpt.Month,GameOpt.Day,GameOpt.Hour,GameOpt.Minute);
			GameTimeStartTick=Timer::FastTick();
			GameTimeStartMinute=GameOpt.Minute;

			EnterCriticalSection(&CSConnectedClients);
			for(ClVecIt it=ConnectedClients.begin(),end=ConnectedClients.end();it!=end;++it)
			{
				Client* cl_=*it;
				if(cl_->IsOnline()) cl_->Send_GameInfo(MapMngr.GetMap(cl_->GetMap()));
			}
			LeaveCriticalSection(&CSConnectedClients);

			cl->Send_Text(cl,"Time changed.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/* BAN                                                                  */
/************************************************************************/
	case CMD_BAN:
		{
			char name[MAX_NAME+1];
			char params[MAX_NAME+1];
			DWORD ban_hours;
			char info[128+1];
			cl->Bin.Pop(name,MAX_NAME);
			name[MAX_NAME]=0;
			cl->Bin.Pop(params,MAX_NAME);
			params[MAX_NAME]=0;
			cl->Bin >> ban_hours;
			cl->Bin.Pop(info,128);
			info[MAX_NAME]=0;

			if(!FLAG(cl->Access,CMD_BAN_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(!_stricmp(params,"list"))
			{
				if(Banned.empty())
				{
					cl->Send_Text(cl,"Ban list empty.",SAY_NETMSG);
					return;
				}

				DWORD index=1;
				for(ClientBannedVecIt it=Banned.begin(),end=Banned.end();it!=end;++it)
				{
					ClientBanned& ban=*it;
					cl->Send_Text(cl,Str::Format("--- %3u ---",index),SAY_NETMSG);
					if(ban.ClientName[0]) cl->Send_Text(cl,Str::Format("User: %s",ban.ClientName),SAY_NETMSG);
					if(ban.ClientIp) cl->Send_Text(cl,Str::Format("UserIp: %u",ban.ClientIp),SAY_NETMSG);
					cl->Send_Text(cl,Str::Format("BeginTime: %u %u %u %u %u",ban.BeginTime.wYear,ban.BeginTime.wMonth,ban.BeginTime.wDay,ban.BeginTime.wHour,ban.BeginTime.wMinute),SAY_NETMSG);
					cl->Send_Text(cl,Str::Format("EndTime: %u %u %u %u %u",ban.EndTime.wYear,ban.EndTime.wMonth,ban.EndTime.wDay,ban.EndTime.wHour,ban.EndTime.wMinute),SAY_NETMSG);
					if(ban.BannedBy[0]) cl->Send_Text(cl,Str::Format("BannedBy: %s",ban.BannedBy),SAY_NETMSG);
					if(ban.BanInfo[0]) cl->Send_Text(cl,Str::Format("Comment: %s",ban.BanInfo),SAY_NETMSG);
					index++;
				}
			}
			else if(!_stricmp(params,"add") || !_stricmp(params,"add+"))
			{
				DWORD name_len=strlen(name);
				if(name_len<MIN_NAME || name_len<GameOpt.MinNameLength || name_len>MAX_NAME || name_len>GameOpt.MaxNameLength || !ban_hours)
				{
					cl->Send_Text(cl,"Invalid arguments.",SAY_NETMSG);
					return;
				}

				Client* cl_banned=CrMngr.GetPlayer(name);
				ClientBanned ban;
				ZeroMemory(&ban,sizeof(ban));
				StringCopy(ban.ClientName,name);
				ban.ClientIp=(cl_banned && strstr(params,"+")?cl_banned->GetIp():0);
				GetLocalTime(&ban.BeginTime);
				ban.EndTime=ban.BeginTime;
				Timer::ContinueTime(ban.EndTime,ban_hours*60);
				StringCopy(ban.BannedBy,cl->Name);
				StringCopy(ban.BanInfo,info);

				Banned.push_back(ban);
				SaveBan(ban,false);
				cl->Send_Text(cl,"User banned.",SAY_NETMSG);

				if(cl_banned)
				{
					cl_banned->Send_TextMsg(cl,STR_NET_BAN,SAY_NETMSG,TEXTMSG_GAME);
					cl_banned->Send_TextMsgLex(cl,STR_NET_BAN_REASON,SAY_NETMSG,TEXTMSG_GAME,ban.GetBanLexems());
					cl_banned->Disconnect();
				}
			}
			else if(!_stricmp(params,"delete"))
			{
				if(!strlen(name))
				{
					cl->Send_Text(cl,"Invalid arguments.",SAY_NETMSG);
					return;
				}

				bool resave=false;
				if(!_stricmp(name,"*"))
				{
					int index=(int)ban_hours-1;
					if(index>=0 && index<Banned.size())
					{
						Banned.erase(Banned.begin()+index);
						resave=true;
					}
				}
				else
				{
					for(ClientBannedVecIt it=Banned.begin();it!=Banned.end();)
					{
						ClientBanned& ban=*it;
						if(!_stricmp(ban.ClientName,name))
						{
							SaveBan(ban,true);
							it=Banned.erase(it);
							resave=true;
						}
						else ++it;
					}
				}

				if(resave)
				{
					SaveBans();
					cl->Send_Text(cl,"User unbanned.",SAY_NETMSG);
				}
				else
				{
					cl->Send_Text(cl,"User not found.",SAY_NETMSG);
				}
			}
			else
			{
				cl->Send_Text(cl,"Unknown option.",SAY_NETMSG);
			}
		}
		break;
/************************************************************************/
/* DELETE_ACCOUNT                                                       */
/************************************************************************/
	case CMD_DELETE_ACCOUNT:
		{
			char pass[MAX_NAME+1];
			cl->Bin.Pop(pass,MAX_NAME);
			pass[MAX_NAME]=0;

			if(!FLAG(cl->Access,CMD_DELETE_ACCOUNT_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			if(strcmp(cl->Pass,pass)) cl->Send_Text(cl,"Invalid password.",SAY_NETMSG);
			else
			{
				if(!cl->Data.ClientToDelete)
				{
					cl->Data.ClientToDelete=true;
					cl->Send_Text(cl,"Your account will be deleted after character exit from game.",SAY_NETMSG);
				}
				else
				{
					cl->Data.ClientToDelete=false;
					cl->Send_Text(cl,"Deleting canceled.",SAY_NETMSG);
				}
			}
		}
		break;
/************************************************************************/
/* CHANGE_PASSWORD                                                      */
/************************************************************************/
	case CMD_CHANGE_PASSWORD:
		{
			char pass[MAX_NAME+1];
			char new_pass[MAX_NAME+1];
			cl->Bin.Pop(pass,MAX_NAME);
			pass[MAX_NAME]=0;
			cl->Bin.Pop(new_pass,MAX_NAME);
			new_pass[MAX_NAME]=0;

			if(!FLAG(cl->Access,CMD_CHANGE_PASSWORD_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			DWORD pass_len=strlen(new_pass);
			if(strcmp(cl->Pass,pass)) cl->Send_Text(cl,"Invalid current password.",SAY_NETMSG);
			else if(pass_len<MIN_NAME || pass_len<GameOpt.MinNameLength || pass_len>MAX_NAME || pass_len>GameOpt.MaxNameLength || !CheckUserPass(new_pass)) cl->Send_Text(cl,"Invalid new password.",SAY_NETMSG);
			else
			{
				ClientData* data=GetClientData(cl->GetId());
				if(data)
				{
					StringCopy(data->ClientPass,new_pass);
					StringCopy(cl->Pass,new_pass);
					cl->Send_Text(cl,"Password changed.",SAY_NETMSG);
				}
			}
		}
		break;
/************************************************************************/
/* DROP_UID                                                             */
/************************************************************************/
	case CMD_DROP_UID:
		{
			if(!FLAG(cl->Access,CMD_DROP_UID_ACCESS))
			{
				cl->Send_Text(cl,"Access denied.",SAY_NETMSG);
				return;
			}

			ClientData* data=GetClientData(cl->GetId());
			if(data)
			{
				for(int i=0;i<5;i++) data->UID[i]=0;
				data->UIDEndTick=0;
			}
			cl->Send_Text(cl,"UID dropped, you can relogin on another account without timeout.",SAY_NETMSG);
		}
		break;
/************************************************************************/
/*                                                                      */
/************************************************************************/
	default:
		cl->Send_Text(cl,"Unknown Command.",SAY_NETMSG);
		return;
	}
}

void FOServer::SaveGameInfoFile()
{
	AddWorldSaveData(&GameOpt.Year,sizeof(GameOpt.Year));
	AddWorldSaveData(&GameOpt.Month,sizeof(GameOpt.Month));
	AddWorldSaveData(&GameOpt.Day,sizeof(GameOpt.Day));
	AddWorldSaveData(&GameOpt.Hour,sizeof(GameOpt.Hour));
	AddWorldSaveData(&GameOpt.Minute,sizeof(GameOpt.Minute));
	AddWorldSaveData(&GameOpt.TimeMultiplier,sizeof(GameOpt.TimeMultiplier));
	AddWorldSaveData(&BestScores[0],sizeof(BestScores));
}

void FOServer::LoadGameInfoFile(FILE* f, DWORD version)
{
	if(version>=WORLD_SAVE_V4)
	{
		fread(&GameOpt.Year,sizeof(GameOpt.Year),1,f);
		fread(&GameOpt.Month,sizeof(GameOpt.Month),1,f);
		fread(&GameOpt.Day,sizeof(GameOpt.Day),1,f);
		fread(&GameOpt.Hour,sizeof(GameOpt.Hour),1,f);
		fread(&GameOpt.Minute,sizeof(GameOpt.Minute),1,f);
		fread(&GameOpt.TimeMultiplier,sizeof(GameOpt.TimeMultiplier),1,f);
	}
	else
	{
		BYTE month,day,hour,minute,tmul;
		fread(&GameOpt.Year,sizeof(GameOpt.Year),1,f);
		fread(&month,sizeof(month),1,f);
		fread(&day,sizeof(day),1,f);
		fread(&hour,sizeof(hour),1,f);
		fread(&minute,sizeof(minute),1,f);
		fread(&tmul,sizeof(tmul),1,f);
		GameOpt.Month=month;
		GameOpt.Day=day;
		GameOpt.Hour=hour;
		GameOpt.Minute=minute;
		GameOpt.TimeMultiplier=tmul;
	}

	ZeroMemory(&BestScores[0],sizeof(BestScores));
	if(version==WORLD_SAVE_V1)
	{
		for(int i=0;i<SCORES_MAX;i++)
		{
			DWORD id;
			char name[MAX_NAME+1];
			char padding[3];
			int val;
			fread(&id,sizeof(id),1,f);
			fread(name,sizeof(name),1,f);
			fread(padding,sizeof(padding),1,f);
			fread(&val,sizeof(val),1,f);
			BestScores[i].ClientId=id;
			StringCopy(BestScores[i].ClientName,name);
			BestScores[i].Value=val;
		}
	}
	else if(version>=WORLD_SAVE_V2)
	{
		fread(&BestScores[0],sizeof(BestScores),1,f);
	}
}

void FOServer::InitGameTime()
{
	if(!GameOpt.TimeMultiplier)
	{
		if(Script::PrepareContext(ServerFunctions.GetStartTime,CALL_FUNC_STR,"Game"))
		{
			Script::SetArgAddress(&GameOpt.TimeMultiplier);
			Script::SetArgAddress(&GameOpt.Year);
			Script::SetArgAddress(&GameOpt.Month);
			Script::SetArgAddress(&GameOpt.Day);
			Script::SetArgAddress(&GameOpt.Hour);
			Script::SetArgAddress(&GameOpt.Minute);
			Script::RunPrepared();
		}

		GameOpt.Year=CLAMP(GameOpt.Year,1700,30000);
	}

	GameOpt.TimeMultiplier=CLAMP(GameOpt.TimeMultiplier,1,99);
	GameOpt.Year=CLAMP(GameOpt.Year,1601,30827);
	GameOpt.Month=CLAMP(GameOpt.Month,1,12);
	GameOpt.Day=CLAMP(GameOpt.Day,1,31);
	GameOpt.Hour=CLAMP(GameOpt.Hour,0,23);
	GameOpt.Minute=CLAMP(GameOpt.Minute,0,59);
	GameOpt.FullMinute=GetFullMinute(GameOpt.Year,GameOpt.Month,GameOpt.Day,GameOpt.Hour,GameOpt.Minute);
	GameTimeStartTick=Timer::FastTick();
	GameTimeStartMinute=GameOpt.Minute;
}

void FOServer::ProcessGameTime()
{
	DWORD delta_tick=Timer::FastTick()-GameTimeStartTick;
	DWORD minute=(delta_tick/1000*GameOpt.TimeMultiplier/60+GameTimeStartMinute)%60;
	if(minute==GameOpt.Minute) return;

	GameOpt.Minute++;
	if(GameOpt.Minute>59)
	{
		GameOpt.Minute=0;
		GameOpt.Hour++;
		if(GameOpt.Hour>23)
		{
			GameOpt.Hour=0;
			GameOpt.Day++;
			if(GameOpt.Day>GameTimeMonthDay(GameOpt.Year,GameOpt.Month))
			{
				GameOpt.Day=1;
				GameOpt.Month++;
				if(GameOpt.Month>12)
				{
					GameOpt.Month=1;
					GameOpt.Year++;
				}
			}
		}
	}

	GameOpt.FullMinute=GetFullMinute(GameOpt.Year,GameOpt.Month,GameOpt.Day,GameOpt.Hour,GameOpt.Minute);
}

bool FOServer::Init()
{
	if(Active) return true;
	Active=0;

	IniParser cfg;
	cfg.LoadFile(SERVER_CONFIG_FILE);

	WriteLog("***   Starting initialization   ****\n");
	/*WriteLog("FOServer<%u>.\n",sizeof(FOServer));
	WriteLog("MapMngr<%u>.\n",sizeof(CMapMngr));
	WriteLog("ItemMngr<%u>.\n",sizeof(ItemManager));
	WriteLog("VarMngr<%u>.\n",sizeof(CVarMngr));
	WriteLog("MrFixit<%u>.\n",sizeof(CraftManager));
	WriteLog("Client<%u>.\n",sizeof(Client));
	WriteLog("Npc<%u>.\n",sizeof(Npc));
	WriteLog("Location<%u>.\n",sizeof(Location));
	WriteLog("Map<%u>.\n",sizeof(Map));
	WriteLog("Item<%u>.\n",sizeof(Item));
	WriteLog("Item::ItemData<%u>.\n",sizeof(Item::ItemData));
	WriteLog("CScriptString<%u>.\n",sizeof(CScriptString));
	WriteLog("string<%u>.\n",sizeof(string));*/

	// Dump
#ifdef FOSERVER_DUMP
	hDump=CreateFile(".\\dump.dat",GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,0,NULL);
#endif

	// Check the sizes of struct and classes
	STATIC_ASSERT(offsetof(ProtoItem,Weapon.Weapon_Aim)==182);
	STATIC_ASSERT(offsetof(GameVar,RefCount)==22);
	STATIC_ASSERT(offsetof(TemplateVar,Flags)==76);
	STATIC_ASSERT(offsetof(AIDataPlane,RefCounter)==84);
	STATIC_ASSERT(offsetof(GlobalMapGroup,EncounterForce)==84);
	STATIC_ASSERT(offsetof(Item,PLexems)==128);
	STATIC_ASSERT(offsetof(Critter::CrTimeEvent,Identifier)==12);
	STATIC_ASSERT(offsetof(Critter,RefCounter)==9768);
	STATIC_ASSERT(offsetof(Client,LanguageMsg)==9836);
	STATIC_ASSERT(offsetof(Npc,Reserved)==9800);
	STATIC_ASSERT(offsetof(MapObject,RunTime.RefCounter)==244);
	STATIC_ASSERT(offsetof(ProtoMap::MapEntire,Dir)==8);
	STATIC_ASSERT(offsetof(ScenToSend,PicMapHash)==24);
	STATIC_ASSERT(offsetof(ProtoMap,HexFlags)==320);
	STATIC_ASSERT(offsetof(Map,RefCounter)==766);
	STATIC_ASSERT(offsetof(GlobalMapZone,Reserved)==20);
	STATIC_ASSERT(offsetof(ProtoLocation,GeckEnabled)==92);
	STATIC_ASSERT(offsetof(Location,RefCounter)==286);

	STATIC_ASSERT(sizeof(DWORD)==4);
	STATIC_ASSERT(sizeof(WORD)==2);
	STATIC_ASSERT(sizeof(BYTE)==1);
	STATIC_ASSERT(sizeof(int)==4);
	STATIC_ASSERT(sizeof(short)==2);
	STATIC_ASSERT(sizeof(char)==1);
	STATIC_ASSERT(sizeof(bool)==1);
	STATIC_ASSERT(sizeof(string)==28);
	STATIC_ASSERT(sizeof(IntVec)==16);
	STATIC_ASSERT(sizeof(IntMap)==12);
	STATIC_ASSERT(sizeof(IntSet)==12);
	STATIC_ASSERT(sizeof(IntPair)==8);
	STATIC_ASSERT(sizeof(Item::ItemData)==92);
	STATIC_ASSERT(sizeof(MapObject)==MAP_OBJECT_SIZE+sizeof(MapObject::_RunTime));
	STATIC_ASSERT(sizeof(ScenToSend)==32);
	STATIC_ASSERT(sizeof(NpcBagItem)==16);
	STATIC_ASSERT(sizeof(CritData)==7404);
	STATIC_ASSERT(sizeof(CritDataExt)==6944);
	STATIC_ASSERT(sizeof(GameVar)==24);
	STATIC_ASSERT(sizeof(ProtoItem)==184);

	// Critters parameters
	Critter::SendDataCallback=&Net_Output;
	Critter::ParamsSendMsgLen=sizeof(Critter::ParamsSendCount);
	Critter::ParamsSendCount=0;
	ZeroMemory(Critter::ParamsSendEnabled,sizeof(Critter::ParamsSendEnabled));
	ZeroMemory(Critter::ParamsChangeScript,sizeof(Critter::ParamsChangeScript));
	ZeroMemory(Critter::ParamsGetScript,sizeof(Critter::ParamsGetScript));
	ZeroMemory(Critter::SlotDataSendEnabled,sizeof(Critter::SlotDataSendEnabled));

	// Accesses
	if(cfg.IsLoaded())
	{
		char buf[2048];
		AccessClient.clear();
		AccessTester.clear();
		AccessModer.clear();
		AccessAdmin.clear();
		if(cfg.GetStr("Access_client","",buf)) Str::ParseLine(buf,' ',AccessClient,Str::ParseLineDummy);
		if(cfg.GetStr("Access_tester","",buf)) Str::ParseLine(buf,' ',AccessTester,Str::ParseLineDummy);
		if(cfg.GetStr("Access_moder","",buf)) Str::ParseLine(buf,' ',AccessModer,Str::ParseLineDummy);
		if(cfg.GetStr("Access_admin","",buf)) Str::ParseLine(buf,' ',AccessAdmin,Str::ParseLineDummy);
	}

	// Generic
	FileManager::SetDataPath(".\\"); // File manager
	FONames::GenerateFoNames(PT_SERVER_DATA); // Generate name of defines
	if(!InitScriptSystem()) goto label_Error; // Script system
	if(!InitLangPacks(LangPacks)) goto label_Error; // Langpacks
	if(!InitLangScript(LangPacks)) goto label_Error; // Client scripts
	if(!LoadClientsData()) goto label_Error;
	ConnectedClients.clear();
	ConnectedClients.reserve(MAX_CLIENTS_IN_GAME);
	InitializeCriticalSection(&CSConnectedClients);
	SaveClients.clear();
	SaveClients.reserve(MAX_CLIENTS_IN_GAME);
	LastHoloId=USER_HOLO_START_NUM;
	RegIp.clear();
	LoadBans();

	// Managers
	if(!AIMngr.Init()) goto label_Error; // NpcAi manager
	if(!ItemMngr.Init()) goto label_Error; // Item manager
	if(!CrMngr.Init()) goto label_Error; // Critter manager
	if(!MapMngr.Init()) goto label_Error; // Map manager
	if(!VarMngr.Init(FileManager::GetFullPath("",PT_SCRIPTS))) goto label_Error; // Var Manager (only before dialog manager!)
	if(!DlgMngr.LoadDialogs(DIALOGS_PATH,DIALOGS_LST_NAME)) goto label_Error; // Dialog manager
	if(!InitLangPacksDialogs(LangPacks)) goto label_Error; // Create FONPC.MSG, FODLG.MSG, need call after InitLangPacks and DlgMngr.LoadDialogs
	if(!InitCrafts(LangPacks)) goto label_Error; // MrFixit
	if(!InitLangCrTypes(LangPacks)) goto label_Error; // Critter types
	if(!MapMngr.RefreshGmMask("wm.msk")) goto label_Error; // Load gm mask

	// Prototypes
	if(!ItemMngr.LoadProtos()) goto label_Error; // Proto items
	if(!CrMngr.LoadProtos()) goto label_Error; // Proto critters
	if(!MapMngr.LoadMapsProtos()) goto label_Error; // Proto maps
	if(!MapMngr.LoadLocationsProtos()) goto label_Error; // Proto locations

	// Copy of data
	if(!LoadWorld()) goto label_Error;
	VarsGarbarger(0);
	InitGameTime();
	if(!ItemMngr.CheckProtoFunctions()) goto label_Error; // Check valid of proto functions
	if(!TransferAllNpc()) goto label_Error; // Transfer critter copies to maps
	if(!TransferAllItems()) goto label_Error; // Transfer items copies to critters and maps
	MapMngr.RunInitScriptMaps(); // Init scripts for maps
	CrMngr.RunInitScriptCritters(); // Init scripts for critters
	ItemMngr.RunInitScriptItems(); // Init scripts for maps

	// Try generate world if not exist
	if(!MapMngr.GenerateWorld("GenerateWorld.cfg",PT_MAPS)) goto label_Error;

	// End of initialization
	Statistics.BytesSend=0;
	Statistics.BytesRecv=0;
	Statistics.DataReal=1;
	Statistics.DataCompressed=1;
	Statistics.ServerStartTick=Timer::FastTick();

#ifndef FONLINE_SINGLE
	// Net
	WSADATA wsa;
	if(WSAStartup(MAKEWORD(2,2),&wsa))
	{
		WriteLog("WSAStartup error!");
		goto label_Error;
	}

	ListenSock=WSASocket(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_OVERLAPPED);

	UINT port;
	port=cfg.GetInt("Port",4000);

	SOCKADDR_IN sin;
	sin.sin_family=AF_INET;
	sin.sin_port=htons(port);
	sin.sin_addr.s_addr=INADDR_ANY;

	WriteLog("Starting server on port<%u>.\n",port);

	if(bind(ListenSock,(sockaddr*)&sin,sizeof(sin))==SOCKET_ERROR)
	{
		WriteLog("Bind error!");
		goto label_Error;
	}

	if(listen(ListenSock,SOMAXCONN)==SOCKET_ERROR)
	{
		WriteLog("Listen error!");
		goto label_Error;
	}

	WorkThreadCount=cfg.GetInt("NetWorkThread",0);
	if(!WorkThreadCount)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		WorkThreadCount=si.dwNumberOfProcessors;
	}

	IOCompletionPort=CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,NULL,WorkThreadCount);
	if(!IOCompletionPort)
	{
		WriteLog("Can't create IO Completion Port, error<%u>.\n",GetLastError());
		goto label_Error;
	}

	IOThreadHandles=new HANDLE[WorkThreadCount+1];
	WriteLog("Starting net listen thread.\n");
	IOThreadHandles[0]=(HANDLE)_beginthreadex(NULL,0,Net_Listen,(void*)IOCompletionPort,0,NULL);
	WriteLog("Starting net work threads, count<%u>.\n",WorkThreadCount);
	for(DWORD i=0;i<WorkThreadCount;i++) IOThreadHandles[i+1]=(HANDLE)_beginthreadex(NULL,0,Net_Work,(void*)IOCompletionPort,0,NULL);
#endif // !FONLINE_SINGLE

	// Start script
	if(!Script::PrepareContext(ServerFunctions.Start,CALL_FUNC_STR,"Game") || !Script::RunPrepared() || !Script::GetReturnedBool())
	{
		WriteLog(__FUNCTION__" - Start script fail.\n");
		goto label_Error;
	}

	// Process command line definitions
	const char* cmd_line=GetCommandLine();
	asIScriptEngine* engine=Script::GetEngine();
	for(int i=0,j=engine->GetGlobalPropertyCount();i<j;i++)
	{
		const char* name;
		int type_id;
		bool is_const;
		const char* config_group;
		void* pointer;
		if(engine->GetGlobalPropertyByIndex(i,&name,&type_id,&is_const,&config_group,&pointer)>=0)
		{
			const char* cmd_name=strstr(cmd_line,name);
			if(cmd_name)
			{
				const char* type_decl=engine->GetTypeDeclaration(type_id);
				if(!strcmp(type_decl,"bool"))
				{
					*(bool*)pointer=atoi(cmd_name+strlen(name)+1)!=0;
					WriteLog("Global var<%s> changed to<%s>.\n",name,*(bool*)pointer?"true":"false");
				}
				else if(!strcmp(type_decl,"string"))
				{
					*(string*)pointer=cmd_name+strlen(name)+1;
					WriteLog("Global var<%s> changed to<%s>.\n",name,(*(string*)pointer).c_str());
				}
				else
				{
					*(int*)pointer=atoi(cmd_name+strlen(name)+1);
					WriteLog("Global var<%s> changed to<%d>.\n",name,*(int*)pointer);
				}
			}
		}
	}

	ScriptSystemUpdate();

	// World saver
	if(WorldSaveManager)
	{
		DumpBeginEvent=CreateEvent(NULL,FALSE,FALSE,NULL);
		DumpEndEvent=CreateEvent(NULL,FALSE,TRUE,NULL);
		DumpThreadHandle=(HANDLE)_beginthreadex(NULL,0,Dump_Work,NULL,0,NULL);
	}
	SaveWorldTime=cfg.GetInt("WorldSaveTime",60)*60*1000;
	SaveWorldNextTick=Timer::FastTick()+SaveWorldTime;

	Active=true;
	return true;

label_Error:
	WriteLog("Initialization fail!\n");
	if(ListenSock!=INVALID_SOCKET) closesocket(ListenSock);
	return false;
}

bool FOServer::InitCrafts(LangPackVec& lang_packs)
{
	WriteLog("FixBoy load crafts...\n");
	MrFixit.Finish();

	LanguagePack* main_lang;
	for(LangPackVecIt it=lang_packs.begin(),end=lang_packs.end();it!=end;++it)
	{
		LanguagePack& lang=*it;

		if(it==lang_packs.begin())
		{
			if(!MrFixit.LoadCrafts(lang.Msg[TEXTMSG_CRAFT]))
			{
				WriteLog(__FUNCTION__" - Unable to load crafts from<%s>.\n",lang.GetPath());
				return false;
			}
			main_lang=&lang;
			continue;
		}

		CraftManager mr_fixit;
		if(!mr_fixit.LoadCrafts(lang.Msg[TEXTMSG_CRAFT]))
		{
			WriteLog(__FUNCTION__" - Unable to load crafts from<%s>.\n",lang.GetPath());
			return false;
		}

		if(!(MrFixit==mr_fixit))
		{
			WriteLog(__FUNCTION__" - Compare crafts fail. <%s>with<%s>.\n",main_lang->GetPath(),lang.GetPath());
			return false;
		}
	}
	WriteLog("FixBoy load crafts complete.\n");
	return true;
}

bool FOServer::InitLangPacks(LangPackVec& lang_packs)
{
	WriteLog("Loading language packs...\n");

	IniParser cfg;
	cfg.LoadFile(SERVER_CONFIG_FILE);
	int cur_lang=0;

	while(true)
	{
		char cur_str_lang[256];
		char lang_name[256];
		sprintf(cur_str_lang,"Language_%d",cur_lang);

		if(!cfg.GetStr(cur_str_lang,"",lang_name)) break;

		if(strlen(lang_name)!=4)
		{
			WriteLog("Language name not equal 4 digits.\n");
			return false;
		}

		DWORD pack_id=*(DWORD*)&lang_name;
		if(std::find(lang_packs.begin(),lang_packs.end(),pack_id)!=lang_packs.end())
		{
			WriteLog("Language pack<%u> is already Init.\n",cur_lang);
			return false;
		}

		LanguagePack lang;
		if(!lang.Init(Str::Format("%s%s",FileMngr.GetDataPath(),FileMngr.GetPath(PT_TXT_GAME)),*(DWORD*)&lang_name))
		{
			WriteLog("Unable to init Language pack<%u>.\n",cur_lang);
			return false;
		}

		lang_packs.push_back(lang);
		cur_lang++;
	}

	WriteLog("Loading language packs complete, loaded<%u> packs.\n",cur_lang);
	return true;
}

bool FOServer::InitLangPacksDialogs(LangPackVec& lang_packs)
{
	srand(666666);
	for(DialogPackMapIt it=DlgMngr.DialogsPacks.begin(),end=DlgMngr.DialogsPacks.end();it!=end;++it)
	{
		DialogPack* pack=(*it).second;
		for(int i=0,j=pack->TextsLang.size();i<j;i++)
		{
			for(LangPackVecIt it_=lang_packs.begin(),end_=lang_packs.end();it_!=end_;++it_)
			{
				LanguagePack& lang=*it_;
				char lang_name[5];
				*(DWORD*)&lang_name=lang.Name;
				lang_name[4]='\0';

				if(pack->TextsLang[i]!=string(lang_name)) continue;

				FOMsg* msg=pack->Texts[i];
				FOMsg* msg_dlg=&lang.Msg[TEXTMSG_DLG];

				// Npc texts
				for(int n=100;n<300;n+=10)
				{
					for(int l=0,k=msg->Count(n);l<k;l++)
						msg_dlg->AddStr(pack->PackId*1000+n,msg->GetStr(n));
				}

				// Dialogs text
				for(int i_=0;i_<pack->Dialogs.size();i_++)
				{
					Dialog& dlg=pack->Dialogs[i_];
					if(dlg.TextId<10000000) dlg.TextId=msg_dlg->AddStr(msg->GetStr((i_+1)*1000));
					else msg_dlg->AddStr(dlg.TextId,msg->GetStr((i_+1)*1000));
					for(int j_=0;j_<dlg.Answers.size();j_++)
					{
						DialogAnswer& answ=dlg.Answers[j_];
						if(answ.TextId<10000000) answ.TextId=msg_dlg->AddStr(msg->GetStr((i_+1)*1000+(j_+1)*10));
						else msg_dlg->AddStr(answ.TextId,msg->GetStr((i_+1)*1000+(j_+1)*10));
					}
				}

				// Any texts
				StringMulMap& data=msg->GetData();
				StringMulMapIt it__=data.upper_bound(99999999);
				for(;it__!=data.end();++it__) msg_dlg->AddStr(100000000+pack->PackId*100000+((*it__).first-100000000),(*it__).second);
			}
		}
	}

	for(LangPackVecIt it=lang_packs.begin(),end=lang_packs.end();it!=end;++it)
	{
		LanguagePack& lang=*it;
		lang.Msg[TEXTMSG_DLG].CalculateHash();
		//lang.Msg[TEXTMSG_DLG].SaveMsgFile(Str::Format("%d.txt",lang.Name));
	}

	srand(Timer::FastTick());
	return true;
}

void FOServer::FinishLangPacks()
{
	WriteLog("Finish lang packs...\n");
	LangPacks.clear();
	WriteLog("Finish lang packs ok.\n");
}

#pragma MESSAGE("Send bytecode, not preprocessed scripts.")
bool FOServer::InitLangScript(LangPackVec& lang_packs)
{
	FILE* f=fopen(FileManager::GetFullPath(SCRIPTS_LST,PT_SCRIPTS),"rt");
	if(!f)
	{
		WriteLog(__FUNCTION__" - File not found<%s>.\n",FileManager::GetFullPath(SCRIPTS_LST,PT_SCRIPTS));
		return false;
	}

	int module_num=1;
	int errors=0;
	char buf[1024];
	string value,config;
	Script::Undefine("__SERVER");
	while(fgets(buf,1024,f))
	{
		if(buf[0]!='@') continue;
		istrstream str(&buf[1]);
		str >> value;
		if(str.fail() || value!="client") continue;
		str >> value;
		if(str.fail() || (value!="module" && value!="bind")) continue;

		if(value=="module")
		{
			str >> value;
			if(str.fail()) continue;

			Script::Define("__CLIENT");
			char* buf=Script::Preprocess((value+".fos").c_str(),false);
			if(!buf)
			{
				WriteLog(__FUNCTION__" - Unable to preprocess client script<%s>.\n",value.c_str());
				Script::Undefine("__CLIENT");
				errors++;
				continue;
			}
			Script::Undefine("__CLIENT");

			for(LangPackVecIt it=lang_packs.begin(),end=lang_packs.end();it!=end;++it)
			{
				LanguagePack& lang=*it;
				FOMsg& msg_script=lang.Msg[TEXTMSG_INTERNAL];
				msg_script.EraseStr(module_num);
				msg_script.EraseStr(module_num+1);
				msg_script.EraseStr(module_num+2);
				msg_script.EraseStr(module_num+3);
				msg_script.AddStr(module_num,value.c_str());
				msg_script.AddBinary(module_num+1,(BYTE*)buf,strlen(buf)+1);
			}
			delete[] buf;
			module_num+=2;
		}
		else
		{
			// Make bind line
			string config_="@ client bind ";
			str >> value;
			if(str.fail()) continue;
			config_+=value+" ";
			str >> value;
			if(str.fail()) continue;
			config_+=value;
			config+=config_+"\n";
		}
	}
	Script::Define("__SERVER");
	fclose(f);

	// Add config text
	for(LangPackVecIt it=lang_packs.begin(),end=lang_packs.end();it!=end;++it)
	{
		LanguagePack& lang=*it;
		FOMsg& msg_script=lang.Msg[TEXTMSG_INTERNAL];
		msg_script.AddStr(STR_INTERNAL_SCRIPT_CONFIG,config.c_str());
	}

	// Recalculate hashes
	for(LangPackVecIt it=lang_packs.begin(),end=lang_packs.end();it!=end;++it)
	{
		LanguagePack& lang=*it;
		lang.Msg[TEXTMSG_INTERNAL].CalculateHash();
	}

	return errors==0;
}

bool FOServer::ReloadLangScript()
{
	if(InitLangScript(LangPacks))
	{
		EnterCriticalSection(&CSConnectedClients);
		for(ClVecIt it=ConnectedClients.begin(),end=ConnectedClients.end();it!=end;++it)
		{
			Client* cl_=*it;
			LangPackVecIt it_l=std::find(LangPacks.begin(),LangPacks.end(),cl_->LanguageMsg);
			if(it_l!=LangPacks.end()) Send_MsgData(cl_,cl_->LanguageMsg,TEXTMSG_INTERNAL,(*it_l).Msg[TEXTMSG_INTERNAL]);
			cl_->Send_LoadMap(NULL);
		}
		LeaveCriticalSection(&CSConnectedClients);
		return true;
	}
	return false;
}

bool FOServer::InitLangCrTypes(LangPackVec& lang_packs)
{
	FOMsg msg_crtypes;
	if(!CritType::InitFromFile(&msg_crtypes)) return false;

	for(LangPackVecIt it=lang_packs.begin(),end=lang_packs.end();it!=end;++it)
		(*it).Msg[TEXTMSG_INTERNAL]+=msg_crtypes;

	return true;
}

void FOServer::LogToClients(char* str)
{
	WORD str_len=strlen(str);
	if(str_len && str[str_len-1]=='\n') str_len--;
	if(str_len)
	{
		for(ClVecIt it=LogClients.begin();it<LogClients.end();)
		{
			Client* cl=*it;
			if(cl->IsOnline())
			{
				cl->Send_TextEx(0,str,str_len,SAY_NETMSG,10,false);
				++it;
			}
			else
			{
				cl->Release();
				it=LogClients.erase(it);
			}
		}
		if(LogClients.empty()) LogFinish(LOG_FUNC);
	}
}

DWORD FOServer::GetBanTime(ClientBanned& ban)
{
	SYSTEMTIME time;
	GetLocalTime(&time);
	int diff=Timer::GetTimeDifference(ban.EndTime,time)+1;
	return diff>0?diff:1;
}

void FOServer::ProcessBans()
{
	bool resave=false;
	SYSTEMTIME time;
	GetLocalTime(&time);
	for(ClientBannedVecIt it=Banned.begin();it!=Banned.end();)
	{
		SYSTEMTIME& ban_time=(*it).EndTime;
		if(time.wYear>=ban_time.wYear && time.wMonth>=ban_time.wMonth && time.wDay>=ban_time.wDay && time.wHour>=ban_time.wHour && time.wMinute>=ban_time.wMinute)
		{
			SaveBan(*it,true);
			it=Banned.erase(it);
			resave=true;
		}
		else ++it;
	}
	if(resave) SaveBans();
}

void FOServer::SaveBan(ClientBanned& ban, bool expired)
{
	const char* fname=(expired?BANS_FNAME_EXPIRED:BANS_FNAME_ACTIVE);
	FILE* f=NULL;
	if(fopen_s(&f,fname,"at"))
	{
		WriteLog(__FUNCTION__" - Can't open file<%s>.\n",fname);
		return;
	}

	fprintf(f,"[Ban]\n");
	if(ban.ClientName[0]) fprintf(f,"User=%s\n",ban.ClientName);
	if(ban.ClientIp) fprintf(f,"UserIp=%u\n",ban.ClientIp);
	fprintf(f,"BeginTime=%u %u %u %u %u\n",ban.BeginTime.wYear,ban.BeginTime.wMonth,ban.BeginTime.wDay,ban.BeginTime.wHour,ban.BeginTime.wMinute);
	fprintf(f,"EndTime=%u %u %u %u %u\n",ban.EndTime.wYear,ban.EndTime.wMonth,ban.EndTime.wDay,ban.EndTime.wHour,ban.EndTime.wMinute);
	if(ban.BannedBy[0]) fprintf(f,"BannedBy=%s\n",ban.BannedBy);
	if(ban.BanInfo[0]) fprintf(f,"Comment=%s\n",ban.BanInfo);
	fprintf(f,"\n");
	fclose(f);
}

void FOServer::SaveBans()
{
	FILE* f=NULL;
	if(fopen_s(&f,BANS_FNAME_ACTIVE,"wt"))
	{
		WriteLog(__FUNCTION__" - Can't open file<%s>.\n",BANS_FNAME_ACTIVE);
		return;
	}

	for(ClientBannedVecIt it=Banned.begin(),end=Banned.end();it!=end;++it)
	{
		ClientBanned& ban=*it;
		fprintf(f,"[Ban]\n");
		if(ban.ClientName[0]) fprintf(f,"User=%s\n",ban.ClientName);
		if(ban.ClientIp) fprintf(f,"UserIp=%u\n",ban.ClientIp);
		fprintf(f,"BeginTime=%u %u %u %u %u\n",ban.BeginTime.wYear,ban.BeginTime.wMonth,ban.BeginTime.wDay,ban.BeginTime.wHour,ban.BeginTime.wMinute);
		fprintf(f,"EndTime=%u %u %u %u %u\n",ban.EndTime.wYear,ban.EndTime.wMonth,ban.EndTime.wDay,ban.EndTime.wHour,ban.EndTime.wMinute);
		if(ban.BannedBy[0]) fprintf(f,"BannedBy=%s\n",ban.BannedBy);
		if(ban.BanInfo[0]) fprintf(f,"Comment=%s\n",ban.BanInfo);
		fprintf(f,"\n");
	}
	fclose(f);
}

void FOServer::LoadBans()
{
	Banned.clear();
	Banned.reserve(1000);
	IniParser bans_txt;
	if(!bans_txt.LoadFile(BANS_FNAME_ACTIVE))
	{
		WriteLog(__FUNCTION__" - Can't open file<%s>.\n",BANS_FNAME_ACTIVE);
		return;
	}

	char buf[512];
	while(bans_txt.GotoNextApp("Ban"))
	{
		ClientBanned ban;
		ZeroMemory(&ban,sizeof(ban));
		SYSTEMTIME time;
		ZeroMemory(&time,sizeof(time));
		if(bans_txt.GetStr("Ban","User","",buf)) StringCopy(ban.ClientName,buf);
		ban.ClientIp=bans_txt.GetInt("Ban","UserIp",0);
		if(bans_txt.GetStr("Ban","BeginTime","",buf) && sscanf(buf,"%u%u%u%u%u",&time.wYear,&time.wMonth,&time.wDay,&time.wHour,&time.wMinute)) ban.BeginTime=time;
		if(bans_txt.GetStr("Ban","EndTime","",buf) && sscanf(buf,"%u%u%u%u%u",&time.wYear,&time.wMonth,&time.wDay,&time.wHour,&time.wMinute)) ban.EndTime=time;
		if(bans_txt.GetStr("Ban","BannedBy","",buf)) StringCopy(ban.BannedBy,buf);
		if(bans_txt.GetStr("Ban","Comment","",buf)) StringCopy(ban.BanInfo,buf);
		Banned.push_back(ban);
	}
	ProcessBans();
}

bool FOServer::LoadClientsData()
{
	WriteLog("Indexing client data.\n");

	LastClientId=0;
	ClientsData.reserve(10000);
	WIN32_FIND_DATA fdata;
	HANDLE h=FindFirstFile(".\\save\\clients\\*.client",&fdata);
	if(h==INVALID_HANDLE_VALUE)
	{
		WriteLog("Clients data not found.\n");
		return true;
	}

	DwordSet id_already;
	while(true)
	{
		// Take name from file title
		char name[64];
		StringCopy(name,fdata.cFileName);
		*strstr(name,".client")=0;

		// Take id and password from file
		char fname_[64];
		sprintf(fname_,".\\save\\clients\\%s",fdata.cFileName);
		FILE* f=NULL;
		if(fopen_s(&f,fname_,"rb"))
		{
			WriteLog("Unable to open client save file<%s>.\n",fname_);
			return false;
		}
		DWORD id=0;
		char pass[MAX_NAME+1]={0};
		fread(&id,sizeof(id),1,f);
		fread(pass,sizeof(pass),1,f);
		fclose(f);

		DWORD pass_len=strlen(pass);
		if(!IS_USER_ID(id) || pass_len<MIN_NAME || pass_len<GameOpt.MinNameLength || pass_len>MAX_NAME || pass_len>GameOpt.MaxNameLength || !CheckUserPass(pass))
		{
			WriteLog("Wrong id<%u> or password<%s> of client<%s>. Skip.\n",id,pass,name);
			if(!FindNextFile(h,&fdata)) break;
			continue;
		}

		if(id_already.count(id))
		{
			WriteLog("Id<%u> of user<%s> already used by another client.\n",id,name);
			return false;
		}
		id_already.insert(id);

		// Add
		//WriteLog("Indexing<%s> -> id<%u> name<%s> pass<%s>.\n",fdata.cFileName,id,name,pass);
		ClientData data;
		data.Clear();
		StringCopy(data.ClientName,name);
		StringCopy(data.ClientPass,pass);
		data.ClientId=id;
		ClientsData.push_back(data);
		if(id>LastClientId) LastClientId=id;

		if(!FindNextFile(h,&fdata)) break;
	}

	FindClose(h);
	if(ClientsData.size()>10000) ClientsData.reserve(ClientsData.size()*2);
	WriteLog("Indexing complete, clients found<%u>.\n",ClientsData.size());
	return true;
}

bool FOServer::SaveClient(Client* cl, bool deferred)
{
	if(!strcmp(cl->Name,"err") || !cl->GetId())
	{
		WriteLog(__FUNCTION__" - Trying save not valid client.\n");
		return false;
	}

	CritDataExt* data_ext=cl->GetDataExt();
	if(!data_ext)
	{
		WriteLog(__FUNCTION__" - Can't get extended critter data.\n");
		return false;
	}

	if(deferred && WorldSaveManager)
	{
		AddClientSaveData(cl);
	}
	else
	{
		char fname[64];
		sprintf(fname,".\\save\\clients\\%s.client",cl->Name);
		FILE* f=NULL;
		if(fopen_s(&f,fname,"wb"))
		{
			WriteLog(__FUNCTION__" - Unable to open client save file<%s>.\n",fname);
			return false;
		}

		_fwrite_nolock(&cl->Data.Id,sizeof(cl->Data.Id),1,f);
		_fwrite_nolock(cl->Pass,sizeof(cl->Pass),1,f);
		_fwrite_nolock(&cl->Data,sizeof(cl->Data),1,f);
		_fwrite_nolock(data_ext,sizeof(CritDataExt),1,f);
		DWORD te_count=cl->CrTimeEvents.size();
		_fwrite_nolock(&te_count,sizeof(te_count),1,f);
		if(te_count) _fwrite_nolock(&cl->CrTimeEvents[0],te_count*sizeof(Critter::CrTimeEvent),1,f);
		fclose(f);
	}
	return true;
}

bool FOServer::LoadClient(Client* cl)
{
	CritDataExt* data_ext=cl->GetDataExt();
	if(!data_ext)
	{
		WriteLog(__FUNCTION__" - Can't get extended critter data.\n");
		return false;
	}

	char fname[64];
	sprintf(fname,".\\save\\clients\\%s.client",cl->Name);
	FILE* f=NULL;
	if(fopen_s(&f,fname,"rb"))
	{
		WriteLog(__FUNCTION__" - Unable to open client save file<%s>.\n",fname);
		return false;
	}

	DWORD id=-2;
	fread(&id,sizeof(id),1,f);
	fread(cl->Pass,sizeof(cl->Pass),1,f);
	fread(&cl->Data,sizeof(cl->Data),1,f);
	fread(data_ext,sizeof(CritDataExt),1,f);
	DWORD te_count;
	fread(&te_count,sizeof(te_count),1,f);
	if(te_count)
	{
		cl->CrTimeEvents.resize(te_count);
		fread(&cl->CrTimeEvents[0],te_count*sizeof(Critter::CrTimeEvent),1,f);
	}
	fclose(f);

	if(id!=cl->Data.Id)
	{
		WriteLog(__FUNCTION__" - Client save file<%s> truncated.\n",fname);
		return false;
	}
	return true;
}

void FOServer::SaveWorld()
{
	double tick;
	if(WorldSaveManager)
	{
		// Be sure what Dump_Work thread in wait state
		WaitForSingleObject(DumpEndEvent,INFINITE);
		tick=Timer::AccurateTick();
		WorldSaveDataBufCount=0;
		WorldSaveDataBufFreeSize=0;
		ClientsSaveDataCount=0;
	}
	else
	{
		// Save directly to file
		tick=Timer::AccurateTick();
		char fname[64];
		sprintf(fname,".\\save\\world%04d.fo",SaveWorldIndex+1);
		DumpFile=NULL;
		if(fopen_s(&DumpFile,fname,"wb"))
		{
			WriteLog("Can't create dump file<%s>.\n",fname);
			return;
		}
	}

	// Version
	DWORD version=WORLD_SAVE_LAST;
	AddWorldSaveData(&version,sizeof(version));

	// ServerFunctions.SaveWorld
	SaveWorldDeleteIndexes.clear();
	if(Script::PrepareContext(ServerFunctions.WorldSave,CALL_FUNC_STR,"Game"))
	{
		asIScriptArray* delete_indexes=Script::CreateArray("uint[]");
		Script::SetArgDword(SaveWorldIndex+1);
		Script::SetArgObject(delete_indexes);
		if(Script::RunPrepared()) Script::AssignScriptArrayInVector(SaveWorldDeleteIndexes,delete_indexes);
		delete_indexes->Release();
	}

	// SaveAllLocationsAndMapsFile
	MapMngr.SaveAllLocationsAndMapsFile(AddWorldSaveData);

	// SaveCrittersFile
	CrMngr.SaveCrittersFile(AddWorldSaveData);

	// SaveAllItemsFile
	ItemMngr.SaveAllItemsFile(AddWorldSaveData);

	// SaveVarsDataFile
	VarMngr.SaveVarsDataFile(AddWorldSaveData);

	// SaveHoloInfoFile
	SaveHoloInfoFile();

	// SaveAnyDataFile
	SaveAnyDataFile();

	// SaveTimeEventsFile
	SaveTimeEventsFile();

	// SaveGameInfoFile
	SaveGameInfoFile();

	// SaveScriptFunctionsFile
	SaveScriptFunctionsFile();
	AddWorldSaveData(&version,sizeof(version));

	// SaveClient
	EnterCriticalSection(&CSConnectedClients);
	for(ClVecIt it=ConnectedClients.begin(),end=ConnectedClients.end();it!=end;++it)
	{
		Client* cl=*it;
		if(cl->GetId()) SaveClient(cl,true);
	}
	LeaveCriticalSection(&CSConnectedClients);
	for(ClVecIt it=SaveClients.begin(),end=SaveClients.end();it!=end;++it)
	{
		Client* cl=*it;
		SaveClient(cl,true);
		cl->Release();
	}
	SaveClients.clear();

	// Finish collect data
	if(WorldSaveManager)
	{
		// Awake Dump_Work
		SetEvent(DumpBeginEvent);
	}
	else
	{
		_fclose_nolock(DumpFile);
		DumpFile=NULL;
		SaveWorldIndex++;
		if(SaveWorldIndex>=WORLD_SAVE_MAX_INDEX) SaveWorldIndex=0;
	}

	WriteLog("World saved in %g ms.\n",Timer::AccurateTick()-tick);
}

bool FOServer::LoadWorld()
{
	FILE* f=NULL;
	for(int i=WORLD_SAVE_MAX_INDEX;i>=1;i--)
	{
		char fname[64];
		sprintf(fname,".\\save\\world%04d.fo",i);

		if(!fopen_s(&f,fname,"rb"))
		{
			WriteLog("Begin load from world file<%s>.\n",fname);
			SaveWorldIndex=i;
			break;
		}
		f=NULL;
	}

	if(!f)
	{
		WriteLog("World dump file not found.\n");
		SaveWorldIndex=0;
		return true;
	}

	DWORD version=0;
	fread(&version,sizeof(version),1,f);
	if(version!=WORLD_SAVE_V1 && version!=WORLD_SAVE_V2 && version!=WORLD_SAVE_V3 && version!=WORLD_SAVE_V4 &&
		version!=WORLD_SAVE_V5 && version!=WORLD_SAVE_V6 && version!=WORLD_SAVE_V7)
	{
		WriteLog("Unknown version<%u> of world dump file.\n",version);
		fclose(f);
		return false;
	}
	if(version<WORLD_SAVE_V6)
	{
		WriteLog("Version of save file is not supported.\n");
		fclose(f);
		return false;
	}
	if(!MapMngr.LoadAllLocationsAndMapsFile(f)) return false;
	if(!CrMngr.LoadCrittersFile(f)) return false;
	if(!ItemMngr.LoadAllItemsFile(f,version)) return false;
	if(!VarMngr.LoadVarsDataFile(f)) return false;
	LoadHoloInfoFile(f);
	LoadAnyDataFile(f);
	LoadTimeEventsFile(f);
	LoadGameInfoFile(f,version);
	LoadScriptFunctionsFile(f);
	DWORD version_=0;
	fread(&version_,sizeof(version_),1,f);
	if(version!=version_)
	{
		WriteLog("World dump file truncated.\n");
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

void FOServer::AddWorldSaveData(void* data, size_t size)
{
	if(!WorldSaveManager)
	{
		_fwrite_nolock(data,size,1,DumpFile);
		return;
	}

	if(!WorldSaveDataBufFreeSize)
	{
		WorldSaveDataBufCount++;
		WorldSaveDataBufFreeSize=WORLD_SAVE_DATA_BUFFER_SIZE;

		if(WorldSaveDataBufCount>=WorldSaveData.size())
		{
			MEMORY_PROCESS(MEMORY_SAVE_DATA,WORLD_SAVE_DATA_BUFFER_SIZE);
			WorldSaveData.push_back(new BYTE[WORLD_SAVE_DATA_BUFFER_SIZE]);
		}
	}

	size_t flush=(size<=WorldSaveDataBufFreeSize?size:WorldSaveDataBufFreeSize);
	if(flush)
	{
		BYTE* ptr=WorldSaveData[WorldSaveDataBufCount-1];
		memcpy(&ptr[WORLD_SAVE_DATA_BUFFER_SIZE-WorldSaveDataBufFreeSize],data,flush);
		WorldSaveDataBufFreeSize-=flush;
		if(!WorldSaveDataBufFreeSize) AddWorldSaveData(((BYTE*)data)+flush,size-flush);
	}
}

void FOServer::AddClientSaveData(Client* cl)
{
	if(ClientsSaveDataCount>=ClientsSaveData.size())
	{
		ClientsSaveData.push_back(ClientSaveData());
		MEMORY_PROCESS(MEMORY_SAVE_DATA,sizeof(ClientSaveData));
	}

	ClientSaveData& csd=ClientsSaveData[ClientsSaveDataCount];
	csd.Id=cl->GetId();
	memcpy(csd.Name,cl->Name,sizeof(csd.Name));
	memcpy(csd.Password,cl->Pass,sizeof(csd.Password));
	memcpy(&csd.Data,&cl->Data,sizeof(cl->Data));
	memcpy(&csd.DataExt,cl->GetDataExt(),sizeof(CritDataExt));
	csd.TimeEvents=cl->CrTimeEvents;

	ClientsSaveDataCount++;
}

unsigned int __stdcall FOServer::Dump_Work(void* data)
{
	char fname[MAX_FOPATH];

	while(true)
	{
		if(WaitForSingleObject(DumpBeginEvent,INFINITE)!=WAIT_OBJECT_0) break;

		// Save world data
		FILE* fworld=NULL;
		sprintf(fname,".\\save\\world%04d.fo",SaveWorldIndex+1);
		if(!fopen_s(&fworld,fname,"wb"))
		{
			for(size_t i=0;i<WorldSaveDataBufCount;i++)
			{
				BYTE* ptr=WorldSaveData[i];
				size_t flush=WORLD_SAVE_DATA_BUFFER_SIZE;
				if(i==WorldSaveDataBufCount-1) flush-=WorldSaveDataBufFreeSize;
				fwrite(ptr,flush,1,fworld);
				Sleep(10);
			}
			fclose(fworld);
			SaveWorldIndex++;
			if(SaveWorldIndex>=WORLD_SAVE_MAX_INDEX) SaveWorldIndex=0;
		}
		else
		{
			WriteLog(__FUNCTION__" - Can't create world dump file<%s>.\n",fname);
		}

		// Save clients data
		for(size_t i=0;i<ClientsSaveDataCount;i++)
		{
			ClientSaveData& csd=ClientsSaveData[i];

			FILE* fc=NULL;
			sprintf(fname,".\\save\\clients\\%s.client",csd.Name);
			if(fopen_s(&fc,fname,"wb"))
			{
				WriteLog(__FUNCTION__" - Unable to open client save file<%s>.\n",fname);
				continue;
			}

			fwrite(&csd.Id,sizeof(csd.Id),1,fc);
			fwrite(csd.Password,sizeof(csd.Password),1,fc);
			fwrite(&csd.Data,sizeof(csd.Data),1,fc);
			fwrite(&csd.DataExt,sizeof(csd.DataExt),1,fc);
			DWORD te_count=csd.TimeEvents.size();
			fwrite(&te_count,sizeof(te_count),1,fc);
			if(te_count) fwrite(&csd.TimeEvents[0],te_count*sizeof(Critter::CrTimeEvent),1,fc);
			fclose(fc);
			Sleep(1);
		}

		// Clear old dump files
		for(DwordVecIt it=SaveWorldDeleteIndexes.begin(),end=SaveWorldDeleteIndexes.end();it!=end;++it)
		{
			FILE* fold=NULL;
			sprintf_s(fname,".\\save\\world%04d.fo",*it);
			if(!fopen_s(&fold,fname,"rb"))
			{
				fclose(fold);
				DeleteFile(fname);
			}
		}

		// Notify about end of processing
		SetEvent(DumpEndEvent);
	}
	return 0;
}

/************************************************************************/
/* RADIO                                                                */
/************************************************************************/
/*
const char[10][]={
"���� � ������ � �����",
"���� - ������� ��� ����������?",
"���� - ��� ��?",
"���� - ��� ��?",
"� ����� � ���, �� ����� ��� �� ���������",
"��� ��� ���-��",
"",
"",
"",
"",
};
const int HORSEMAN_TEXT_COUNT=
*/
void FOServer::RadioClearChannels()
{
	WriteLog("Clear radio channels.\n");
	for(int i=0;i<0x10000;++i) SAFEDEL(RadioChannels[i]);
	WriteLog("Clear radio channels complete.\n");
}

void FOServer::RadioAddPlayer(Client* cl, WORD channel)
{
#ifdef RADIO_SAFE
	DwordVec* cha=RadioChannels[channel];
#else
	CrVec* cha=RadioChannels[channel];
#endif

	if(!RadioChannels[channel])
	{
#ifdef RADIO_SAFE
		RadioChannels[channel]=new DwordVec;
#else
		RadioChannels[channel]=new CrVec;
#endif
		cha=RadioChannels[channel];
		if(!cha) return;
	}
	else
	{
#ifdef RADIO_SAFE
		if(std::find(cha->begin(),cha->end(),cl->GetId())!=cha->end()) return;
#else
		if(std::find(cha->begin(),cha->end(),cl)!=cha->end()) return;
#endif
	}

#ifdef RADIO_SAFE
	cha->push_back(cl->GetId());
#else
	cha->push_back(cl);
#endif
}

void FOServer::RadioErsPlayer(Client* cl, WORD channel)
{
#ifdef RADIO_SAFE
	DwordVec* cha=RadioChannels[channel];
#else
	CrVec* cha=RadioChannels[channel];
#endif

	if(!cha) return;

#ifdef RADIO_SAFE
	DwordVecIt it=std::find(cha->begin(),cha->end(),cl->GetId());
#else
	CrVecIt it=std::find(cha->begin(),cha->end(),cl);
#endif
	if(it==cha->end()) return;
	cha->erase(it);
}

void FOServer::RadioSendText(Critter* cr, WORD channel, const char* text, bool unsafe_text)
{
#ifdef RADIO_SAFE
	DwordVec* cha=RadioChannels[channel];
#else
	CrVec* cha=RadioChannels[channel];
#endif

	if(!cha) return;
	if(!text || !text[0]) return;
	WORD str_len=strlen(text);
	DWORD from_id=0;//(cr?cr->GetId():0);
	WORD intellect=(cr?cr->GetSayIntellect():0);

#ifdef RADIO_SAFE
	DwordVecIt it=cha->begin();
#else
	CrVecIt it=cha->begin();
#endif

	while(it!=cha->end())
	{
#ifdef RADIO_SAFE
		Client* send_cl=(Client*)CrMngr.GetCritter(*it);
		if(!send_cl)
		{
			it=cha->erase(it);
			continue;
		}
#else
		Client* send_cl=(Client*)(*it).second;
#endif

		if(send_cl->GetRadio() && send_cl->GetRadioChannel()==channel)
		{
			send_cl->Send_TextEx(from_id,text,str_len,SAY_RADIO,intellect,unsafe_text);
			++it;
		}
		else
		{
			it=cha->erase(it);
		}
	}
}

void FOServer::RadioSendMsg(Critter* cr, WORD channel, WORD text_msg, DWORD num_str)
{
#ifdef RADIO_SAFE
	DwordVec* cha=RadioChannels[channel];
#else
	CrVec* cha=RadioChannels[channel];
#endif
	if(!cha) return;

#ifdef RADIO_SAFE
	DwordVecIt it=cha->begin();
#else
	CrVecIt it=cha->begin();
#endif

	while(it!=cha->end())
	{
#ifdef RADIO_SAFE
		Client* send_cl=(Client*)CrMngr.GetCritter(*it);
		if(!send_cl)
		{
			it=cha->erase(it);
			continue;
		}
#else
		Client* send_cl=(Client*)(*it).second;
#endif

		if(send_cl->GetRadio() && send_cl->GetRadioChannel()==channel)
		{
			send_cl->Send_TextMsg(cr,num_str,SAY_RADIO,text_msg);
			++it;
		}
		else
		{
			it=cha->erase(it);
		}
	}
}

/************************************************************************/
/* Scores                                                               */
/************************************************************************/

void FOServer::SetScore(int score, Critter* cr, int val)
{
	cr->Data.Scores[score]+=val;
	if(BestScores[score].ClientId==cr->GetId()) return;
	if(BestScores[score].Value>=cr->Data.Scores[score]) return; //TODO: less/greater
	BestScores[score].Value=cr->Data.Scores[score];
	BestScores[score].ClientId=cr->GetId();
	StringCopy(BestScores[score].ClientName,cr->GetName());
}

void FOServer::SetScore(int score, const char* name)
{
	BestScores[score].ClientId=0;
	BestScores[score].Value=0;
	StringCopy(BestScores[score].ClientName,name);
}

const char* FOServer::GetScores()
{
	static char scores[SCORE_NAME_LEN*SCORES_MAX]; // Only names
	for(int i=0;i<SCORES_MAX;i++)
	{
		char* offs=&scores[i*SCORE_NAME_LEN]; // Name
		memcpy(offs,BestScores[i].ClientName,SCORE_NAME_LEN);
	}

	/*static char scores[(MAX_NAME+sizeof(int))*SCORES_MAX];
	for(int i=0;i<SCORES_MAX;i++)
	{
		char* offs=&scores[i*(MAX_NAME+sizeof(int))]; //Name + Value
		memcpy(offs,&BestScores[i].ClientName,MAX_NAME);
		memcpy(offs+MAX_NAME,&BestScores[i].Value,sizeof(int));
	}*/
	return scores;
}

void FOServer::ClearScore(int score)
{
	BestScores[score].ClientId=0;
	BestScores[score].ClientName[0]='\0';
	BestScores[score].Value=0;
}

void FOServer::Process_GetScores(Client* cl)
{
	if(Timer::FastTick()-SCORES_SEND_TIME<cl->LastSendScoresTick)
	{
		WriteLog(__FUNCTION__" - Client<%s> ignore send scores timeout.\n",cl->GetInfo());
		return;
	}
	cl->LastSendScoresTick=Timer::FastTick();

	MSGTYPE msg=NETMSG_SCORES;

	BOUT_BEGIN(cl);
	cl->Bout << msg;
	cl->Bout.Push(GetScores(),SCORE_NAME_LEN*SCORES_MAX);
	BOUT_END(cl);
}

/************************************************************************/
/*                                                                      */
/************************************************************************/

void FOServer::ClearUnusedVars()
{
	WriteLog("Begin clearing unused local and unicum vars.\n");

	DWORD lcount=0,ucount=0;
	DwordSet clients;
	for(ClientDataVecIt it=ClientsData.begin(),end=ClientsData.end();it!=end;++it) clients.insert((*it).ClientId);
	CrMap& npcs=CrMngr.GetCritters();
	VarsMap& vars=VarMngr.GetVars();
	VarsVec& quest_vars=VarMngr.GetQuestVars();
	for(VarsMapIt it=vars.begin();it!=vars.end();)
	{
		GameVar* var=(*it).second;
		if(var->Type==VAR_GLOBAL) { ++it; continue; }

		if(var->VarValue!=var->VarTemplate->StartVal || var->IsRandom())
		{
			if(var->Type==VAR_LOCAL)
			{
				DWORD master_id=var->GetMasterId();
				if(npcs.count(master_id) || clients.count(master_id)) { ++it; continue; }
				if(var->IsQuest()) quest_vars[var->QuestVarIndex]=NULL;
				lcount++;
			}
			else // VAR_UNICUM
			{
				DWORD master_id=var->GetMasterId();
				if(npcs.count(master_id) || clients.count(master_id)) { ++it; continue; }
				DWORD slave_id=var->GetSlaveId();
				if(npcs.count(slave_id) || clients.count(slave_id)) { ++it; continue; }
				if(var->IsQuest()) quest_vars[var->QuestVarIndex]=NULL;
				ucount++;
			}
		}
		else
		{
			if(var->IsQuest()) quest_vars[var->QuestVarIndex]=NULL;
			if(var->Type==VAR_LOCAL) lcount++;
			else ucount++; // VAR_UNICUM
		}

		var->Release();
		it=vars.erase(it);
	}

	WriteLog("Complete, erased<%u> vars (local<%u>, unicum<%u>).\n",lcount+ucount,lcount,ucount);
}

void FOServer::VarsGarbarger(DWORD cycle_tick)
{
	if(cycle_tick)
	{
		if(!VarsGarbageTime) return;

		DWORD tick=Timer::FastTick();
		if(tick-cycle_tick>100) return;
		if(tick-VarsGarbageLastTick<VarsGarbageTime) return;
	}

	ClearUnusedVars();
	VarsGarbageLastTick=Timer::FastTick();
}

/************************************************************************/
/* Time events                                                          */
/************************************************************************/
struct TimeEvent
{
	DWORD Num;
	DWORD FullMinute;
	string FuncName;
	int BindId;
	DWORD Rate;
	DwordVec Values;
	bool IsSaved;

	bool operator==(const DWORD& r){return Num==r;}
};
typedef vector<TimeEvent> TimeEventVec;
typedef vector<TimeEvent>::iterator TimeEventVecIt;
TimeEventVec TimeEvents;
DWORD TimeEventsLastNum=0;

#define TIME_EVENTS_RESERVE       (1000000/sizeof(TimeEvent)) // 1 mb
#define TIME_EVENTS_PER_CYCLE     (10)

void AddTimeEvent(TimeEvent& te)
{
	for(TimeEventVecIt it=TimeEvents.begin(),end=TimeEvents.end();it!=end;++it)
	{
		TimeEvent& te_=*it;
		if(te.FullMinute<te_.FullMinute)
		{
			TimeEvents.insert(it,te);
			return;
		}
	}
	TimeEvents.push_back(te);
}

void FOServer::SaveTimeEventsFile()
{
	DWORD count=0;
	for(TimeEventVecIt it=TimeEvents.begin(),end=TimeEvents.end();it!=end;++it) if((*it).IsSaved) count++;
	AddWorldSaveData(&count,sizeof(count));
	for(TimeEventVecIt it=TimeEvents.begin(),end=TimeEvents.end();it!=end;++it)
	{
		TimeEvent& te=*it;
		if(!te.IsSaved) continue;
		AddWorldSaveData(&te.Num,sizeof(te.Num));
		WORD script_name_len=te.FuncName.length();
		AddWorldSaveData(&script_name_len,sizeof(script_name_len));
		AddWorldSaveData((void*)te.FuncName.c_str(),script_name_len);
		AddWorldSaveData(&te.FullMinute,sizeof(te.FullMinute));
		AddWorldSaveData(&te.Rate,sizeof(te.Rate));
		DWORD values_size=te.Values.size();
		AddWorldSaveData(&values_size,sizeof(values_size));
		if(values_size) AddWorldSaveData(&te.Values[0],values_size*sizeof(DWORD));
	}
}

void FOServer::LoadTimeEventsFile(FILE* f)
{
	WriteLog("Load time events...");

	DWORD count=0;
	fread(&count,sizeof(count),1,f);
	for(DWORD i=0;i<count;i++)
	{
		DWORD num;
		fread(&num,sizeof(num),1,f);

		char script_name[MAX_SCRIPT_NAME*2+2];
		WORD script_name_len;
		fread(&script_name_len,sizeof(script_name_len),1,f);
		fread(script_name,script_name_len,1,f);
		script_name[script_name_len]=0;

		DWORD begin_minute;
		fread(&begin_minute,sizeof(begin_minute),1,f);
		DWORD rate;
		fread(&rate,sizeof(rate),1,f);

		DwordVec values;
		DWORD values_size;
		fread(&values_size,sizeof(values_size),1,f);
		if(values_size)
		{
			values.resize(values_size);
			fread(&values[0],values_size*sizeof(DWORD),1,f);
		}

		int bind_id=Script::Bind(script_name,"uint %s(uint[]@)",false);
		if(bind_id<=0)
		{
			WriteLog("Unable to bind script function, event num<%u>, name<%s>.\n",num,script_name);
			continue;
		}

		TimeEvent te;
		te.FullMinute=begin_minute;
		te.Num=num;
		te.FuncName=script_name;
		te.BindId=bind_id;
		te.IsSaved=true;
		if(values_size) te.Values=values;

		TimeEvents.push_back(te);
		if(TimeEventsLastNum<num) TimeEventsLastNum=num;
	}

	WriteLog("complete, count<%u>.\n",count);
}

DWORD FOServer::CreateScriptEvent(DWORD begin_minute, const char* script_name, DwordVec& values, bool save)
{
	if(values.size()*sizeof(DWORD)>TIME_EVENT_MAX_SIZE) SCRIPT_ERROR_R0("Values size greather than maximum.");

	char full_name[256];
	char module_name[256];
	char func_name[256];
	if(!Script::ReparseScriptName(script_name,module_name,func_name)) SCRIPT_ERROR_R0("Can't reparse script name.");
	Str::SFormat(full_name,"%s@%s",module_name,func_name);

	int bind_id=Script::Bind(module_name,func_name,"uint %s(uint[]@)",false);
	if(bind_id<=0) SCRIPT_ERROR_R0("Script function not found.");

	TimeEvent te;
	te.FullMinute=begin_minute;
	te.Num=TimeEventsLastNum+1;
	te.FuncName=full_name;
	te.BindId=bind_id;
	te.IsSaved=save;
	te.Rate=0;
	te.Values=values;

	AddTimeEvent(te);
	TimeEventsLastNum++;
	return TimeEventsLastNum;
}

void FOServer::EraseTimeEvent(DWORD num)
{
	TimeEventVecIt it=std::find(TimeEvents.begin(),TimeEvents.end(),num);
	if(it!=TimeEvents.end()) TimeEvents.erase(it);
}

void FOServer::ProcessTimeEvents()
{
	for(int i=0;!TimeEvents.empty() && TimeEvents[0].FullMinute<=GameOpt.FullMinute && i<TIME_EVENTS_PER_CYCLE;i++)
	{
		TimeEvent cur_event=TimeEvents[0]; // Copy
		TimeEvents.erase(TimeEvents.begin()); // And erase
		DWORD wait_time=0;
		if(Script::PrepareContext(cur_event.BindId,CALL_FUNC_STR,Str::Format("Time event<%u>",cur_event.Num)))
		{
			asIScriptArray* dw=NULL;
			DWORD size=cur_event.Values.size();

			if(size>0)
			{
				dw=Script::CreateArray("uint[]");
				if(!dw)
				{
					WriteLog(__FUNCTION__" - Create uint array fail. Wait 10 real minutes.\n");
					wait_time=GameOpt.TimeMultiplier*10;
				}
				else
				{
					dw->Resize(size);
					memcpy(dw->GetElementPointer(0),(void*)&cur_event.Values[0],size*sizeof(DWORD));
				}
			}

			if(!size || dw)
			{
				Script::SetArgObject(dw);
				if(!Script::RunPrepared())
				{
					WriteLog(__FUNCTION__" - RunPrepared fail. Wait 10 real minutes.\n");
					wait_time=GameOpt.TimeMultiplier*10;
				}
				else
				{
					wait_time=Script::GetReturnedDword();
					if(wait_time && dw) // Recopy array
					{
						DWORD arr_size=dw->GetElementCount();
						cur_event.Values.resize(arr_size);
						if(arr_size) memcpy(&cur_event.Values[0],dw->GetElementPointer(0),arr_size*sizeof(cur_event.Values[0]));
					}
				}

				if(dw) dw->Release();
			}
		}
		else
		{
			WriteLog(__FUNCTION__" - Game contexts prepare fail. Wait 10 real minutes.\n");
			wait_time=GameOpt.TimeMultiplier*10;
		}

		if(wait_time)
		{
			cur_event.FullMinute=GameOpt.FullMinute+wait_time;
			cur_event.Rate++;
			AddTimeEvent(cur_event);
		}
	}
}

DWORD FOServer::GetTimeEventsCount()
{
	return TimeEvents.size();
}

string FOServer::GetTimeEventsStatistics()
{
	static string result;
	char str[1024];
	sprintf(str,"Time events: %u\n",TimeEvents.size());
	result=str;
	SYSTEMTIME st=GetGameTime(GameOpt.FullMinute);
	sprintf(str,"Game time: %02u.%02u.%04u %02u:%02u\n",st.wDay,st.wMonth,st.wYear,st.wHour,st.wMinute);
	result+=str;
	result+="Number    Date       Time  Rate Saved Function                       Values\n";
	for(size_t i=0,j=TimeEvents.size();i<j;i++)
	{
		TimeEvent& te=TimeEvents[i];
		st=GetGameTime(te.FullMinute);
		sprintf(str,"%09u %02u.%02u.%04u %02u:%02u %04u %-5s %-30s",te.Num,st.wDay,st.wMonth,st.wYear,st.wHour,st.wMinute,te.Rate,te.IsSaved?"true":"false",te.FuncName.c_str());
		result+=str;
		for(size_t k=0,l=te.Values.size();k<l;k++)
		{
			sprintf(str," %-10u",te.Values[k]);
			result+=str;
		}
		result+="\n";
	}
	return result;
}

void FOServer::SaveScriptFunctionsFile()
{
	const StrVec& cache=Script::GetScriptFuncCache();
	DWORD count=cache.size();
	AddWorldSaveData(&count,sizeof(count));
	for(size_t i=0,j=cache.size();i<j;i++)
	{
		const string& func_name=cache[i];
		DWORD len=func_name.length();
		AddWorldSaveData(&len,sizeof(len));
		AddWorldSaveData((void*)func_name.c_str(),len);
	}
}

void FOServer::LoadScriptFunctionsFile(FILE* f)
{
	DWORD count=0;
	fread(&count,sizeof(count),1,f);
	for(DWORD i=0;i<count;i++)
	{
		Script::ResizeCache(i);

		char script[1024];
		DWORD len=0;
		fread(&len,sizeof(len),1,f);
		fread(script,len,1,f);
		script[len]=0;

		// Cut off decl
		char* decl=strstr(script,"|");
		if(!decl)
		{
			WriteLog(__FUNCTION__" - Function declaration not found, script<%s>.\n",script);
			continue;
		}
		*decl=0;
		decl++;

		// Parse
		if(!Script::GetScriptFuncNum(script,decl))
		{
			WriteLog(__FUNCTION__" - Function<%s,%s> not found.\n",script,decl);
			continue;
		}
	}
}

/************************************************************************/
/* Any data                                                             */
/************************************************************************/
void FOServer::SaveAnyDataFile()
{
	DWORD count=AnyData.size();
	AddWorldSaveData(&count,sizeof(count));
	for(AnyDataMapIt it=AnyData.begin(),end=AnyData.end();it!=end;++it)
	{
		const string& name=(*it).first;
		ByteVec& data=(*it).second;
		DWORD name_len=name.length();
		AddWorldSaveData(&name_len,sizeof(name_len));
		AddWorldSaveData((void*)name.c_str(),name_len);
		DWORD data_len=data.size();
		AddWorldSaveData(&data_len,sizeof(data_len));
		if(data_len) AddWorldSaveData(&data[0],data_len);
	}
}

void FOServer::LoadAnyDataFile(FILE* f)
{
	AutoPtrArr<BYTE> data(new BYTE[ANY_DATA_MAX_SIZE]);
	if(!data.IsValid()) return;

	DWORD count=0;
	fread(&count,sizeof(count),1,f);
	for(DWORD i=0;i<count;i++)
	{
		char name[ANY_DATA_MAX_NAME+1];
		DWORD name_len;
		fread(&name_len,sizeof(name_len),1,f);
		fread(name,name_len,1,f);
		name[name_len]=0;

		DWORD data_len;
		fread(&data_len,sizeof(data_len),1,f);
		if(data_len) fread(data.Get(),data_len,1,f);

		string name_=name;
		ByteVec data_;
		if(data_len)
		{
			data_.resize(data_len);
			memcpy(&data_[0],data.Get(),data_len);
		}
		AnyData.insert(AnyDataMapVal(name_,data_));
		MEMORY_PROCESS(MEMORY_ANY_DATA,data_.capacity());
	}
}

bool FOServer::SetAnyData(const string& name, const BYTE* data, DWORD data_size)
{
	AnyDataMapIt it=AnyData.find(name);
	if(it==AnyData.end())
	{
		ByteVec data_;
		data_.resize(data_size);
		memcpy(&data_[0],data,data_size);
		AnyData.insert(AnyDataMapVal(name,data_));
		MEMORY_PROCESS(MEMORY_ANY_DATA,data_.capacity());
	}
	else
	{
		ByteVec& data_=(*it).second;
		MEMORY_PROCESS(MEMORY_ANY_DATA,-(int)data_.capacity());
		data_.resize(data_size);
		memcpy(&data_[0],data,data_size);
		MEMORY_PROCESS(MEMORY_ANY_DATA,data_.capacity());
	}
	return true;
}

BYTE* FOServer::GetAnyData(const string& name, DWORD& length)
{
	AnyDataMapIt it=AnyData.find(name);
	if(it==AnyData.end()) return NULL;
	ByteVec& data=(*it).second;
	length=data.size();
	return length?&data[0]:(BYTE*)1; // If length == 0 than return dummy ptr
}

bool FOServer::IsAnyData(const string& name)
{
	AnyDataMapIt it=AnyData.find(name);
	return it!=AnyData.end();
}

void FOServer::EraseAnyData(const string& name)
{
	AnyData.erase(name);
}

string FOServer::GetAnyDataStatistics()
{
	static string result;
	char str[256];
	result="Any data count: ";
	result+=_itoa(AnyData.size(),str,10);
	result+="\nName                          Length    Data\n";
	for(AnyDataMapIt it=AnyData.begin(),end=AnyData.end();it!=end;++it)
	{
		const string& name=(*it).first;
		ByteVec& data=(*it).second;
		sprintf(str,"%-30s",name.c_str());
		result+=str;
		sprintf(str,"%-10u",data.size());
		result+=str;
		for(size_t i=0,j=data.size();i<j;i++)
		{
			sprintf(str,"%02X",data[i]);
			result+=str;
		}
		result+="\n";
	}
	return result;
}

/*
bool FOServer::LoadAnyData()
{
	WriteLog("Load Any data...\n");

	if(!Sql.Query("SELECT * FROM `any_data`;"))
	{
		WriteLog("DB select error.\n");
		return false;
	}

	int res_count;
	if((res_count=Sql.StoreResult())<0)
	{
		WriteLog("DB store result error.\n");
		return false;
	}

	if(!res_count)
	{
		WriteLog("Any data not found.\n");
		return true;
	}

	SQL_RESULT res=NULL;
	DWORD loaded=0;
	for(int i=0;i<res_count;++i)
	{
		if(!(res=Sql.GetNextResult()))
		{
			WriteLog("DB get next result error.\n");
			return false;
		}

		DWORD id=atoi(res[0]);
		DWORD count=atoi(res[1]);
		char* str_data=res[2];

		DwordVec data;
		if(count)
		{
			data.resize(count);
			memcpy(&data[0],str_data,count*sizeof(DWORD));
		}

		AnyData.insert(DwordVecMapVal(id,data));
		loaded++;
	}

	WriteLog("Loaded Any data success, count<%u>.\n",loaded);
	return true;
}*/


/************************************************************************/
/*                                                                      */
/************************************************************************/