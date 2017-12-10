#include "DtbtNginx.h"
#include "inNginx.pb.h"
#include "protoCallBack.h"
#include "Processpool.h"
#include "Nginx.h"
#include "easylogging++.h"
#include "ConsistentHash.h"

//使用宏 INITIALIZE_EASYLOGGINGPP 初始化log
INITIALIZE_EASYLOGGINGPP

DtbtNginx::DtbtNginx():status(FOLLOWER), voteNum(0){
	version[0] = 0;
	version[1] = 0;
	csshash = new ConsistentHash();
	//init time
	srand(time(0));
}
DtbtNginx::~DtbtNginx(){
	delete csshash;
}

bool DtbtNginx::ReadDtbtNginxConf(string num, string confSrc){
	if(num.empty() || confSrc.empty())
		return false;
	string hostPre("DtbtNginx");
	string lisSerPre("ListenNginx");
	string lisCliPre("ListenClient");
	ReadConf rc;
	rc.read(confSrc);
	int idx;
	for(auto m : rc.conf){
		string key = m.first;
		string val = m.second;
		idx = val.find(':');
		if(idx == val.npos){
			LOG(ERROR) << "ReadDtbtNginxConf error";
			return false;
		}
		string ip(val, 0, idx);
		string port(val, idx + 1, val.size());
		//设置本机名
		if(hostPre + num == key){
			nginxName = ip + " " + port;
			LOG(DEBUG) << "selfNginxName = " << nginxName;
		}
		else if(lisSerPre + num == key){
			lisSerName = ip + " " + port;
			LOG(DEBUG) << "lisSerName = " << lisSerName;
		}
		else if(lisCliPre + num == key){
			lisCliName = ip + " " + port;
			LOG(DEBUG) << "lisCliName = " << lisCliName;
		}
		else if(hostPre == string(key.begin(), key.end() - 1)){
			otherName.push_back(ip + " " + port);
			LOG(DEBUG) << "DtbtNginx = " << ip + " " + port;
		}
	}
	allNginxNum = otherName.size() + 1;
	return true;
}

int DtbtNginx::CreateListen(string ip, int port) {
	int ret = 0;
	int listenfd = socket(PF_INET, SOCK_STREAM, 0);
	if(listenfd < 0){
		LOG(ERROR) << "CreateListen:" << ip << " " << port;
		return -1;
	}
	/*一个端口释放后会等待两分钟之后才能再被使用，SO_REUSEADDR是让端口释放后立即就可以被再次使用。*/
    int reuse = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        return -1;
    }
    struct sockaddr_in address;
    bzero( &address, sizeof(address) );
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &address.sin_addr);
    address.sin_port = htons( port );
    
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof( address ));
    if(-1 == ret){
    	LOG(ERROR) << "bind " << ip << " " << port;
    	exit(0);
    }
    
    ret = listen( listenfd, 5 );
    if(-1 == ret){
    	LOG(ERROR) << "listen " << ip << " " << port;
    	exit(0);
    }
    return listenfd;
}
/* connect other nginxs */
void DtbtNginx::ConOtherNginx(){
	aliveNginxfd.clear();
	struct sockaddr_in servaddr;
	int sockfd;
	int size = otherName.size();
	for(int i = 0; i < size; ++i){
		int idx = otherName[i].find(' ');
		string ipStr(otherName[i], 0, idx);
		string portStr(otherName[i], idx + 1, otherName[i].size());
		const char *ip = ipStr.c_str();
		int port = atoi(portStr.c_str());
		if(0 == port){
			LOG(ERROR) << "ConOtherNginx transform port error";
			continue;
		}
		if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			LOG(ERROR) << "ConOtherNginx create socket error";
			return;
		}
		bzero(&servaddr,sizeof(servaddr));
		servaddr.sin_family = AF_INET;
		inet_pton(AF_INET, ip, &servaddr.sin_addr);
		servaddr.sin_port = htons(port);
		
		if(connect(sockfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) == -1) {
			LOG(WARNING) << "Not Connected:" << ip << ":" << port; 
		}
		else{
			//记录集群存活的节点fd
			aliveNginxfd.push_back(sockfd);
			//记录name(ip port)
			nginxs[sockfd].clientName = otherName[i];
			//记录fd
			nginxs[sockfd].sockfd = sockfd;
			//设置超时时间
			nginxs[sockfd].SetTimeout(2, 6);//心跳2s、断开6s
			//监听读事件
			nginxs[sockfd].Addfd2Read();
			
			LOG(DEBUG) << "connect -> " << otherName[i];
		}
	}
}

/* 发起投票 */
void DtbtNginx::VoteSend(){
	//vote for myself
	voteNum = 1;
	version[SUBMIT] = version[ENSURE] + 1;//版本 + 1 
	leaderName[SUBMIT] = nginxName;
	//vote for other
	for(auto fd : aliveNginxfd){
		string data;
		Vote v;
		v.set_version(version[SUBMIT]);
		v.set_nginxname(leaderName[SUBMIT]);
		v.SerializeToString(&data);
		if(!nginxs[fd].WriteProto(VoteNo, data)){
			nginxs[fd].Addfd2Write();
		}
		else{
			nginxs[fd].Addfd2Read();
			//LOG(DEBUG) << "VoteSend to " << nginxs[fd].clientName <<" fd = " << fd;
		}
	}
}
/* 如果变为了Leader 需要向其他 follower 发送ack */
void DtbtNginx::AckVote2FollowerSend(){
	//ackvote for other
	for(auto fd : aliveNginxfd){
		string data;
		AckVote2Follower avf;
		avf.set_version(version[ENSURE]);//版本已经升级过了
		avf.set_nginxname(nginxName);
		avf.SerializeToString(&data);
		if(!nginxs[fd].WriteProto(AckVote2FollowerNo, data)){
			nginxs[fd].Addfd2Write();
		}
		else{
			nginxs[fd].Addfd2Read();
		}
	}
	LOG(DEBUG) << "ACK to Follower";
}

void DtbtNginx::SynchDataSend(){

}

void DtbtNginx::AckData2FollowerSend(){

}
/* check and send keepAlive to all Nginx */
void DtbtNginx::SendKeepAlive(){
	int curTime = time(0);
	int fd;
	for(vector<int>::iterator it = aliveNginxfd.begin(); it != aliveNginxfd.end();){
		fd = *it;
		/* check keepAlive before check lastActive */
		if(nginxs[fd].activeInterval != -1 && curTime - nginxs[fd].lastActive >= nginxs[fd].activeInterval){
			/* check whether Leader */
			if(leaderName[ENSURE] == nginxs[fd].clientName){
				//清空Leader
				leaderName[ENSURE].clear();
				version[ENSURE] = 0;
				//需要重新启动投票定时器
				TimeHeapAddRaft();
			}
			it = aliveNginxfd.erase(it);
			LOG(DEBUG) << nginxs[fd].clientName << " 超时 curTime=" <<
			 curTime << " lastActive=" << nginxs[fd].lastActive << " activeInterval=" << nginxs[fd].activeInterval;
			nginxs[fd].CloseSocket();
		}
		else{
			/* check keepAlive */
			if(nginxs[fd].keepAliveInterval != -1 &&
						 curTime - nginxs[fd].lastKeepAlive >= nginxs[fd].keepAliveInterval){
				/* alive */
				string data;
				KeepAlive ka;
				ka.SerializeToString(&data);
				if(!nginxs[fd].WriteProto(KeepAliveNo, data)){
					nginxs[fd].Addfd2Write();
				}
				else{
					nginxs[fd].Addfd2Read();
				}
				nginxs[fd].lastKeepAlive = curTime;
				LOG(DEBUG) << "send keepAlive to " << nginxs[fd].clientName;
			}
			++it;
		}
	}
}

void DtbtNginx::TimeHeapAdd(size_t timeout){
	timeHeap.push_back(timeout);
	push_heap(timeHeap.begin(), timeHeap.end(), greater<int>());
}

void DtbtNginx::TimeHeapDel(){
	if(!timeHeap.empty()){
		pop_heap(timeHeap.begin(), timeHeap.end(), greater<int>()); 
		timeHeap.pop_back();
	}
}

int DtbtNginx::TimeHeapGet(){
	if(timeHeap.empty()){
		return -1;
	}
	return timeHeap.front();
}
//投票 raft定义在：150ms-300ms
void DtbtNginx::TimeHeapAddRaft(){
	int voteTime = rand() % raftVoteTime + raftVoteTime;
	TimeHeapAdd(voteTime);
}

/* 参数：
1.自己DtBtNginx编号 
2.配置文件位置
*/
int main(int argc, char **argv){
	if(argc != 2){
		LOG(ERROR) << "input 1.DtBtNginx编号";
		return 0;
	}
	DtbtNginx *dbNginx = Singleton<DtbtNginx>::getInstence();

	/* log init config files */
	el::Configurations conf("conf/log.conf");  
	el::Loggers::reconfigureAllLoggers(conf); 

	/* load DtbtNginx config files */
	dbNginx->ReadDtbtNginxConf(argv[1], "conf/DtbtNginx.conf");

	/* 创建父子进程共享的sockfd */
	int idx = dbNginx->lisSerName.find(' ');
	string lisSerIp(dbNginx->lisSerName, 0, idx);
	string lisSerPortStr(dbNginx->lisSerName, idx + 1, dbNginx->lisSerName.size());
	int lisSerPort = atoi(lisSerPortStr.c_str());
	dbNginx->lisSerfd = dbNginx->CreateListen(lisSerIp, lisSerPort);

	idx = dbNginx->lisCliName.find(' ');
	string lisCliIp(dbNginx->lisCliName, 0, idx);
	string lisCliPortStr(dbNginx->lisCliName, idx + 1, dbNginx->lisCliName.size());
	int lisCliPort = atoi(lisCliPortStr.c_str());
	dbNginx->lisClifd = dbNginx->CreateListen(lisCliIp, lisCliPort);
	
	/* 启动一个进程池  */
	Processpool *DtbiNginxProcesspool = Processpool::CreateProcesspool(1, true);//1.子进程个数
	if(DtbiNginxProcesspool) {
		DtbiNginxProcesspool->run<Nginx>();
        delete DtbiNginxProcesspool;
	}
	return 0;
}